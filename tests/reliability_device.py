"""Controlled recovery tests. Requires the temporary RELIABILITY_TEST OTA build.
No flash read, no Wi-Fi credential changes, no router changes. Restores light.
"""
import json
from pathlib import Path
import queue
import re
import socket
import sys
import time

root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'tools'),str(root/'.local/test-deps')]
from device_api import request
import paho.mqtt.client as mqtt

before=json.loads((root/'test-output/before-0.5.1.json').read_text(encoding='utf-8'))
initial=request('/api/state')
assert initial['version']=='0.5.1-test',initial['version']
base=initial['topic'];messages=queue.Queue();rows=[]
network_only='--network-only' in sys.argv
schedule_check=network_only or '--schedule' in sys.argv
secrets={k:json.loads(v) for k,v in re.findall(r'^#define (BOOTSTRAP_\w+) (.+)$',(root/'.local/bootstrap.h').read_text(),re.M)}
client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='ledpasek01-recovery-test')
client.username_pw_set(secrets['BOOTSTRAP_MQTT_USER'],secrets['BOOTSTRAP_MQTT_PASSWORD'])
client.on_message=lambda c,u,m:messages.put((m.topic,m.payload,m.retain))
client.connect(secrets['BOOTSTRAP_MQTT_HOST'],secrets['BOOTSTRAP_MQTT_PORT'],30)
client.subscribe(base+'/#',1);client.loop_start()

def record(label,**values):
    row=dict(label=label,**values);rows.append(row)
    (root/'test-output'/('reliability-network-0.5.1.json' if network_only else 'reliability-0.5.1.json')).write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
    print('PASS',label,json.dumps(values,ensure_ascii=False),flush=True)

def wait_state(predicate,timeout=100):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        try:topic,body,retained=messages.get(timeout=1)
        except queue.Empty:continue
        if topic!=base+'/state' or retained:continue
        state=json.loads(body)
        if predicate(state):return state
    raise AssertionError('No matching fresh MQTT state')

def healthy(s):
    assert s['wifi'] and s['mqtt'] and not s['storage_error'],s
    p=s['protection'];assert p['enabled'] and p['calibrated'] and not p['fault'],p
    assert s['dhcp_hostname']=='ledpasek01'
    assert s['health']['control_age_ms']<500
    return s

def inject(body):
    # A disruption may close its own HTTP response; inject only once.
    try:request('/api/test',body)
    except OSError:pass

def command(body):
    while not messages.empty():messages.get_nowait()
    client.publish(base+'/set',json.dumps(body),qos=1,retain=False).wait_for_publish(8)
    return wait_state(lambda s: all(s.get(k)==v for k,v in body.items()),30)

