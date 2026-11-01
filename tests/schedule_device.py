"""Live OTA feature verification. No flash read, clock changes, or physical short."""
import copy
import json
from pathlib import Path
import queue
import re
import sys
import time

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
sys.path.insert(0, str(root / '.local/test-deps'))
from device_api import request as device_request
import paho.mqtt.client as mqtt

rows = []
def request(path, body=None, expected=200):
    # A lost HTTP reply does not imply a failed write. Only repeat idempotent
    # settings/light commands and reads; never blindly repeat restart/OTA.
    for attempt in range(3):
        try: return device_request(path,body,expected)
        except OSError:
            if attempt==2 or path=='/api/restart': raise
            print('Retry after lost HTTP response:',path,flush=True)
            time.sleep(.5)

def record(label, value=True):
    rows.append(dict(label=label, value=value))
    print(label, json.dumps(value, ensure_ascii=False), flush=True)

def healthy(on=None):
    s = request('/api/state')
    p = s['protection']
    assert p['enabled'] and p['calibrated'] and not p['fault'] and not s['storage_error'], s
    assert s['wifi'] and s['mqtt'] and not s['ap_active'], s
    if on is not None: assert s['output_active'] == on, s
    return s

def fixed(start, end, enabled=True, overnight=False):
    return dict(enabled=enabled, overnight=overnight,
                start=dict(kind='fixed', minute=start % 1440, offset=0),
                end=dict(kind='fixed', minute=end % 1440, offset=0))

