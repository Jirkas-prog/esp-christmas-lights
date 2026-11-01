"""Live HTTP/MQTT/restart/optional OTA check. Changes only this light, restores its state.

Install test-only dependency: pip install --target .local/test-deps paho-mqtt==2.1.0
Run: python tests/device.py --host http://DEVICE_IP [-e esp32wroom32] [--ota PATH/firmware.bin]
Reads local bootstrap credentials; never dumps credentials or device flash.
"""
import argparse
import json
from pathlib import Path
import queue
import re
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / '.local/test-deps'))
import paho.mqtt.client as mqtt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', required=True)
parser.add_argument('--ota', type=Path)
parser.add_argument('--alias', default='ledpasek01')
parser.add_argument('--check-alias-change', action='store_true')
parser.add_argument('-e', '--environment', choices=('tdisplay', 'esp32wroom32'), default='tdisplay')
args = parser.parse_args()
secrets = {key: json.loads(value) for key, value in re.findall(
    r'^#define (BOOTSTRAP_\w+) (.+)$', (root / '.local/bootstrap.h').read_text(), re.M)}
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def request(path, body=None, expected=200, csrf=True):
    headers = {}
    if csrf:
        headers['X-Requested-With'] = 'Svetylka'
    if isinstance(body, dict):
        headers['Content-Type'] = 'application/json'
        body = json.dumps(body).encode()
    elif body is not None:
        headers['Content-Type'] = 'application/octet-stream'
    req = urllib.request.Request(args.host.rstrip('/') + path, body, headers)
    try:
        response = opener.open(req, timeout=30)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        assert response.status == expected, (path, response.status, data[:160])
        assert 'WWW-Authenticate' not in response.headers, 'Web must not request a password'
        return json.loads(data) if 'json' in response.headers.get('Content-Type', '') else data


def light_fields(state):
    return {key: state[key] for key in ('state', 'brightness', 'effect', 'interval_ms')}


def wait_output_active():
    # Protection verifies a fresh pulse before the first normal output after OFF.
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        state = request('/api/state')
        if state['output_active']:
            return state
        time.sleep(0.05)
    raise AssertionError('Output remained blocked; inspect protection/calibration')


def wait_online():
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        try:
            state = request('/api/state')
            if state['wifi'] and state['mqtt']:
                assert state['wifi_mode'] == 1 and not state['ap_active'], 'AP must be disabled while connected'
                return state
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.5)
    raise AssertionError('Device did not reconnect within 45 seconds')


state = wait_online()
original = light_fields(state)
assert not state['storage_error']
expected_board, expected_flash_mb = ('lilygo-t-display', 16) if args.environment == 'tdisplay' else ('esp32-wroom-32', 4)
assert state['board'] == expected_board and state['flash_bytes'] == expected_flash_mb * 1024 * 1024
base = state['topic']
assert state['alias'] == state['hostname'] == state['ap_ssid'] == args.alias
assert base == 'wifi2mqtt/' + args.alias
settings = request('/api/settings')
assert settings['alias'] == args.alias
request('/api/alias', {'alias': 'Invalid Name'}, expected=400)
assert request('/api/settings') == settings
assert settings['ssid'] == secrets['BOOTSTRAP_SSID']
assert settings['mqtt_host'] == secrets['BOOTSTRAP_MQTT_HOST']
assert settings['mqtt_password_set'] and settings['wifi_password_set']
assert not any(key in settings for key in ('wifi_password', 'mqtt_password'))
request('/api/light', {'brightness': 1}, expected=403, csrf=False)
for route, filename in [('/', 'index.html'), ('/app.js', 'app.js'), ('/style.css', 'style.css')]:
    assert request(route) == (root / 'web' / filename).read_bytes(), filename
print('PASS: station-only Wi-Fi, passwordless web/API, POST header check, embedded web assets', flush=True)

inbox = queue.Queue()
received = {}
connected = threading.Event()
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='svetylka-test-' + uuid.uuid4().hex[:12])
client.username_pw_set(secrets['BOOTSTRAP_MQTT_USER'], secrets['BOOTSTRAP_MQTT_PASSWORD'])
client.on_connect = lambda c, u, f, reason, p: connected.set() if not reason.is_failure else None
def on_message(c, u, msg):
    received[msg.topic] = msg.payload
    inbox.put((msg.topic, msg.payload))
client.on_message = on_message
client.connect(secrets['BOOTSTRAP_MQTT_HOST'], secrets['BOOTSTRAP_MQTT_PORT'], 30)
client.loop_start()