try:
    healthy(initial)
    command(dict(state='ON',brightness=10,effect='all'))
    start=request('/api/state');boot=start['health']['boot_count']
    inject(dict(offline_ms=45000))
    s=healthy(wait_state(lambda s:s['uptime_ms']>start['uptime_ms']+43000 and s['wifi'] and s['mqtt']))
    assert s['health']['boot_count']==boot and not s['ap_active'],s
    assert s['health']['fallback_starts']>start['health']['fallback_starts']
    assert s['output_active'] and s['brightness']==10 and s['schedule']['manual_override']
    record('45 s Wi-Fi loss: fallback starts, automatic reconnect, no MCU reset or light-state loss',health=s['health'])

    for kind,key in [('station','wifi_station_resets'),('radio','wifi_radio_resets')]:
        start=request('/api/state');inject(dict(network=kind))
        s=healthy(wait_state(lambda s:s['health'][key]>start['health'][key] and s['wifi'] and s['mqtt'] and s['uptime_ms']>start['uptime_ms']+5000))
        assert s['health']['boot_count']==boot and s['output_active'] and not s['ap_active']
        record(kind+' recovery returns Wi-Fi, MQTT, DHCP name and HTTP without reset',health=s['health'])
        assert request('/api/state')['wifi']

    # Slow/incomplete JSON must release HTTP within its deadline and must never
    # hold the control mutex. MQTT remains available throughout the bad request.
    start=request('/api/state');conn=socket.create_connection(('192.0.2.48',80),timeout=5)
    conn.sendall(b'POST /api/light HTTP/1.1\r\nHost: 192.0.2.48\r\nX-Requested-With: Svetylka\r\nContent-Type: application/json\r\nContent-Length: 2048\r\nConnection: close\r\n\r\n{')
    until=time.monotonic()+18
    while time.monotonic()<until:
        try:conn.sendall(b' ')
        except OSError:break
        time.sleep(1)
    conn.close()
    s=healthy(request('/api/state'));assert s['health']['boot_count']==boot and s['output_active']
    record('slow incomplete HTTP POST times out without stopping PWM/protection or resetting',health=s['health'])

    for task in (['http'] if network_only else ['control','http','loop','network','mqtt']):
        if task=='loop':command(dict(state='OFF'))
        start=request('/api/state');boot=start['health']['boot_count'];at=time.monotonic()
        inject(dict(stall=task))
        s=healthy(wait_state(lambda s:s['health']['boot_count']>boot and s['wifi'] and s['mqtt'],110))
        expected='control_stall' if task=='control' else 'http_stall' if task=='http' else 'none'
        assert s['health']['last_recovery']==expected,s['health']
        if task in ['loop','network','mqtt']:assert s['health']['reset_reason']=='task_watchdog',s['health']
        assert s['health']['resume_restored'],s['health']
        assert s['state']==start['state'] and s['brightness']==start['brightness'] and s['effect']==start['effect'],dict(expected=start,actual=s)
        assert s['schedule']['manual_override']==start['schedule']['manual_override'],dict(expected=start['schedule'],actual=s['schedule'])
        assert s['schedule']['override_until']==start['schedule']['override_until'],dict(expected=start['schedule'],actual=s['schedule'])
        record(task+' stall recovers automatically; manual state and protection preserved',seconds=round(time.monotonic()-at,1),health=s['health'])

    if schedule_check:
        # Real minute boundary while network maintenance is deliberately stuck.
        # MQTT + PWM + scheduler must keep running until the hardware watchdog.
        import datetime as dt
        s=request('/api/state');assert s['schedule']['time_valid']
        local=dt.datetime.strptime(s['schedule']['local_time'][:19],'%Y-%m-%d %H:%M:%S')
        if local.second>50:time.sleep(65-local.second)
        s=request('/api/state');local=dt.datetime.strptime(s['schedule']['local_time'][:19],'%Y-%m-%d %H:%M:%S')
        minute=(local.hour*60+local.minute+1)%1440
        window=dict(enabled=True,overnight=True,start=dict(kind='fixed',minute=minute,offset=0),end=dict(kind='fixed',minute=(minute+2)%1440,offset=0))
        disabled=dict(window,enabled=False)
        request('/api/schedule/presets',dict(evening=dict(brightness=17,effect='all')))
        request('/api/schedule',dict(enabled=True,windows=[window,disabled]))
        request('/api/schedule/resume',{})
        start=request('/api/state');assert start['state']=='OFF'
        inject(dict(stall='network'))
        s=healthy(wait_state(lambda s:s['health']['boot_count']==start['health']['boot_count'] and s['health']['network_age_ms']>1000 and s['output_active'] and s['brightness']==17,70))
        record('real schedule boundary switches ON while network maintenance is stalled',state=s['state'],phase=s['schedule']['phase'],network_age_ms=s['health']['network_age_ms'])
        healthy(wait_state(lambda s:s['health']['boot_count']>start['health']['boot_count'] and s['wifi'] and s['mqtt'],100))
        request('/api/schedule',before['schedule']['config'])
        request('/api/schedule/presets',before['schedule']['window_presets'])

    command(dict(state='OFF'))
    scope=request('/api/scope');assert scope['high_a']==scope['high_b']==0,scope
    assert request('/api/protection')['profiles_mv']==before['protection']['profiles_mv']
    plan=request('/api/schedule')
    for key in ['config','night_gap','window_presets']:assert plan[key]==before['schedule'][key],key
    record('physical PWM OFF after recovery; original calibration and schedule preserved')
finally:
    try:
        if schedule_check:
            request('/api/schedule',before['schedule']['config'])
            request('/api/schedule/presets',before['schedule']['window_presets'])
        s=before['state'];request('/api/light',{k:s[k] for k in ['state','brightness','effect','interval_ms']})
        if not s['schedule']['manual_override']:request('/api/schedule/resume',{})
        record('original light state restored',state=request('/api/state')['state'])
    finally:client.disconnect();client.loop_stop()