original = healthy()
assert original['version'] == '0.4.0'
config = request('/api/schedule')['config']
profiles = request('/api/protection')['profiles_mv']
client = None
try:
    deadline = time.monotonic() + 90
    while not healthy()['schedule']['synced'] and time.monotonic() < deadline:
        time.sleep(2)
    s = healthy()['schedule']
    assert s['synced'] and abs(s['epoch'] - time.time()) < 10, s
    record('PASS SNTP actual UTC and Prague clock', s)
    # Invalid writes must not partially alter the stored schedule.
    for body in [dict(latitude=91), dict(longitude=-181), dict(timezone='bad'),
                 dict(enabled=1), dict(windows=[]), dict(one_percent_off='true')]:
        request('/api/schedule', body, expected=400)
        assert request('/api/schedule')['config'] == config
    request('/api/schedule?date=2026-02-30', expected=400)
    record('PASS atomic validation')
    # Test libc timezone handling on the device: full local days have 23/25 h.
    request('/api/schedule', dict(enabled=False, windows=[fixed(0,0,overnight=True),fixed(0,1,False)]))
    for date, hours in [('2026-03-29',23),('2026-10-25',25),('2026-10-22',24)]:
        day = request('/api/schedule?date='+date)['day']
        w = day['windows'][0]
        assert w['end_epoch']-w['start_epoch'] == hours*3600, day
        record('PASS DST '+date, w)
    request('/api/schedule', config)
    before = request('/api/schedule?date=2026-10-22')['day']
    windows = copy.deepcopy(config['windows'])
    windows[0]['start']['offset'] = -30
    windows[1]['end']['offset'] = 20
    request('/api/schedule',dict(windows=windows))
    after = request('/api/schedule?date=2026-10-22')['day']
    assert after['windows'][0]['start_epoch']==before['sunset_epoch']-1800
    assert after['windows'][1]['end_epoch']==before['sunrise_epoch']+1200
    request('/api/schedule',dict(latitude=89))
    polar = request('/api/schedule?date=2026-06-21')['day']
    assert not polar['sunrise_epoch'] and not polar['sunset_epoch']
    assert not polar['windows'][0]['start_epoch']
    request('/api/schedule', config)
    # Morning window cannot accidentally run into the next day when sunrise is earlier.
    request('/api/schedule',dict(timezone='UTC'))
    summer = request('/api/schedule?date=2026-06-21')['day']
    assert not summer['windows'][1]['start_epoch'], summer
    request('/api/schedule', config)
    record('PASS solar offsets, polar events and early sunrise')

    request('/api/schedule',dict(enabled=False,one_percent_off=True))
    request('/api/light',dict(state='ON',brightness=1))
    assert healthy(False)['state']=='OFF'
    request('/api/light',dict(state='ON',brightness=2,effect='second'))
    time.sleep(.6);healthy(True)
    scope=request('/api/scope')
    assert scope['high_a']==0 and scope['duty_a']==0 and scope['duty_b']>0,scope
    request('/api/schedule',dict(one_percent_off=False))
    request('/api/light',dict(state='ON',brightness=1));time.sleep(.6);healthy(True)
    request('/api/schedule',dict(one_percent_off=True));healthy(False)
    record('PASS 1 percent OFF option and inactive bank stays LOW')

    secrets={k:json.loads(v) for k,v in re.findall(r'^#define (BOOTSTRAP_\w+) (.+)$',(root/'.local/bootstrap.h').read_text(),re.M)}
    messages=queue.Queue()
    client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='ledpasek01-schedule-test')
    client.username_pw_set(secrets['BOOTSTRAP_MQTT_USER'],secrets['BOOTSTRAP_MQTT_PASSWORD'])
    client.on_message=lambda c,u,m:messages.put(json.loads(m.payload))
    client.connect(secrets['BOOTSTRAP_MQTT_HOST'],secrets['BOOTSTRAP_MQTT_PORT'],30)
    client.subscribe('wifi2mqtt/ledpasek01/state');client.loop_start()
    assert messages.get(timeout=15)['version']=='0.4.0'
    while not messages.empty():messages.get_nowait()
    client.publish('wifi2mqtt/ledpasek01/set',json.dumps(dict(state='ON',brightness=1)),retain=False).wait_for_publish()
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        ack=messages.get(timeout=10)
        if ack['brightness']==1 and ack['state']=='OFF':break
    else:raise AssertionError('No MQTT OFF acknowledgment')
    assert ack['schedule']['one_percent_off'];healthy(False)
    record('PASS MQTT 1 percent OFF and complete telemetry')

    # Exercise a real scheduled minute boundary without changing the ESP's clock.
    request('/api/light',dict(state='ON',brightness=23,effect='all',interval_ms=1000))
    t=healthy()['schedule']['local_time'][11:16]
    minute=int(t[:2])*60+int(t[3:])
    # A short OFF gap starts at the next minute; second window then turns ON.
    request('/api/schedule',dict(enabled=True,one_percent_off=True,
        windows=[fixed(minute,minute+1,overnight=True),fixed(minute+1,minute+2,overnight=True)]))
    healthy(True)
    # Adjacent windows form one interval; manual OFF lasts through the shared boundary.
    request('/api/light',dict(state='OFF'));time.sleep(1.2)
    assert healthy(False)['schedule']['manual_override']
    request('/api/schedule/resume',{});healthy(True)
    # Future interval only: start must switch output on and clear a manual override.
    request('/api/schedule',dict(windows=[fixed(minute+1,minute+3,overnight=True),fixed(0,1,False)]))
    healthy(False)
    request('/api/light',dict(state='OFF'))
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        s=healthy()
        if s['output_active']:break
        time.sleep(.7)
    else:raise AssertionError('Scheduled ON boundary missed')
    assert not s['schedule']['manual_override'],s
    record('PASS live scheduled ON, manual override and resume',s['schedule'])
    # An interval ending at the next minute verifies scheduled OFF as well.
    now=s['schedule']['local_time'][11:16];minute=int(now[:2])*60+int(now[3:])
    request('/api/schedule',dict(windows=[fixed(minute,minute+1,overnight=True),fixed(0,1,False)]))
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        s=healthy()
        if not s['output_active']:break
        time.sleep(.7)
    else:raise AssertionError('Scheduled OFF boundary missed')
    record('PASS live scheduled OFF',s['schedule'])

    # Persist a non-default option, and restart by HTTP only.
    request('/api/schedule',dict(**{**config,'one_percent_off':True}))
    request('/api/light',dict(state='OFF',brightness=1))
    time.sleep(2.5);request('/api/restart',{});time.sleep(5)
    s=healthy(False)
    assert s['schedule']['one_percent_off']
    assert request('/api/protection')['profiles_mv']==profiles
    assert s['protection']['limits']==original['protection']['limits']
    record('PASS persistence, restart, calibration and limits preserved')
finally:
    if client is not None:client.loop_stop();client.disconnect()
    request('/api/schedule',config)
    request('/api/light',{k:original[k] for k in ['state','brightness','effect','interval_ms']})
    time.sleep(2.5)
    final=healthy(original['output_active'])
    rows.append(dict(label='Final restored state',value=final))
    (root/'test-output/schedule-0.4.0.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
    print('Restored original settings and light state.',flush=True)
