"""Opt-in live check of the A-point sensor. Select LIGHT_HOST/LIGHT_DEVICE_ID.
Use --calibrate to run the wizard first. No physical short is applied.
"""
import json
from pathlib import Path
import sys
import time
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from device_api import request
before=request('/api/state')
assert before['sense_topology']=='single_point_a'
records=[]
try:
    request('/api/light',{'state':'OFF'})
    request('/api/schedule',{'enabled':False})
    if '--calibrate' in sys.argv:
        request('/api/protection/action',{'action':'calibrate'})
        deadline=time.monotonic()+900
        while True:
            time.sleep(5)
            p=request('/api/state')['protection']
            print('Calibration',p['progress'],p['message'],flush=True)
            if not p['calibrating']:
                assert p['calibrated'] and not p['fault'],p
                break
            if time.monotonic()>deadline:raise TimeoutError('Calibration did not finish')
    assert request('/api/state')['protection']['calibrated']
    for mode in ('first','second','all'):
        for brightness in (2,5,10,20,50,100):
            request('/api/light',{'state':'ON','effect':mode,'brightness':brightness})
            time.sleep(.8)
            s=request('/api/state');p=s['protection'];scope=request('/api/scope')
            assert not p['fault'] and not p['pending_ms'] and not s['storage_error'],p
            assert s['health']['boot_count']==before['health']['boot_count']
            assert p['sampling']=='passive_pwm'
            if mode=='first':assert scope['high_b']==0,scope
            if mode=='second':assert scope['high_a']==0,scope
            if p['monitoring_required']:
                assert p['sample_age_ms']<500 and p['samples']>=100,p
                if mode!='second':assert .8<=p['gain_a']<=1.2,p
                if mode!='first':assert .8<=p['gain_b']<=1.2,p
            records.append({'mode':mode,'brightness':brightness,'protection':p,'pwm':s['pwm']})
            print(mode,brightness,'PASS',flush=True)
    initial=request('/api/state');deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        s=request('/api/state')
        assert not s['protection']['fault']
        for key in ('writes','bridge_switches','edge_errors'):
            assert s['pwm'][key]==initial['pwm'][key],s['pwm']
        time.sleep(.15)
    print('Constant PWM under HTTP load: PASS',flush=True)
finally:
    # Keep the selected board OFF even if a measurement or network request fails.
    request('/api/light',{'state':'OFF'})
    if request('/api/state')['protection']['calibrating']:
        request('/api/protection/action',{'action':'cancel'})
    request('/api/schedule',{'enabled':before['schedule']['enabled']})
    request('/api/light',{'state':'OFF'})
    out=root/'test-output';out.mkdir(exist_ok=True)
    (out/'single-point-device.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
