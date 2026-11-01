"""Offline tests: the HTTP helper must not write to an unverified device."""
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import device_api

class Reply(io.BytesIO):
    status=200
    headers={'Content-Type':'application/json'}
    def __init__(self,value):super().__init__(json.dumps(value).encode())

class TargetGuard(unittest.TestCase):
    def test_requires_explicit_id(self):
        with patch.object(device_api,'expected_id',''),patch.object(device_api.opener,'open') as open_request:
            with self.assertRaises(RuntimeError):device_api.request('/api/light',{'state':'ON'})
            open_request.assert_not_called()
    def test_wrong_device_receives_no_write(self):
        with patch.object(device_api,'expected_id','target'),patch.object(device_api.opener,'open',return_value=Reply({'id':'other'})) as open_request:
            with self.assertRaises(RuntimeError):device_api.request('/api/light',{'state':'ON'})
            self.assertEqual(open_request.call_count,1)
            self.assertIsNone(open_request.call_args.args[0].data)
    def test_verified_device_receives_write(self):
        with patch.object(device_api,'expected_id','target'),patch.object(device_api.opener,'open',side_effect=[Reply({'id':'target'}),Reply({'ok':True})]) as open_request:
            self.assertEqual(device_api.request('/api/light',{'state':'OFF'}),{'ok':True})
            self.assertEqual(json.loads(open_request.call_args.args[0].data),{'state':'OFF'})
    def test_reads_do_not_need_target(self):
        with patch.object(device_api,'expected_id',''),patch.object(device_api.opener,'open',return_value=Reply({'id':'other'})):
            self.assertEqual(device_api.request('/api/state'),{'id':'other'})

if __name__=='__main__':unittest.main()
