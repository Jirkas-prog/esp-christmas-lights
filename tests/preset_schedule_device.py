"""Actual minute boundaries: 1% OFF -> scheduled preset ON -> automatic OFF."""
import datetime
import json
from pathlib import Path
import sys
import time

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from device_api import request

def api(path,body=None):
    for attempt in range(3):
        try:return request(path,body)
        except OSError:
            if attempt==2:raise

saved=api('/api/state');schedule=api('/api/schedule')['config'];rows=[]
assert saved['version']=='0.4.1' and saved['schedule']['time_valid']
def point(minute):return dict(kind='fixed',minute=minute,offset=0)
try:
    api('/api/schedule',dict(enabled=False,one_percent_off=True))
    api('/api/preset',dict(brightness=37,effect='second'))
    api('/api/light',dict(brightness=1,effect='first'))
    s=api('/api/state');epoch=s['schedule']['epoch']
    start=(epoch//60+1)*60
    if start-epoch<15:start+=60
    local=datetime.datetime.strptime(s['schedule']['local_time'][:19],'%Y-%m-%d %H:%M:%S')+datetime.timedelta(seconds=start-epoch)
    minute=local.hour*60+local.minute
    window=dict(enabled=True,overnight=True,start=point(minute),end=point((minute+1)%1440))
    disabled=dict(enabled=False,overnight=False,start=point(0),end=point(1))
    api('/api/schedule',dict(enabled=True,windows=[window,disabled]))
    api('/api/light',dict(brightness=1))
    initial=api('/api/state');assert initial['state']=='OFF' and initial['schedule']['manual_override']
    print('Waiting for actual ON boundary',local.isoformat(),flush=True)
    for label,boundary,on in [('scheduled preset ON',start,True),('scheduled OFF',start+60,False)]:
        deadline=time.monotonic()+max(0,boundary-api('/api/state')['schedule']['epoch'])+15
        while time.monotonic()<deadline:
            s=api('/api/state')
            assert not s['protection']['fault'] and not s['storage_error'],s
            if s['schedule']['epoch']>=boundary and (s['state']=='ON')==on:break
            time.sleep(2)
        else:raise AssertionError(label+' did not happen')
        assert not s['schedule']['manual_override']
        assert s['brightness']==37 and s['effect']=='second'
        assert s['output_active']==on
        rows.append(dict(label=label,state=s));print('PASS',label,s['schedule']['local_time'],flush=True)
        if on:
            scope=api('/api/scope');assert scope['high_a']==0 and scope['high_b']>0
            rows.append(dict(label='inactive polarity stays LOW',scope=scope))
finally:
    api('/api/schedule',dict(enabled=False))
    api('/api/preset',saved['on_preset'])
    api('/api/schedule',schedule)
    api('/api/light',{k:saved[k] for k in ['state','brightness','effect','interval_ms']})
    rows.append(dict(label='Restored final state',state=api('/api/state')))
    (root/'test-output/preset-schedule-0.4.1.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