def wait_message(topic, predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        received, payload = inbox.get(timeout=max(0.01, deadline - time.monotonic()))
        if received == topic and predicate(payload):
            return payload
    raise AssertionError('No expected MQTT message: ' + topic)


try:
    assert connected.wait(10), 'MQTT test client failed to connect'
    discovery = 'homeassistant/light/' + state['id'] + '/config'
    client.subscribe(discovery, 1)
    doc = json.loads(wait_message(discovery, lambda p: True))
    assert doc['command_topic'] == base + '/set'
    assert doc['name'] == doc['device']['name'] == args.alias
    assert doc['unique_id'] == state['id'] and doc['device']['identifiers'] == [state['id']]
    assert set(doc['effect_list']) == {'all', 'first', 'second', 'alternate', 'crossfade'}
    client.subscribe(base + '/availability', 1)
    wait_message(base + '/availability', lambda p: p == b'online')
    client.subscribe(base + '/state', 1)
    wait_message(base + '/state', lambda p: json.loads(p)['id'] == state['id'])
    print('PASS: retained availability/state and Home Assistant discovery', flush=True)

    for effect in doc['effect_list']:
        command = dict(state='ON', brightness=20, effect=effect, interval_ms=750)
        assert light_fields(request('/api/light', command)) == command
    assert not request('/api/light', {'state': 'OFF'})['output_active']
    assert not request('/api/light', {'state': 'ON', 'brightness': 0})['output_active']
    request('/api/light', {'brightness': 20})
    wait_output_active()
    before = light_fields(request('/api/state'))
    request('/api/light', {'brightness': 80, 'interval_ms': 10001}, expected=400)
    checked = request('/api/state')
    assert light_fields(checked) == before and checked['http_stack_free_bytes'] >= 2048
    print('HTTP stack minimum free bytes: ' + str(checked['http_stack_free_bytes']), flush=True)
    print('PASS: all five HTTP modes, OFF/ON/zero brightness, invalid interval rejected atomically', flush=True)

    command = dict(state='ON', brightness=37, effect='crossfade', interval_ms=2350)
    client.publish(base + '/set', json.dumps(command), qos=1, retain=False).wait_for_publish(timeout=5)
    wait_message(base + '/state', lambda p: light_fields(json.loads(p)) == command)
    assert light_fields(request('/api/state')) == command
    print('PASS: MQTT command -> device -> retained state -> HTTP', flush=True)

    request('/api/restart', {})
    wait_message(base + '/availability', lambda p: p == b'offline')
    time.sleep(2)
    assert light_fields(wait_online()) == command
    print('PASS: restart restores light settings and MQTT connection', flush=True)

    request('/api/settings', {'mqtt_port': 0}, expected=400)
    network = {key: settings[key] for key in ('ssid', 'mqtt_host', 'mqtt_port', 'mqtt_user', 'mqtt_enabled')}
    request('/api/settings', network)
    time.sleep(3)
    assert light_fields(wait_online()) == command and request('/api/settings') == settings
    print('PASS: network settings saved to NVS; Wi-Fi/MQTT reconnect after reboot', flush=True)

    if args.ota:
        previous_slot = request('/api/state')['ota_slot']
        request('/api/update', b'\x00' * 2048, expected=400)
        checked = request('/api/state')
        assert light_fields(checked) == command and checked['http_stack_free_bytes'] >= 2048
        request('/api/update', args.ota.read_bytes())
        time.sleep(3)
        restored = wait_online()
        assert restored['version'] == state['version'] and light_fields(restored) == command
        assert restored['ota_slot'] != previous_slot, 'OTA did not switch the running partition'
        print('PASS: invalid OTA rejected; real OTA boots with saved state and MQTT', flush=True)

    if args.check_alias_change:
        temporary = args.alias[:12] + '-test-' + state['id'][-6:]
        previous_base = base
        for alias in (temporary, args.alias):
            request('/api/alias', {'alias': alias})
            time.sleep(3)
            renamed = wait_online()
            assert renamed['alias'] == renamed['hostname'] == renamed['ap_ssid'] == alias
            assert renamed['id'] == state['id'] and renamed['topic'] == 'wifi2mqtt/' + alias
            assert light_fields(renamed) == command
            current_settings = request('/api/settings')
            assert current_settings == dict(settings, alias=alias)
            config = json.loads(wait_message(discovery, lambda p: json.loads(p)['name'] == alias))
            assert config['unique_id'] == state['id'] and config['device']['name'] == alias
            assert config['command_topic'] == renamed['topic'] + '/set'
            assert received[previous_base + '/state'] == received[previous_base + '/availability'] == b''
            previous_base = renamed['topic']
            client.subscribe(previous_base + '/state', 1)
            wait_message(previous_base + '/state', lambda p: bool(p) and json.loads(p)['alias'] == alias)
            client.subscribe(previous_base + '/availability', 1)
            wait_message(previous_base + '/availability', lambda p: p == b'online')
        print('PASS: alias rename/restore survives restart, updates discovery, removes old MQTT state, preserves credentials/light', flush=True)
finally:
    try:
        if request('/api/state')['alias'] != args.alias:
            request('/api/alias', {'alias': args.alias})
            time.sleep(3)
            wait_online()
        request('/api/light', original)
        time.sleep(2.2)
        restored = request('/api/state')
        assert light_fields(restored) == original and not restored['storage_error']
        print('Restored initial light state: ' + json.dumps(original), flush=True)
    finally:
        client.disconnect()
        client.loop_stop()

print('DEVICE CHECK PASSED: ' + json.dumps(restored), flush=True)
