"""Real OTA/sensor calibration check. Never creates a flash backup or shorts LEDs.
Run: python tests/protection_device.py [--ota .pio/build/tdisplay/firmware.bin]
Fault or failed calibration leaves output OFF; success restores the requested light.
"""
import argparse
import json
from pathlib import Path
import re
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', default='http://192.0.2.48')
parser.add_argument('--ota', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
version = re.search(r'Version\[\] = "([0-9.]+)"', (root/'src/main.cpp').read_text(encoding='utf-8')).group(1)
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
records = []


def record(message, data=None):
    print(message, json.dumps(data, ensure_ascii=False) if data is not None else '', flush=True)
    records.append(dict(message=message, data=data))


def request(path, body=None, expected=200):
    headers = {'X-Requested-With': 'Svetylka'}
    if isinstance(body, dict):
        body = json.dumps(body).encode()
        headers['Content-Type'] = 'application/json'
    elif body is not None:
        headers['Content-Type'] = 'application/octet-stream'
    req = urllib.request.Request(args.host + path, body, headers)
    try:
        response = opener.open(req, timeout=90 if path == '/api/update' else 8)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        value = response.read()
        assert response.status == expected, (path, response.status, value[:300])
        return json.loads(value) if 'json' in response.headers.get('Content-Type', '') else value


def wait_online(previous_slot=None):
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        try:
            state = request('/api/state')
            if state['wifi'] and state['mqtt'] and (previous_slot is None or state['ota_slot'] != previous_slot):
                assert not state['ap_active'] and state['wifi_mode'] == 1
                return state
        except OSError:
            pass
        time.sleep(0.5)
    raise AssertionError('Device did not return online')


try:
    before = wait_online()
    assert before['id'] == 'svetylka-20489c6cdda0' and before['board'] == 'lilygo-t-display'
    original_light = {key: before[key] for key in ('state', 'brightness', 'effect', 'interval_ms')}
    old_calibration = request('/api/protection')
    if args.ota:
        blob = args.ota.read_bytes()
        assert blob[0] == 0xe9 and blob[3] >> 4 == 4 and version.encode()+b'\0' in blob
        record('Uploading OTA', len(blob))
        try:
            request('/api/update', blob)
        except OSError as error:
            record('OTA response interrupted; checking boot partition before any retry', str(error))
        time.sleep(3)
        state = wait_online(before['ota_slot'])
        assert state['version'] == version
        record('PASS: OTA, new partition, Wi-Fi and MQTT', state)
    else:
        state = before
    for path, name in [('/', 'index.html'), ('/app.js', 'app.js'), ('/style.css', 'style.css')]:
        assert request(path) == (root/'web'/name).read_bytes()
    p = state['protection']
    migrated = request('/api/protection')
    for key in ('reference_mv', 'zero_mv', 'reverse_mv', 'profiles_mv', 'delay_ms', 'threshold_percent'):
        assert migrated[key] == old_calibration[key], ('Calibration changed during OTA', key)
    assert p['latch_fault'] == old_calibration.get('latch_fault', False)
    record('PASS: calibration preserved; old firmware migrates to non-latching default')
    settings = {key: p[key] for key in ('enabled', 'latch_fault', 'delay_ms', 'threshold_percent')}
    for bad in ({'delay_ms': 99}, {'delay_ms': 10001}, {'enabled': 1}, {'enabled': False, 'unknown': 1}, {'threshold_percent': 96},
                {'latch_fault': 1}, {'latch_fault': 'false'}, {'latch_fault': None}, {'latch_fault': True, 'delay_ms': 99}):
        request('/api/protection', bad, expected=400)
    checked = request('/api/protection')
    assert {key: checked[key] for key in settings} == settings
    record('PASS: web assets and atomic input validation')
    if p['fault']:
        assert not state['output_active'] and state['state'] == 'OFF'
        request('/api/protection', {'latch_fault': True})
        request('/api/restart', {})
        time.sleep(3)
        latched = wait_online()
        assert latched['state'] == 'OFF' and latched['protection']['latched'] and latched['protection']['blocked']
        request('/api/light', {'state': 'ON'}, expected=400)
        request('/api/protection', {'enabled': False})
        request('/api/light', {'state': 'ON'}, expected=400)
        record('PASS: persisted fault blocks ON and cannot be bypassed by disabling protection', p['fault'])
        request('/api/protection', dict(settings, latch_fault=False))
        released = request('/api/state')
        assert released['state'] == 'OFF' and not released['output_active']
        assert released['protection']['fault'] and not released['protection']['latched'] and not released['protection']['blocked']
        acknowledged = request('/api/light', {'state': 'ON', 'brightness': 1})
        assert not acknowledged['protection']['fault'], acknowledged
        request('/api/light', {'state': 'OFF'})
        time.sleep(1)
        assert request('/api/state')['state'] == 'OFF'
        record('PASS: non-latching mode acknowledges existing trip by ordinary ON, without manual reset; no auto-restart')
    request('/api/protection/action', {'action': 'calibrate'})
    time.sleep(0.8)
    intermediate = request('/api/state')
    if intermediate['protection']['calibrating']:
        request('/api/light', {'state': 'ON'}, expected=400)
        request('/api/protection', {'delay_ms': 1000}, expected=409)
        request('/api/protection/action', {'action': 'cancel'})
        cancelled = request('/api/state')
        assert not cancelled['output_active'] and not cancelled['protection']['calibrating']
        record('PASS: calibration excludes competing commands; cancellation leaves OFF')
        request('/api/protection/action', {'action': 'calibrate'})
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        state = request('/api/state')
        p = state['protection']
        record('Calibration', {key: p[key] for key in ('progress', 'calibrating', 'fault', 'message')})
        if not p['calibrating']:
            break
        time.sleep(2)
    assert p['calibrated'] and not p['fault'], 'Physical sensor calibration failed; output remains OFF'
    profiles = request('/api/protection')['profiles_mv']
    assert len(profiles) == 3 and all(len(bank) == 100 for bank in profiles)
    record('PASS: real calibration completed with 300 samples', dict(reference_mv=p['reference_mv'], reverse_mv=p['reverse_mv'], threshold_mv=p['threshold_mv'], profiles_mv=profiles))
    for effect in ('first', 'second', 'all', 'alternate', 'crossfade'):
        for brightness in (1, 100):
            request('/api/light', dict(state='ON', effect=effect, brightness=brightness, interval_ms=1000))
            time.sleep(1.2)
            state = request('/api/state')
            assert not state['protection']['fault'] and state['output_active'], state
            assert state['protection']['sample_age_ms'] < 250
        record('PASS: real sensor at 1/100 % with protection', effect)
    request('/api/light', dict(original_light, state='OFF'))
    request('/api/protection', {'latch_fault': True})
    request('/api/restart', {})
    time.sleep(3)
    state = wait_online()
    p = state['protection']
    assert p['enabled'] and p['calibrated'] and not p['fault'] and p['latch_fault'] and not state['output_active']
    assert request('/api/protection')['profiles_mv'] == profiles
    request('/api/protection', {'latch_fault': False})
    request('/api/light', original_light)
    request('/api/restart', {})
    time.sleep(3)
    state = wait_online()
    assert not state['protection']['latch_fault'] and not state['protection']['fault']
    assert {key: state[key] for key in original_light} == original_light
    assert request('/api/protection')['profiles_mv'] == profiles
    assert not state['storage_error']
    record('PASS: both reaction settings and all 300 calibration values survive reboot; original light state restored', state)
except BaseException:
    try:
        request('/api/light', {'state': 'OFF'})
    except OSError:
        pass
    raise
finally:
    (root/'test-output').mkdir(exist_ok=True)
    (root/'test-output'/('protection-device-'+version+'-'+time.strftime('%Y%m%d-%H%M%S')+'.json')).write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
