"""OTA and passive PWM checks. Does not short the hardware or back up flash."""
import argparse
import json
from pathlib import Path
import sys
import time
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from device_api import request,upload

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ota',action='store_true')
args=parser.parse_args()
records=[]
def record(label,state):
    records.append(dict(label=label,state=state))
    print(label,json.dumps(state,ensure_ascii=False),flush=True)

before=request('/api/state')
original={k:before[k] for k in ('state','brightness','effect','interval_ms')}
try:
    if args.ota:
        state=upload(root/'.pio/build/tdisplay/firmware.bin','0.3.4')
        record('OTA',state)
    for path,name in (('/','index.html'),('/app.js','app.js'),('/style.css','style.css')):
        assert request(path)==(root/'web'/name).read_bytes(), 'Deployed assets differ from source'
    request('/api/protection/action',{'action':'calibrate'})
    deadline=time.monotonic()+240
    while time.monotonic()<deadline:
        time.sleep(2)
        p=request('/api/state')['protection']
        record('Calibration',{k:p[k] for k in ('progress','calibrating','fault','message')})
        if not p['calibrating']:break
    assert p['calibrated'] and not p['fault'],p
    references=request('/api/protection')['profiles_mv']
    for mode in ('first','second','all','alternate','crossfade'):
        for brightness in (1,2,10,50,100):
            request('/api/light',dict(state='ON',effect=mode,brightness=brightness,interval_ms=250))
            time.sleep(.8)
            samples=[]
            for i in range(3):
                state=request('/api/state');p=state['protection'];samples.append(p)
                assert state['output_active'] and not p['fault'],state
                if p['monitoring_required']: assert p['samples']>=100,state
                assert (not p['monitoring_required'] or p['sample_age_ms']<400) and p['sampling']=='passive_pwm',state
                if mode=='second': assert p['pwm_a']==0
                if mode=='first': assert p['pwm_b']==0
                time.sleep(.15)
            record(mode+' '+str(brightness),[{k:p[k] for k in ('gain_a','gain_b','fit_error_mv','samples','pwm_a','pwm_b','pending_ms')} for p in samples])
            if mode in ('first','second') and brightness in (1,100):
                captures=[request('/api/scope') for _ in range(3)]
                active='high_a' if mode=='first' else 'high_b'
                inactive='high_b' if mode=='first' else 'high_a'
                assert sum(c[active] for c in captures)>0,captures
                assert sum(c[inactive] for c in captures)==0,captures
                record('PASS: actual GPIO levels '+mode+' '+str(brightness),
                       {'samples':sum(c['pad_samples'] for c in captures),'active_high':sum(c[active] for c in captures),'inactive_high':0})
    request('/api/light',{'state':'OFF'})
    off=request('/api/state')['protection']['sample_age_ms'];time.sleep(.4)
    state=request('/api/state');p=state['protection']
    assert not state['output_active'] and p['pwm_a']==p['pwm_b']==0 and p['sample_age_ms']>off+300
    request('/api/restart',{})
    time.sleep(4)
    state=request('/api/state');p=state['protection']
    assert p['calibrated'] and not p['fault'] and not p['latch_fault']
    assert request('/api/protection')['profiles_mv']==references
    request('/api/light',original)
    time.sleep(.4)
    state=request('/api/state')
    assert not state['protection']['fault'] and not state['storage_error'] and state['mqtt'] and not state['ap_active']
    record('PASS: passive PWM, no opposite-bank duty, OFF, persistence, original state',state)
except BaseException:
    try: request('/api/light',{'state':'OFF'})
    except OSError: pass
    raise
finally:
    (root/'test-output'/('pwm-device-'+time.strftime('%Y%m%d-%H%M%S')+'.json')).write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
