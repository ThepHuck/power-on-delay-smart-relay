"""Small standard-library client for the relay's /api/v1 endpoints.

Set ESP_RELAY_API_KEY, then run:
  python examples/api_client.py --base http://esp-relay.local/api/v1 GET /status
  python examples/api_client.py PATCH /config --json '{"startupDelaySeconds":30}'

Import RelayApi for scripts. Mutating requests are never retried automatically.
"""

import argparse
import json
import os
from urllib.error import HTTPError
from urllib.request import HTTPRedirectHandler, Request, build_opener


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        return None  # API endpoints never redirect; do not forward credentials elsewhere.


class ApiError(Exception):
    def __init__(self, status, payload, headers):
        self.status = status
        self.payload = payload
        self.headers = headers
        error = payload.get("error", {}) if isinstance(payload, dict) else {}
        self.code = error.get("code", "http_error")
        super().__init__(f"HTTP {status} {self.code}: {error.get('message', payload)}")


class RelayApi:
    def __init__(self, base_url, api_key, timeout=15):
        if not api_key:
            raise ValueError("An API key is required")
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout
        self._opener = build_opener(_NoRedirect)

    def request(self, method, resource, data=None, etag=None):
        if not resource.startswith("/") or resource.startswith("//"):
            raise ValueError("Use a resource path such as /config")
        headers = {"Authorization": f"Bearer {self.api_key}", "Accept": "application/json"}
        body = None
        if data is not None:
            body = json.dumps(data, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if etag is not None:
            headers["If-Match"] = etag
        request = Request(self.base_url + resource, data=body, headers=headers, method=method.upper())
        try:
            response = self._opener.open(request, timeout=self.timeout)
        except HTTPError as error:
            with error:
                raw = error.read().decode("utf-8")
                try:
                    payload = json.loads(raw)
                except json.JSONDecodeError:
                    payload = {"error": {"code": "invalid_response", "message": raw}}
                raise ApiError(error.code, payload, error.headers) from error
        with response:
            return json.load(response), response.headers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://esp-relay.local/api/v1")
    parser.add_argument("method", choices=["GET", "POST", "PUT", "PATCH", "DELETE"])
    parser.add_argument("resource")
    sources = parser.add_mutually_exclusive_group()
    sources.add_argument("--json", help="JSON request object")
    sources.add_argument("--json-file", help="Read the JSON request object from a UTF-8 file")
    parser.add_argument("--etag", help="ETag from the latest schedule response, including quotes")
    args = parser.parse_args()
    payload = json.loads(args.json) if args.json is not None else None
    if args.json_file:
        with open(args.json_file, encoding="utf-8") as source:
            payload = json.load(source)
    key = os.environ.get("ESP_RELAY_API_KEY")
    if not key:
        parser.error("Set ESP_RELAY_API_KEY to a key generated under Config")
    try:
        data, headers = RelayApi(args.base, key).request(args.method, args.resource, payload, args.etag)
    except ApiError as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(data, indent=2))
    if headers.get("ETag"):
        print("ETag:", headers["ETag"])


if __name__ == "__main__":
    main()
