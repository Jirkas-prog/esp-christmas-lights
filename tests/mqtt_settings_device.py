"""MQTT settings and manual/automatic coexistence, on the actual OTA device.
Requires paho-mqtt and Jinja2 in .local/test-deps. Never reads flash or changes wiring.
"""
import copy
import json
from pathlib import Path
import queue
import re
import sys
import time

root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'tools'),str(root/'.local/test-deps')]
from device_api import request as http
import paho.mqtt.client as mqtt
from jinja2 import Environment,StrictUndefined

base='wifi2mqtt/ledpasek01'
identity='svetylka-20489c6cdda0'
secrets={k:json.loads(v) for k,v in re.findall(r'^#define (BOOTSTRAP_\w+) (.+)$',(root/'.local/bootstrap.h').read_text(),re.M)}
messages=queue.Queue();latest={};retained=set();entities={};rows=[]
client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='ledpasek01-settings-test')
client.username_pw_set(secrets['BOOTSTRAP_MQTT_USER'],secrets['BOOTSTRAP_MQTT_PASSWORD'])
def on_message(c,u,m):
    if m.topic.startswith(base+'/') or ('/'+identity+'_' in m.topic or '/'+identity+'/' in m.topic):
        messages.put((m.topic,m.payload,m.retain))
client.on_message=on_message
client.connect(secrets['BOOTSTRAP_MQTT_HOST'],secrets['BOOTSTRAP_MQTT_PORT'],30)
client.subscribe([(base+'/#',1),('homeassistant/+/+/config',1)])
client.loop_start()

def pump(timeout=.1):
    try:topic,payload,retain=messages.get(timeout=timeout)
    except queue.Empty:return None
    try:value=json.loads(payload)
    except (ValueError,TypeError):value=payload.decode(errors='replace')
    if retain:retained.add(topic)
    latest[topic]=value
    if topic.startswith('homeassistant/'):entities[topic]=value
    return topic,value
def wait(predicate,timeout=25):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if predicate():return
        pump(.15)
    raise AssertionError('MQTT timeout waiting for expected state')
def drain():
    while pump(.02) is not None:pass
def cmd(suffix,body,ok=True):
    drain();latest.pop(base+'/result',None)
    payload=json.dumps(body) if isinstance(body,dict) else body
    client.publish(base+'/'+suffix,payload,qos=1,retain=False).wait_for_publish(timeout=8)
    wait(lambda:latest.get(base+'/result',{}).get('command')==suffix)
    result=latest[base+'/result'];assert result['ok']==ok,result
    return result
def state():
    return latest.get(base+'/state',{})
def config():
    return latest.get(base+'/config',{})
def record(label,value=True):
    rows.append(dict(label=label,value=value));print('PASS',label,json.dumps(value,ensure_ascii=False),flush=True)
def fixed(a,b,enabled=True,overnight=False):
    return dict(enabled=enabled,overnight=overnight,start=dict(kind='fixed',minute=a,offset=0),end=dict(kind='fixed',minute=b,offset=0))

