"""Opt-in passive protection regression during effect/brightness changes."""
import json,sys,time
from pathlib import Path
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
from device_api import request
before=request('/api/state');rows=[]
assert before['protection']['calibrated'] and not before['protection']['calibrating']
try:
    request('/api/light',{'state':'OFF'});request('/api/schedule',{'enabled':False})
    for interval in (100,3000):
        for mode in ('alternate','crossfade','soft_crossfade','breathe','tide','candlelight'):
            for brightness in (10,50,100):
                request('/api/light',{'state':'ON','effect':mode,'brightness':brightness,'interval_ms':interval})
                end=time.monotonic()+4;samples=[]
                while time.monotonic()<end:
                    time.sleep(.15);s=request('/api/state');p=s['protection'];samples.append(p)
                    assert not p['fault'] and not s['storage_error'],(mode,brightness,interval,p)
                    assert s['health']['boot_count']==before['health']['boot_count']
                rows.append(dict(mode=mode,brightness=brightness,interval=interval,samples=samples))
                print(mode,brightness,interval,'PASS',flush=True)
finally:
    request('/api/light',{'state':'OFF'})
    request('/api/light',{'effect':before['effect'],'brightness':before['brightness'],'interval_ms':before['interval_ms'],'state':'OFF'})
    request('/api/schedule',{'enabled':before['schedule']['enabled']});request('/api/light',{'state':'OFF'})
    (root/'test-output/single-point-effects.json').write_text(json.dumps(rows,indent=2)+'\n',encoding='utf-8')
