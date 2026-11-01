"""Live regression for noisy ADC, manual cutoffs and retained calibration. OTA only."""
import json
from pathlib import Path
import sys
import time

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from device_api import request

rows=[]
before=request('/api/state')
assert before['version']=='0.3.4' and before['protection']['calibrated']
original={k:before[k] for k in ('state','effect','brightness','interval_ms')}
limits=before['protection']['limits']
saved_limits={k:limits[k]['ignored'] for k in ('first','second','all')}

def record(label,value):
    rows.append(dict(label=label,value=value))
    print(label,json.dumps(value,ensure_ascii=False),flush=True)

try:
    request('/api/light',{'state':'OFF'})
    request('/api/protection/limits',{'first':85,'second':90,'all':45})
    p=request('/api/state')['protection']
    assert p['thermal_warning'] and p['ignored_short_power_mw']>2000
    record('PASS above-2W override and warning',p['limits'])
    request('/api/protection/limits',{'first':101,'second':0,'all':0},expected=400)
    assert request('/api/state')['protection']['limits']==p['limits']
    request('/api/protection/limits',{'automatic':True})
    assert all(request('/api/state')['protection']['limits'][k]['ignored']==limits[k]['automatic'] for k in saved_limits)
    for mode in ('all','first','second','alternate','crossfade'):
        levels=range(1,21) if mode=='all' else (1,2,3,5,10,15,19,20,40,80,100)
        for brightness in levels:
            request('/api/light',dict(state='ON',effect=mode,brightness=brightness,interval_ms=100))
            captures=[];start=time.monotonic()
            while time.monotonic()-start<3.5:
                time.sleep(.25)
                state=request('/api/state');p=state['protection'];captures.append(p)
                assert state['output_active'] and not p['fault'],state
                assert p['enabled'] and p['calibrated'] and not state['storage_error']
                if time.monotonic()-start>1 and p['monitoring_required']:
                    assert p['samples']>=100 and p['sample_age_ms']<450,state
                if not p['monitoring_required']:
                    assert not p['pending_ms'],state
            record(mode+' '+str(brightness),dict(cutoff=p['ignored_brightness'],monitored=p['monitoring_required'],
                max_pending=max(c['pending_ms'] for c in captures),
                gains=[round(p['gain_a'],3),round(p['gain_b'],3)],
                error=round(p['fit_error_mv'],1),noise=round(p['adc_noise_mv'],1)))
            if mode in ('first','second') and brightness in (1,20,100):
                scope=request('/api/scope')
                assert scope['high_b' if mode=='first' else 'high_a']==0,scope
                record('Inactive GPIO '+mode+' '+str(brightness),dict(samples=scope['pad_samples'],inactive_high=0))
    request('/api/light',{'state':'OFF'})
    profiles=request('/api/protection')['profiles_mv']
    request('/api/protection/limits',saved_limits)
    request('/api/restart',{})
    time.sleep(5)
    state=request('/api/state');p=state['protection']
    assert p['calibrated'] and not p['fault'] and request('/api/protection')['profiles_mv']==profiles
    assert {k:p['limits'][k]['ignored'] for k in saved_limits}==saved_limits
    assert state['wifi'] and state['mqtt'] and not state['ap_active'] and not state['storage_error']
    request('/api/light',original)
    record('PASS persistence and restore',request('/api/state'))
except BaseException:
    request('/api/light',{'state':'OFF'})
    raise
finally:
    (root/'test-output'/('low-brightness-'+time.strftime('%Y%m%d-%H%M%S')+'.json')).write_text(
        json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf8')
