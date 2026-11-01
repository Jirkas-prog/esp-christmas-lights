"""Passive on-device PWM regression; only OTA, no flash backup or physical short."""
import json
from pathlib import Path
import sys
import time

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from device_api import request

def api(path,body=None,expected=200):
    for attempt in range(3):
        try:return request(path,body,expected)
        except OSError:
            if attempt==2:raise

before=api('/api/state');baseline=json.loads((root/'test-output/before-0.5.2.json').read_text(encoding='utf-8'))
assert before['version'].startswith('0.5.2')
profiles=api('/api/protection')['profiles_mv'];records=[]
original=baseline['/api/state'];config=original['protection'];is_test=before['version'].endswith('-test')

def record(label,data):
    records.append(dict(label=label,data=data));print(label,json.dumps(data,ensure_ascii=False),flush=True)

def healthy(check_samples=True):
    s=api('/api/state');p=s['protection']
    assert not p['fault'] and p['enabled'] and p['calibrated'] and not s['storage_error'],s
    assert s['health']['boot_count']==before['health']['boot_count'],s
    assert p['sampling']=='passive_pwm'
    if check_samples and p['monitoring_required']:assert p['sample_age_ms']<500 and p['samples']>=100,p
    return s

try:
    if '--effects-only' not in sys.argv and '--low-only' not in sys.argv:
        api('/api/light',dict(state='ON',effect='all',brightness=100))
        time.sleep(.6);initial=healthy()
        # Reads, real NVS changes and repeated idempotent settings must not turn the
        # bridge off or rewrite a constant output. Includes two PCNT counter wraps.
        deadline=time.monotonic()+65;reads=0
        while time.monotonic()<deadline:
            api('/api/protection',{'delay_ms':config['delay_ms']+(100 if reads%2 else 0)})
            time.sleep(.3) # A real setting change starts a fresh 200 ms ADC window.
            s=healthy();q=s['pwm'];reads+=1
            assert q['bridge_switches']==initial['pwm']['bridge_switches'],q
            assert q['writes']==initial['pwm']['writes'],q
            assert q['edge_errors']==initial['pwm']['edge_errors'],q
            if is_test and reads%5==0:
                result=api('/api/test',{'lock_ms':200})
                assert result['frames']>=30 and result['writes']==0 and result['bridge_switches']==0,result
            time.sleep(.4)
        record('constant PWM under HTTP, NVS and application-lock load',dict(readings=reads,before=initial['pwm'],after=s['pwm']))
        api('/api/protection',{'delay_ms':config['delay_ms']})
        for mode in ('first','second','all'):
            for brightness in (2,20,100):
                api('/api/light',dict(state='ON',effect=mode,brightness=brightness));time.sleep(.65)
                s=healthy();scope=api('/api/scope');q=s['pwm']
                assert scope['pwm_frames']>=6,scope
                if mode=='first':assert scope['high_b']==0 and scope['high_a']>0,scope
                if mode=='second':assert scope['high_a']==0 and scope['high_b']>0,scope
                record(mode+' '+str(brightness),dict(pwm=q,pads=scope['pad_samples'],a=scope['high_a'],b=scope['high_b'],samples=s['protection']['samples'],pwm_frames_during_scope=scope['pwm_frames']))
    if '--low-only' not in sys.argv:
        for mode in ('alternate','crossfade','soft_crossfade','breathe','tide','candlelight'):
            api('/api/light',dict(state='ON',effect=mode,brightness=100,interval_ms=500));time.sleep(.7)
            start=healthy();deadline=time.monotonic()+max(3,start['effect_cycle_ms']/1000+1)
            samples=[];locked_writes=0
            while time.monotonic()<deadline:
                s=healthy();samples.append(s['protection']['samples'])
                if is_test and mode in ('crossfade','soft_crossfade','breathe'):
                    result=api('/api/test',{'lock_ms':200})
                    assert result['frames']>=30 and result['bridge_switches']==0,result
                    locked_writes+=result['writes']
                    record(mode+' independent during 200 ms lock',result)
                time.sleep(.2)
            assert s['pwm']['writes']>start['pwm']['writes'] and not s['protection']['fault']
            if is_test and mode in ('crossfade','soft_crossfade','breathe'):assert locked_writes>=20,locked_writes
            record(mode+' full cycle',dict(min_samples=min(samples),max_samples=max(samples),pwm=s['pwm']))
    for mode in ('soft_crossfade','breathe','tide','candlelight'):
        for brightness in (2,9,20):
            api('/api/light',dict(state='ON',effect=mode,brightness=brightness,interval_ms=500));time.sleep(.65)
            start=healthy(False);deadline=time.monotonic()+start['effect_cycle_ms']/1000+.5
            frames=[];required_at=None
            while time.monotonic()<deadline:
                s=healthy(False);p=s['protection'];frames.append(p)
                # Re-entering a monitored phase starts a new 200 ms ADC window;
                # an old sample during that warm-up is expected, not a lost task.
                if not p['monitoring_required']:required_at=None
                elif required_at is None:required_at=time.monotonic()
                elif time.monotonic()-required_at>.5:assert p['sample_age_ms']<500 and p['samples']>=100,p
                time.sleep(.15)
            assert s['state']=='ON' and not s['protection']['fault']
            record(mode+' low '+str(brightness),dict(readings=len(frames),monitored=sum(p['monitoring_required'] for p in frames),max_pending_ms=max(p['pending_ms'] for p in frames)))
    api('/api/light',{'brightness':1});time.sleep(.2)
    off=healthy();scope=api('/api/scope')
    assert off['state']=='OFF' and not off['output_active'] and scope['high_a']==scope['high_b']==0
    api('/api/light',{'state':'ON'});time.sleep(.4);healthy()
    assert api('/api/protection')['profiles_mv']==profiles
    record('1 percent physical OFF, preset ON and original calibration',True)
finally:
    api('/api/protection',{'delay_ms':config['delay_ms']})
    api('/api/light',{k:original[k] for k in ('state','brightness','effect','interval_ms')})
    if original['schedule']['enabled'] and not original['schedule']['manual_override']:
        api('/api/schedule/resume',{})
    (root/'test-output'/('pwm-isolation-'+before['version']+('-low' if '--low-only' in sys.argv else '')+'.json')).write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
