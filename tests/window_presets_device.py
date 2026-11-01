"""No flash reads: live evening/morning presets, MQTT, persistence and real edges."""
import copy
import datetime as dt
import json
from pathlib import Path
import queue
import re
import sys
import time

root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'tools'),str(root/'.local/test-deps')]
from device_api import request
import paho.mqtt.client as mqtt
from jinja2 import Environment,StrictUndefined

def api(path,body=None,expected=200):
    for attempt in range(3):
        try:return request(path,body,expected)
        except OSError:
            if attempt==2 or path=='/api/restart':raise

original=api('/api/state');schedule=api('/api/schedule');profiles=api('/api/protection')['profiles_mv']
assert original['version']=='0.4.3'
base=original['topic'];identity=original['id'];rows=[];messages=queue.Queue();latest={};entities={}
secrets={k:json.loads(v) for k,v in re.findall(r'^#define (BOOTSTRAP_\w+) (.+)$',(root/'.local/bootstrap.h').read_text(),re.M)}
client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='ledpasek01-window-test')
client.username_pw_set(secrets['BOOTSTRAP_MQTT_USER'],secrets['BOOTSTRAP_MQTT_PASSWORD'])
def message(c,u,m):
    if m.topic.startswith(base+'/') or '/'+identity+'_' in m.topic or '/'+identity+'/' in m.topic:
        messages.put((m.topic,m.payload,m.retain))
client.on_message=message
client.connect(secrets['BOOTSTRAP_MQTT_HOST'],secrets['BOOTSTRAP_MQTT_PORT'],30)
client.subscribe([(base+'/#',1),('homeassistant/+/+/config',1)]);client.loop_start()
def pump(timeout=.1):
    try:topic,payload,retained=messages.get(timeout=timeout)
    except queue.Empty:return False
    try:value=json.loads(payload)
    except ValueError:value=payload.decode()
    latest[topic]=value
    if topic.startswith('homeassistant/'):entities[topic]=value
    return True
def wait(predicate,timeout=30):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if predicate():return
        pump()
    raise AssertionError('Timed out waiting for MQTT result')
def cmd(suffix,body,ok=True):
    while pump(.01):pass
    latest.pop(base+'/result',None)
    client.publish(base+'/'+suffix,json.dumps(body),qos=1,retain=False).wait_for_publish(timeout=8)
    wait(lambda:latest.get(base+'/result',{}).get('command')==suffix)
    result=latest[base+'/result'];assert result['ok']==ok,result
def record(label,value=True):
    rows.append(dict(label=label,value=value));print('PASS',label,json.dumps(value,ensure_ascii=False),flush=True)
def healthy(on=None,brightness=None,phase=None,effect=None):
    s=api('/api/state');p=s['protection']
    assert p['enabled'] and p['calibrated'] and not p['fault'] and not s['storage_error'],s
    if on is not None:assert s['output_active']==on and (s['state']=='ON')==on,s
    if brightness is not None:assert s['brightness']==brightness,s
    if phase:assert s['schedule']['phase']==phase,s
    if effect:assert s['effect']==effect,s
    return s
def fixed(a,b,enabled=True):
    return dict(enabled=enabled,overnight=True,start=dict(kind='fixed',minute=a%1440,offset=0),end=dict(kind='fixed',minute=b%1440,offset=0))

