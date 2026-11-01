"""No flash reads: bilingual effects, MQTT restart, DHCP hostname and passive protection."""
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
assert original['version']=='0.5.0'
base=original['topic'];identity=original['id'];rows=[];messages=queue.Queue();latest={};entities={}
secrets={k:json.loads(v) for k,v in re.findall(r'^#define (BOOTSTRAP_\w+) (.+)$',(root/'.local/bootstrap.h').read_text(),re.M)}
client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='ledpasek01-effects-test')
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

labels=dict(all='All (Celý řetěz)',first='First Half (První polovina)',second='Second Half (Druhá polovina)',alternate='Alternate (Střídání)',crossfade='Linear Crossfade (Lineární prolínání)',soft_crossfade='Soft Crossfade (Jemné prolínání)',breathe='Breathe (Dýchání)',tide='Tide (Přelévání)',candlelight='Candlelight (Svíčka)')
try:
    wait(lambda:len(entities)==47 and base+'/config' in latest and base+'/light/state' in latest)
    light_entity=entities['homeassistant/light/'+identity+'/config']
    assert light_entity['effect_list']==list(labels.values())
    assert light_entity['state_topic']==base+'/light/state'
    env=Environment(undefined=StrictUndefined)
    for name in ['preset_effect','night_effect','window_0_effect','window_1_effect']:
        e=next(v for k,v in entities.items() if k.endswith('_'+name+'/config'))
        assert e['options']==list(labels.values())
        assert env.from_string(e['value_template']).render(value_json=latest[base+'/config']) in labels.values()
        for label in labels.values():assert json.loads(env.from_string(e['command_template']).render(value=label))
    restart=next(v for k,v in entities.items() if k.endswith('_restart/config'))
    assert restart['command_topic']==base+'/system/action' and restart['device_class']=='restart'
    assert restart['enabled_by_default'] and not restart['retain']
    assert json.loads(restart['payload_press'])=={'action':'restart'}
    assert original['dhcp_hostname']==original['alias']=='ledpasek01'
    record('47 discovery entities; all 9 bilingual options and templates; DHCP stack hostname ledpasek01')
    cmd('schedule/set',dict(enabled=False))
    cmd('set',dict(state='OFF'))
    presets=dict(evening=dict(brightness=35,effect=labels['candlelight']),morning=dict(brightness=23,effect=labels['tide']))
    cmd('schedule/presets/set',presets)
    cmd('preset/set',dict(brightness=28,effect=labels['soft_crossfade']))
    cmd('schedule/night/set',dict(brightness=2,effect=labels['breathe']))
    for mode,label in labels.items():
        cmd('set',dict(state='OFF',effect=label))
        state=healthy(False,effect=mode);assert state['effect_name']==label
        wait(lambda:latest.get(base+'/light/state',{}).get('effect')==label)
        assert latest[base+'/state']['effect']==mode
    record('bilingual commands round-trip through HA light state; technical state keeps legacy codes')
    before=healthy();cmd('system/action',{'action':'restart'})
    latest.pop(base+'/state',None);latest.pop(base+'/availability',None)
    # The restart itself and its proof use only MQTT. No HTTP request is needed.
    wait(lambda:latest.get(base+'/state',{}).get('uptime_ms',10**12)<before['uptime_ms'] and latest.get(base+'/availability')=='online',60)
    state=healthy(False);assert state['dhcp_hostname']=='ledpasek01'
    data=api('/api/schedule');assert data['window_presets']['evening']['effect']=='candlelight' and data['window_presets']['morning']['effect']=='tide'
    assert data['night_gap']['effect']=='breathe' and state['on_preset']['effect']=='soft_crossfade'
    record('restart from discovered MQTT button; reconnect and new presets persist; actual DHCP hostname retained')
    # A safe single-bank cutoff must not be reused at twice its thermal power.
    saved_limits={k:v['ignored'] for k,v in original['protection']['limits'].items()}
    cmd('protection/limits/set',dict(first=79,second=79,all=5))
    for mode in ['breathe','tide','candlelight']:
        cmd('set',dict(state='ON',brightness=100,effect=mode,interval_ms=1000))
        p=healthy(True)['protection']
        assert p['monitoring_required'] and p['ignored_brightness']==5 and not p['thermal_warning'],p
    cmd('protection/limits/set',saved_limits)
    record('combined effects use combined cutoff; single-bank 79 percent cannot silently exempt both banks')
    for mode in (['tide'] if '--cutoff-only' in sys.argv else ['soft_crossfade','breathe','tide','candlelight']):
        for brightness in ([9] if '--cutoff-only' in sys.argv else [2,8,9,20,100] if mode=='tide' else [2,8,20,100]):
            cmd('set',dict(state='ON',brightness=brightness,effect=labels[mode],interval_ms=1000))
            state=healthy(True,brightness,effect=mode);duration=state['effect_cycle_ms']/1000+2
            frames=[];until=time.monotonic()+duration
            while time.monotonic()<until:
                state=healthy(True,brightness,effect=mode);p=state['protection']
                assert p['pwm_a']<=brightness*5 and p['pwm_b']<=brightness*5
                frames.append(dict(a=p['pwm_a'],b=p['pwm_b'],required=p['monitoring_required'],samples=p['samples'],pending=p['pending_ms'],power_mw=p['short_power_mw']))
                time.sleep(.25)
            if brightness>=20:
                assert any(f['required'] and f['samples']>=100 for f in frames),frames
                assert len({(f['a'],f['b']) for f in frames})>2,frames
            if brightness==2:assert not any(f['required'] for f in frames)
            record(mode+' '+str(brightness)+' percent',dict(cycle_ms=state['effect_cycle_ms'],readings=len(frames),min_a=min(f['a'] for f in frames),max_a=max(f['a'] for f in frames),min_b=min(f['b'] for f in frames),max_b=max(f['b'] for f in frames),monitored=sum(f['required'] for f in frames),max_pending_ms=max(f['pending'] for f in frames)))
    # The legacy commands and real 1%-OFF policy continue to work.
    cmd('set',dict(brightness=1));healthy(False)
    cmd('set',dict(state='ON'));healthy(True,28,effect='soft_crossfade')
    cmd('set',dict(brightness=2));healthy(True,2,effect='soft_crossfade')
    cmd('set',dict(state='OFF'));healthy(False)
    assert api('/api/protection')['profiles_mv']==profiles
    record('legacy slider/ON/OFF behavior and calibration preserved')
finally:
    api('/api/protection/limits',{k:v['ignored'] for k,v in original['protection']['limits'].items()})
    api('/api/schedule',dict(enabled=False))
    api('/api/schedule/presets',schedule['window_presets'])
    api('/api/schedule/night',schedule['night_gap'])
    api('/api/preset',original['on_preset'])
    api('/api/schedule',schedule['config'])
    api('/api/light',{k:original[k] for k in ['state','brightness','effect','interval_ms']})
    if schedule['config']['enabled'] and not original['schedule']['manual_override']:api('/api/schedule/resume',{})
    rows.append(dict(label='Restored state',value=healthy()))
    client.loop_stop();client.disconnect()
    output='cutoff-0.5.0.json' if '--cutoff-only' in sys.argv else 'effects-0.5.0.json'
    (root/'test-output'/output).write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
