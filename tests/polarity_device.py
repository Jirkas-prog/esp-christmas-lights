"""Opt-in live checks of saved polarity, MQTT discovery and unchanged calibration.
Requires LIGHT_HOST/LIGHT_DEVICE_ID and private bootstrap credentials. Leaves OFF.
"""
import json,queue,re,sys,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'tools'),str(root/'.local/test-deps')]
from device_api import request,expected_id
import paho.mqtt.client as mqtt
from jinja2 import Environment,StrictUndefined
before=request('/api/state');identity=before['id'];base=before['topic']
assert identity==expected_id and before['protection']['calibrated'] and not before['protection']['calibrating']
output=before['protection']['sense_output'];profiles=request('/api/protection')['profiles_mv']
secrets={k:json.loads(v) for k,v in re.findall(r'^#define (BOOTSTRAP_\w+) (.+)$',(root/'.local/bootstrap.h').read_text(),re.M)}
client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='single-point-verification')
client.username_pw_set(secrets['BOOTSTRAP_MQTT_USER'],secrets['BOOTSTRAP_MQTT_PASSWORD'])
messages=queue.Queue();latest={}
client.on_message=lambda c,u,m:messages.put((m.topic,m.payload))
client.connect(secrets['BOOTSTRAP_MQTT_HOST'],secrets['BOOTSTRAP_MQTT_PORT'],30)
client.subscribe([(base+'/#',1),('homeassistant/select/'+identity+'_sense_output/config',1)])
client.loop_start()
def wait(condition,timeout=35):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if condition():return
        try:topic,payload=messages.get(timeout=.2)
        except queue.Empty:continue
        try:latest[topic]=json.loads(payload)
        except ValueError:latest[topic]=payload.decode()
    raise AssertionError('MQTT timeout')
def command(body,okay):
    latest.pop(base+'/result',None)
    client.publish(base+'/protection/polarity/set',json.dumps(body),qos=1,retain=False).wait_for_publish(8)
    wait(lambda:latest.get(base+'/result',{}).get('command')=='protection/polarity/set')
    assert latest[base+'/result']['ok']==okay,latest[base+'/result']
try:
    topic='homeassistant/select/'+identity+'_sense_output/config'
    wait(lambda:topic in latest and base+'/config' in latest and base+'/calibration' in latest)
    cal=latest[base+'/calibration']['pwm']
    entity=latest[topic];assert entity['options']==['AOUT1','AOUT2'] and not entity['retain']
    env=Environment(undefined=StrictUndefined)
    assert env.from_string(entity['value_template']).render(value_json=latest[base+'/config'])==output
    for value in entity['options']:
        assert json.loads(env.from_string(entity['command_template']).render(value=value))=={'output':value}
    request('/api/light',{'state':'OFF'})
    command({'output':'invalid'},False)
    command({'output':output},True)
    assert request('/api/protection')['profiles_mv']==profiles
    # Exercise restart over MQTT; verify the saved sensor orientation and full RC calibration.
    latest.pop(base+'/calibration',None)
    client.publish(base+'/system/action','{"action":"restart"}',qos=1,retain=False).wait_for_publish(8)
    wait(lambda:latest.get(base+'/state',{}).get('health',{}).get('boot_count',0)>before['health']['boot_count'],60)
    wait(lambda:base+'/calibration' in latest)
    assert latest[base+'/calibration']['pwm']==cal
    after=request('/api/state')
    assert after['protection']['sense_output']==output and after['protection']['calibrated']
    assert not after['protection']['fault'] and after['wifi'] and after['mqtt'] and after['state']=='OFF'
    assert request('/api/protection')['profiles_mv']==profiles
    print('PASS: HA polarity selector/templates, MQTT invalid/idempotent commands, restart and saved orientation/calibration')
finally:
    client.disconnect();client.loop_stop()
    request('/api/light',{'state':'OFF'})
