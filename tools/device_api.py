"""Local ESP32 HTTP helper. Never reads flash or creates a board backup."""
import json
import time
import urllib.error
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
host = 'http://192.0.2.48'

def request(path, body=None, expected=200):
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
    assert before['id'] == 'svetylka-20489c6cdda0' and before['board'] == 'lilygo-t-display'
    blob = path.read_bytes()
    assert blob[0] == 0xe9 and blob[3] >> 4 == 4 and version.encode()+b'\0' in blob
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
            if state['version'] == version and state['ota_slot'] != before['ota_slot'] and state['wifi'] and state['mqtt']:
                print('OTA verified:', version, state['ota_slot'], flush=True)
                return state
        except OSError:
            pass
    raise AssertionError('New firmware did not return online')
