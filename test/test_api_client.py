import io
import json
import unittest
from email.message import Message
from urllib.error import HTTPError
from unittest.mock import Mock, patch

from examples.api_client import ApiError, RelayApi, _NoRedirect


class ApiClientTests(unittest.TestCase):
    def setUp(self):
        self.opener = Mock()
        with patch("examples.api_client.build_opener", return_value=self.opener):
            self.api = RelayApi("http://device.local/api/v1/", "test-key")

    def respond(self, data, headers=None):
        response = io.BytesIO(json.dumps(data).encode())
        response.headers = headers or Message()
        self.opener.open.return_value = response

    def test_read_uses_header_auth_and_preserves_etag(self):
        headers = Message()
        headers["ETag"] = '"abc123"'
        self.respond({"schedules": []}, headers)
        data, returned = self.api.request("GET", "/schedules")
        request = self.opener.open.call_args.args[0]
        self.assertEqual(request.full_url, "http://device.local/api/v1/schedules")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-key")
        self.assertIsNone(request.data)
        self.assertEqual(data, {"schedules": []})
        self.assertEqual(returned["ETag"], '"abc123"')

    def test_schedule_write_preserves_quotes_and_json_types(self):
        self.respond({"schedules": []})
        payload = {"hour": 3, "minute": 15, "delaySeconds": 10, "days": [1]}
        self.api.request("POST", "/schedules", payload, etag='"abc123"')
        request = self.opener.open.call_args.args[0]
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.get_header("If-match"), '"abc123"')
        self.assertEqual(request.get_header("Content-type"), "application/json")
        self.assertEqual(json.loads(request.data), payload)
        self.assertNotIn(b" ", request.data)

    def test_api_error_is_not_retried(self):
        headers = Message()
        headers["Retry-After"] = "5"
        body = {"error": {"code": "clock_unavailable", "message": "Wait for NTP."}}
        self.opener.open.side_effect = HTTPError("http://device.local", 503, "Unavailable", headers,
                                                io.BytesIO(json.dumps(body).encode()))
        with self.assertRaises(ApiError) as caught:
            self.api.request("GET", "/status")
        self.assertEqual(caught.exception.status, 503)
        self.assertEqual(caught.exception.code, "clock_unavailable")
        self.assertEqual(caught.exception.headers["Retry-After"], "5")
        self.opener.open.assert_called_once()

    def test_non_json_error_is_reported(self):
        self.opener.open.side_effect = HTTPError("http://device.local", 502, "Bad gateway", Message(),
                                                io.BytesIO(b"Gateway unavailable"))
        with self.assertRaises(ApiError) as caught:
            self.api.request("GET", "/status")
        self.assertEqual(caught.exception.code, "invalid_response")

    def test_credentials_are_not_forwarded_to_redirects(self):
        self.assertIsNone(_NoRedirect().redirect_request(None, None, 302, "Found", Message(),
                                                        "http://another-host/"))

    def test_requires_a_key_and_relative_resource(self):
        with self.assertRaises(ValueError):
            RelayApi("http://device.local/api/v1", "")
        for path in ("http://another-host/", "//another-host/", "config"):
            with self.assertRaises(ValueError):
                self.api.request("GET", path)
        self.opener.open.assert_not_called()


if __name__ == "__main__":
    unittest.main()