try:
    wait(lambda:len(entities)==47 and base+'/config' in latest and base+'/calibration' in latest)
    calibration=copy.deepcopy(latest[base+'/calibration'])
    env=Environment(undefined=StrictUndefined)
    for i,key in enumerate(['evening','morning']):
        for field,value in [('brightness','37'),('effect','first')]:
            e=next(v for k,v in entities.items() if k.endswith('_window_'+str(i)+'_'+field+'/config'))
            assert e['command_topic']==base+'/schedule/presets/set' and not e['retain']
            env.from_string(e['value_template']).render(value_json=latest[base+'/config'])
            assert json.loads(env.from_string(e['command_template']).render(value=value))=={key:{field:int(value) if field=='brightness' else value}}
    record('47 discovery entities including independent evening/morning brightness/effect; templates valid')
    assert schedule['window_presets']==dict(evening=original['on_preset'],morning=original['on_preset'])
    record('migration inherited the existing ON preset for both windows')
    cmd('schedule/set',dict(enabled=False,one_percent_off=True))
    desired=dict(evening=dict(brightness=37,effect='first'),morning=dict(brightness=23,effect='second'))
    cmd('schedule/presets/set',desired)
    cmd('schedule/presets/set',dict(evening=dict(effect='all')))
    cmd('schedule/presets/set',dict(evening=dict(effect='first')))
    wait(lambda:latest.get(base+'/config',{}).get('window_presets')==desired)
    for bad in [{},{'morning':None},{'morning':[]},{'evening':{'brightness':101}},{'evening':{'brightness':True}},{'morning':{'brightness':2.5}},{'evening':{'brightness':14},'morning':{'effect':'bad'}},{'night':{'brightness':2}}]:
        cmd('schedule/presets/set',bad,False)
    assert api('/api/schedule')['window_presets']==desired
    record('MQTT partial writes preserve the other window; invalid writes rejected atomically')
    cmd('schedule/night/set',dict(brightness=2,effect='all'))
    clock=healthy()['schedule']['local_time'];minute=int(clock[11:13])*60+int(clock[14:16])
    evening=[fixed(minute-30,minute+30),fixed(minute+60,minute+120)]
    cmd('schedule/set',dict(enabled=True,windows=evening));healthy(True,37,'evening','first')
    cmd('preset/set',dict(brightness=62,effect='crossfade'))
    cmd('schedule/action',dict(action='resume'));healthy(True,37,'evening','first')
    cmd('set',dict(state='ON'));s=healthy(True,62,effect='crossfade');assert s['schedule']['manual_override']
    cmd('set',dict(state='OFF'));assert healthy(False)['schedule']['manual_override']
    cmd('schedule/presets/set',dict(evening=dict(brightness=38)));healthy(False)
    cmd('schedule/set',dict(enabled=True));healthy(False)
    cmd('schedule/presets/set',desired)
    cmd('schedule/action',dict(action='resume'));healthy(True,37,'evening','first')
    for value,on in [(0,False),(1,False),(2,True),(37,True)]:
        cmd('schedule/presets/set',dict(evening=dict(brightness=value)))
        s=healthy(on,phase='evening');assert s['schedule']['scheduled_on']==on
        if on:assert s['brightness']==value
    cmd('schedule/presets/set',dict(evening=dict(brightness=1)))
    cmd('schedule/set',dict(one_percent_off=False));healthy(True,1,'evening','first')
    cmd('schedule/set',dict(one_percent_off=True));cmd('schedule/action',dict(action='resume'));healthy(False)
    cmd('schedule/presets/set',desired);healthy(True,37,'evening','first')
    record('evening preset, independent manual ON, manual OFF survives editing, 0/optional 1 percent physically OFF')
    # Both windows active: the morning preset wins; disabling it resumes evening.
    overlap=[fixed(minute-60,minute+60),fixed(minute-30,minute+30)]
    cmd('schedule/set',dict(windows=overlap));healthy(True,23,'morning','second')
    cmd('schedule/window/1/set',dict(enabled=False));healthy(True,37,'evening','first')
    cmd('schedule/window/1/set',dict(enabled=True));healthy(True,23,'morning','second')
    for value in [0,1]:
        cmd('schedule/presets/set',dict(morning=dict(brightness=value)));healthy(False,phase='morning')
    cmd('schedule/presets/set',desired);healthy(True,23,'morning','second')
    record('morning wins overlaps including zero brightness, disabled morning falls back to evening')
    cmd('schedule/set',dict(windows=evening));healthy(True,37,'evening','first')
    uptime=healthy()['uptime_ms'];api('/api/restart',{})
    until=time.monotonic()+50
    while time.monotonic()<until:
        time.sleep(2)
        try:
            s=api('/api/state')
            if s['uptime_ms']<uptime and s['mqtt'] and s['state']=='ON':break
        except OSError:pass
    else:raise AssertionError('Device did not return after restart')
    healthy(True,37,'evening','first')
    assert api('/api/schedule')['window_presets']==desired
    assert api('/api/state')['on_preset']==dict(brightness=62,effect='crossfade')
    record('independent evening/morning presets survive restart; automation resumes without controller')

    s=healthy();epoch=s['schedule']['epoch'];first=(epoch//60+1)*60
    if first-epoch<20:first+=60
    local=dt.datetime.strptime(s['schedule']['local_time'][:19],'%Y-%m-%d %H:%M:%S')+dt.timedelta(seconds=first-epoch)
    minute=local.hour*60+local.minute
    cmd('schedule/set',dict(windows=[fixed(minute-2,minute),fixed(minute+1,minute+2)]))
    healthy(True,37,'evening','first')
    cmd('set',dict(state='OFF'));s=healthy(False);assert s['schedule']['manual_override']
    print('Waiting for evening manual OFF -> night -> morning -> OFF boundaries from',local.isoformat(),flush=True)
    for phase,boundary,bright,effect,on in [('night',first,2,'all',True),('morning',first+60,23,'second',True),('off',first+120,23,'second',False)]:
        until=time.monotonic()+max(0,boundary-healthy()['schedule']['epoch'])+15
        while time.monotonic()<until:
            s=healthy()
            if s['schedule']['epoch']>=boundary and s['schedule']['phase']==phase and s['output_active']==on:break
            time.sleep(2)
        else:raise AssertionError('Missing actual boundary '+phase)
        s=healthy(on,bright,phase,effect);assert not s['schedule']['manual_override']
        record('actual phase transition '+phase,{k:s[k] for k in ['state','brightness','effect','output_active']})
    assert api('/api/protection')['profiles_mv']==profiles
    cmd('get',{});wait(lambda:latest[base+'/calibration']==calibration)
    record('original calibration preserved')
finally:
    api('/api/schedule',dict(enabled=False))
    api('/api/schedule/presets',schedule['window_presets'])
    api('/api/schedule/night',schedule['night_gap'])
    api('/api/preset',original['on_preset'])
    api('/api/schedule',schedule['config'])
    api('/api/light',{k:original[k] for k in ['state','brightness','effect','interval_ms']})
    if schedule['config']['enabled'] and not original['schedule']['manual_override']:api('/api/schedule/resume',{})
    rows.append(dict(label='Restored state',value=healthy()))
    client.loop_stop();client.disconnect()
    (root/'test-output/window-presets-0.4.3.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
