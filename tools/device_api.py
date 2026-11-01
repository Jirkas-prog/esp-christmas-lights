"""Local ESP32 HTTP helper. Never reads flash or creates a board backup."""
import json
import os
import time
import urllib.error
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
host = os.environ.get('LIGHT_HOST', 'http://ledpasek02.local').rstrip('/')
expected_id = os.environ.get('LIGHT_DEVICE_ID', '')

def request(path, body=None, expected=200):
    if body is not None:
        if not expected_id:
            raise RuntimeError('Set LIGHT_DEVICE_ID to the verified target ID before writing.')
        actual=request('/api/state')['id']
        if actual!=expected_id:
            raise RuntimeError('Device identity mismatch; refusing write.')
    headers = {'X-Requested-With': 'Svetylka'}
    if isinstance(body, dict):
        body = json.dumps(body).encode()
        headers['Content-Type'] = 'application/json'
    elif body is not None:
        headers['Content-Type'] = 'application/octet-stream'
    try:
        response = opener.open(urllib.request.Request(host+path, body, headers), timeout=60 if path == '/api/update' else 8)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        value = response.read()
        assert response.status == expected, (path, response.status, value[:300])
        return json.loads(value) if 'json' in response.headers.get('Content-Type', '') else value

def upload(path, version):
    before = request('/api/state')
    if not expected_id or before['id']!=expected_id:
        raise RuntimeError('Verify LIGHT_DEVICE_ID before OTA.')
    flash_code={'lilygo-t-display':4,'esp32-wroom-32':2}[before['board']]
    blob = path.read_bytes()
    assert blob[0] == 0xe9 and blob[3] >> 4 == flash_code and version.encode()+b'\0' in blob
    print('Uploading', version, len(blob), flush=True)
    try:
        request('/api/update', blob)
    except OSError:
        print('Checking device after interrupted OTA response', flush=True)
    deadline = time.monotonic()+45
    while time.monotonic() < deadline:
        time.sleep(1)
        try:
            state = request('/api/state')
            if state['id'] == expected_id and state['version'] == version and state['ota_slot'] != before['ota_slot'] and state['wifi'] and state['mqtt']:
                print('OTA verified:', version, state['ota_slot'], flush=True)
                return state
        except OSError:
            pass
    raise AssertionError('New firmware did not return online')