saved=None;original=None
try:
    wait(lambda:all(base+'/'+s in latest for s in ['state','config','calibration']) and len(entities)==41,45)
    assert state()['version']=='0.4.1'
    assert all(base+'/'+s in retained for s in ['state','config','calibration'])
    original=copy.deepcopy(state());saved=copy.deepcopy(config());cal=copy.deepcopy(latest[base+'/calibration'])
    assert len(cal['profiles_mv'])==3 and len(cal['profiles_mv'][0])==100
    assert len(cal['pwm']['references_mv'])==2 and len(cal['pwm']['references_mv'][0])==104
    serialized=json.dumps([config(),cal])
    for key in ['BOOTSTRAP_MQTT_PASSWORD','BOOTSTRAP_WIFI_PASSWORD']:
        assert not secrets.get(key) or secrets[key] not in serialized
    assert 'wifi_password' not in config()['network'] and 'mqtt_password' not in config()['network']
    record('retained settings and complete calibration; no passwords',dict(entities=len(entities),profile_points=300,pwm_points=208))

    # Render all discovery templates with a strict Jinja engine, including time
    # conversion and Czech select labels; then use their output on the real board.
    env=Environment(undefined=StrictUndefined)
    for topic,e in entities.items():
        if e.get('schema')=='json':
            assert not e['flash'] and not e['transition']
            continue
        domain=topic.split('/')[1]
        if 'value_template' in e:
            env.from_string(e['value_template']).render(value_json=latest[e['state_topic']])
        if 'command_template' in e:
            sample=e['options'][0] if domain=='select' else '05:45:00' if domain=='time' else str(e['min'])
            assert isinstance(json.loads(env.from_string(e['command_template']).render(value=sample)),dict)
        if domain=='number':assert e['step']>=.001
        if 'command_topic' in e:assert not e['retain']
        assert e['device']['identifiers']==[identity]
    record('all 41 discovery messages and command/state templates')

    cmd('set',dict(state='OFF',brightness=23,effect='all'))
    cmd('schedule/set',dict(enabled=True,windows=[fixed(0,0,overnight=True),fixed(0,1,False)]))
    wait(lambda:state().get('output_active') and state()['schedule']['enabled'])
    cmd('set',dict(state='OFF'))
    wait(lambda:state().get('state')=='OFF' and state()['schedule']['manual_override'])
    cmd('schedule/set',dict(enabled=True))
    cmd('schedule/window/0/set',dict(start=dict(offset=-15)))
    time.sleep(2);cmd('get',{})
    wait(lambda:config()['schedule']['windows'][0]['start']['offset']==-15)
    wait(lambda:state()['state']=='OFF' and state()['schedule']['manual_override'])
    cmd('schedule/action',dict(action='resume'));wait(lambda:state()['output_active'])
    cmd('schedule/set',dict(enabled=False));wait(lambda:not state()['schedule']['enabled'])
    assert state()['output_active']
    cmd('set',dict(state='OFF'));wait(lambda:not state()['output_active'])
    record('manual OFF survives repeated enable and edits; disabling schedule leaves current light alone')

    # Outside the active window, both manual ON and OFF still work.
    clock=state()['schedule']['local_time'][11:16];minute=int(clock[:2])*60+int(clock[3:])
    begin=(minute+120)%1440;end=(minute+180)%1440
    cmd('schedule/set',dict(enabled=True,windows=[fixed(begin,end,overnight=True),fixed(0,1,False)]))
    cmd('set',dict(state='ON'));wait(lambda:state()['output_active'] and state()['schedule']['manual_override'])
    assert not state()['schedule']['scheduled_on']
    cmd('set',dict(state='OFF'));wait(lambda:not state()['output_active'])
    cmd('schedule/set',dict(enabled=False));wait(lambda:not state()['schedule']['enabled'])
    record('manual ON/OFF also works outside the automatic interval')

    for topic,e in entities.items():
        if topic.endswith('_window_1_start_time/config'):
            payload=env.from_string(e['command_template']).render(value='05:45:00')
            cmd('schedule/window/1/set',payload)
    wait(lambda:config()['schedule']['windows'][1]['start']['minute']==345)
    # Synthetic update beside the public Greenwich reference point.
    cmd('schedule/set',dict(timezone='UTC',latitude=51.4789,longitude=0.001,one_percent_off=True))
    wait(lambda:config()['schedule']['timezone']=='UTC')
    cmd('set',dict(state='ON',brightness=1));wait(lambda:state()['brightness']==1 and state()['state']=='OFF')
    assert not state()['output_active'] and not state()['protection']['pwm_a'] and not state()['protection']['pwm_b']
    cmd('set',dict(state='ON'));wait(lambda:state()['state']=='ON' and state()['brightness']==100 and state()['effect']=='all')
    cmd('set',dict(brightness=1));wait(lambda:state()['state']=='OFF' and state()['brightness']==1)
    cmd('set',dict(brightness=2));wait(lambda:state()['state']=='ON' and state()['brightness']==2)
    cmd('set',dict(brightness=0));wait(lambda:state()['state']=='OFF' and state()['brightness']==0)
    cmd('preset/set',dict(brightness=37,effect='second'));wait(lambda:config()['on_preset']==dict(brightness=37,effect='second'))
    assert state()['state']=='OFF'
    cmd('set',dict(state='ON'));wait(lambda:state()['state']=='ON' and state()['brightness']==37 and state()['effect']=='second')
    # Both HTTP and MQTT share the exact intent processing, including HA's combined command.
    s=http('/api/light',dict(state='ON',brightness=1));assert s['state']=='OFF' and not s['output_active']
    s=http('/api/light',dict(brightness=2));assert s['state']=='ON' and s['brightness']==2 and s['effect']=='second'
    s=http('/api/light',dict(state='ON',brightness=17,effect='first'));assert s['state']=='ON' and s['brightness']==17 and s['effect']=='first'
    cmd('set',dict(state='ON',brightness=18));wait(lambda:state()['state']=='ON' and state()['brightness']==18 and state()['effect']=='first')
    cmd('set',dict(state='ON',effect='second'));wait(lambda:state()['effect']=='second' and state()['brightness']==18)
    s=http('/api/light',dict(state='OFF',brightness=65));assert s['state']=='OFF' and not s['output_active']
    cmd('set',dict(brightness=1));wait(lambda:state()['state']=='OFF' and state()['brightness']==1)
    scope=http('/api/scope');assert scope['pad_samples']>0 and scope['duty_a']==scope['duty_b']==scope['high_a']==scope['high_b']==0
    record('passive GPIO trace after 1 percent OFF',{k:scope[k] for k in ['pad_samples','duty_a','duty_b','high_a','high_b']})
    for bad in [dict(brightness=1),dict(brightness=101),dict(effect='wrong'),dict(brightness=30,unknown=1)]:cmd('preset/set',bad,False)
    cmd('get',{});wait(lambda:config()['on_preset']==dict(brightness=37,effect='second'))
    record('1 percent physical OFF, 2 percent slider ON, default/custom preset ON, explicit fields, HTTP parity and preset validation')
    before=copy.deepcopy(config()['schedule'])
    for suffix,body in [('schedule/set',{'latitude':91}),('schedule/window/0/set',{'start':{'offset':721}}),
                        ('schedule/window/1/set',{'end':{'minute':1440}}),('schedule/set',{'enabled':1}),
                        ('network/set',{'mqtt_port':0}),('protection/set',{'delay_ms':99})]:
        cmd(suffix,body,False)
    cmd('schedule/set','{"enabled":true,"unknown":0}',False)
    cmd('schedule/set','{broken',False)
    cmd('schedule/set',b'{"enabled":true}\x00',False)
    cmd('get',{});drain();assert config()['schedule']==before
    record('time/coordinates/1 percent settings and atomic rejection')

    cmd('protection/set',dict(enabled=False,latch_fault=True,delay_ms=2400,threshold_percent=81))
    wait(lambda:config()['protection']['threshold_percent']==81)
    cmd('protection/set',dict(enabled=True));wait(lambda:state()['protection']['enabled'])
    cmd('protection/limits/set',dict(first=6));wait(lambda:config()['protection']['limits']['first']==6)
    assert config()['protection']['limits']['second']==saved['protection']['limits']['second']
    cmd('protection/limits/set',dict(all=45));wait(lambda:state()['protection']['thermal_warning'])
    cmd('protection/limits/set',dict(automatic=True));wait(lambda:config()['protection']['limits']['first']==cal['quality']['first']['automatic'])
    cmd('protection/set',{k:saved['protection'][k] for k in ['enabled','latch_fault','delay_ms','threshold_percent']})
    cmd('protection/limits/set',saved['protection']['limits'])
    record('protection, latch, delay, threshold, individual limits and warning above 2 W')

    cmd('protection/action',dict(action='calibrate'))
    wait(lambda:state()['protection']['calibrating'])
    cmd('schedule/set',dict(enabled=True),False)
    cmd('protection/action',dict(action='cancel'))
    wait(lambda:not state()['protection']['calibrating'])
    cmd('protection/action',dict(action='clear'))
    cmd('get',{});drain();assert latest[base+'/calibration']==cal
    record('start/cancel calibration over MQTT preserves existing calibration')

    cmd('alias/set',dict(alias=saved['alias']))
    cmd('network/set',dict(mqtt_port=saved['network']['mqtt_port']))
    cmd('schedule/set',saved['schedule'])
    cmd('set',{k:original[k] for k in ['state','brightness','effect','interval_ms']})
    time.sleep(2.5)
    uptime=state()['uptime_ms'];result=cmd('system/action',dict(action='restart'))
    assert result['restart_pending']
    wait(lambda:state().get('uptime_ms',uptime)<uptime and state().get('mqtt'),60)
    cmd('get',{});wait(lambda:config()['schedule']==saved['schedule'])
    assert latest[base+'/calibration']==cal
    assert state()['protection']['calibrated'] and not state()['protection']['fault']
    assert not state()['storage_error'] and not state()['ap_active']
    wait(lambda:config()['on_preset']==dict(brightness=37,effect='second'))
    cmd('set',dict(state='ON'));wait(lambda:state()['brightness']==37 and state()['effect']=='second' and state()['state']=='ON')
    cmd('preset/set',saved['on_preset']);wait(lambda:config()['on_preset']==saved['on_preset'])
    cmd('set',{k:original[k] for k in ['state','brightness','effect','interval_ms']})
    record('MQTT restart, persistence, original calibration and light restored')
    (root/'test-output/mqtt-discovery-0.4.1.json').write_text(json.dumps(entities,ensure_ascii=False,indent=2),encoding='utf-8')
finally:
    if saved and original:
        # Always leave the original settings; HTTP is the independent recovery path.
        if http('/api/state')['protection']['calibrating']:http('/api/protection/action',dict(action='cancel'))
        for path,body in [('/api/preset',saved['on_preset']),('/api/schedule',saved['schedule']),('/api/protection',{k:saved['protection'][k] for k in ['enabled','latch_fault','delay_ms','threshold_percent']}),
                          ('/api/protection/limits',saved['protection']['limits']),('/api/light',{k:original[k] for k in ['state','brightness','effect','interval_ms']})]:
            for attempt in range(3):
                try:http(path,body);break
                except OSError:
                    if attempt==2:raise
        final=http('/api/state');rows.append(dict(label='Final restored state',value=final))
    client.loop_stop();client.disconnect()
    (root/'test-output/mqtt-settings-0.4.1.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
