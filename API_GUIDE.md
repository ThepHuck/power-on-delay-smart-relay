# ESP8266 relay API guide

The API is available at `http://<device-address>/api/v1`. The default mDNS
address is `http://esp-relay.local/api/v1`; the device's IP address also works.
All paths below are relative to `/api/v1`.

## Create your first key

1. Sign in to the web interface and open **Config → API keys**.
2. Enter a key name and choose **No expiration**, **Number of days**, or
   **Expiration date**.
3. Select **Generate API key** and copy the key immediately. The full key is
   shown once. Later, you can list its name, ID and expiration, or revoke it.

Each key has full administration access, including creating and revoking other
keys. There are **five slots**. Expired keys retain their slots until revoked.
To rotate a key, create a replacement, update clients, then revoke the old key.
If all slots are full, revoke one first. The web login remains available if
every API key expires, is revoked, or is lost.

Keys persist through restarts and power loss. The controller stores SHA-256
digests of randomly generated 256-bit secrets; it cannot retrieve a lost key.
API keys and the admin password are independent. Changing the password signs
out web sessions and preserves API keys.

## Authentication and request format

Send the full key in an HTTP header on **every** request:

```http
Authorization: Bearer esp_<64 hexadecimal characters>
```

Cookies, passwords, query parameters and the setup hotspot password do not
authenticate API requests. No CSRF token is required for the API. Web forms
continue using their existing login and CSRF protection.

POST, PUT and PATCH require one JSON object, `Content-Type: application/json`,
and `Content-Length`. Normal HTTP clients set Content-Length automatically
when given a complete string or byte buffer. DELETE and GET take no body.
Chunked request bodies and `Expect: 100-continue` are unsupported. JSON responses
may be chunked; standard clients decode them automatically. Connections close
after each response. Cross-origin browser access is not enabled.

Request limits are **4096 body bytes**, **2048 header bytes**, **512 bytes per
header line**, and **five JSON nesting levels**. Headers must arrive within
five seconds; the complete request has a ten-second deadline. Bodies are not
buffered before authentication. Unknown fields, wrong JSON types, out-of-range
numbers, malformed dates, duplicate framing/authentication headers, embedded
NULs and trailing data after the JSON object are rejected. Send integers as
JSON numbers, rather than strings, booleans or fractional numbers.

The firmware serves HTTP. Headers and response bodies, including newly issued
keys, travel without TLS. Use it on a trusted network or through a TLS-terminating
gateway/VPN; avoid exposing port 80 directly to the internet. Keep keys out of
URLs, source control and request logs.

## Expiration rules

| Choice | Behavior |
| --- | --- |
| `none` | No expiration; usable even before the first NTP sync after boot. |
| `days` | Whole number **1–30**. Expires exactly that many 24-hour periods after creation. |
| `date` | Valid `YYYY-MM-DD`. Remains valid throughout the selected date, including 23:59:59; expires at **00:00 the next day** in the device's configured time zone. |

Creating an expiring key requires a successful NTP sync. An already ended date
is rejected; today's date is accepted until its next midnight. Date-based
expiration uses local daylight saving rules, so the remaining duration need
not be a multiple of 24 hours. For example, with `Etc/UTC`, selecting
`2030-01-31` sets the exclusive deadline to `2030-02-01T00:00:00Z`.

The deadline is stored as a UTC instant at creation. Changing the device's
time zone later does not move an existing deadline. Date-based key metadata
includes the original selected date and time zone. To change a key's expiration,
issue a replacement and revoke the original.

After each controller restart, expiring keys return **503 `clock_unavailable`**
until NTP has synchronized. A no-expiration key can inspect status or repair
NTP/Wi-Fi settings during this period; the web UI is also available. Once synced,
the software clock keeps advancing through Wi-Fi outages. During a boot session,
backward NTP adjustments cannot extend key lifetimes. Observed expiration is
latched and saved so a later clock rollback cannot reactivate the key.

The ESP8266 has no battery-backed RTC; drift and NTP accuracy affect deadlines.
Expired, revoked and unknown keys return **401 `unauthorized`**.

## Endpoint reference

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | API version and limits. |
| GET | `/status` | Relay, countdown, Wi-Fi, clock and uptime. |
| GET | `/config` | Device name, hostname and startup delay. |
| PATCH | `/config` | Update one or more device settings. |
| PUT | `/password` | Change the web admin password. |
| GET | `/wifi` | Saved SSID, connection and pending configuration result. |
| PUT | `/wifi` | Test a new network, then save credentials after success. |
| GET | `/time` | NTP settings, selected zone and local clock. |
| PATCH | `/time` | Update one or more NTP/time-zone settings. |
| GET | `/timezones` | List supported time-zone names. |
| GET | `/schedules` | List schedules and get their ETag. |
| POST | `/schedules` | Add a schedule; requires If-Match. |
| PUT | `/schedules` | Replace the entire schedule collection atomically; requires If-Match. |
| GET | `/schedules/{id}` | Read one schedule and the collection's ETag. |
| PUT | `/schedules/{id}` | Replace one schedule; requires If-Match. |
| DELETE | `/schedules/{id}` | Delete one schedule; requires If-Match. |
| GET | `/relay` | Relay state and countdown. |
| PUT | `/relay` | Set the relay explicitly ON/OFF; cancels a countdown. |
| POST | `/timer` | Turn OFF, then ON after a countdown. |
| DELETE | `/timer` | Cancel a countdown, leaving the current relay state in place. |
| GET | `/keys` | List key metadata, including expired keys. Never returns saved secrets. |
| POST | `/keys` | Generate another key; returns its secret once. |
| DELETE | `/keys/{id}` | Revoke a key immediately and free its slot. |

## Quick start

PowerShell:

```powershell
$env:ESP_RELAY_API_KEY = 'paste-the-key-you-generated'
$base = 'http://esp-relay.local/api/v1'
$headers = @{ Authorization = "Bearer $env:ESP_RELAY_API_KEY" }
Invoke-RestMethod "$base/status" -Headers $headers
Invoke-RestMethod "$base/config" -Method Patch -Headers $headers `
  -ContentType 'application/json' `
  -Body (@{ displayName = 'Router relay'; startupDelaySeconds = 30 } | ConvertTo-Json)
```

POSIX shell / curl:

```sh
export ESP_RELAY_API_KEY='paste-the-key-you-generated'
BASE='http://esp-relay.local/api/v1'
curl --fail-with-body -sS "$BASE/status" \
  -H "Authorization: Bearer $ESP_RELAY_API_KEY"
curl --fail-with-body -sS -X PATCH "$BASE/config" \
  -H "Authorization: Bearer $ESP_RELAY_API_KEY" \
  -H 'Content-Type: application/json' \
  --data '{"displayName":"Router relay","startupDelaySeconds":30}'
```

The included [Python client](examples/api_client.py) uses only the standard
library. With `ESP_RELAY_API_KEY` set:

```sh
python examples/api_client.py GET /status
python examples/api_client.py GET /timezones
python examples/api_client.py PATCH /config --json-file device-settings.json
```

For the final command, `device-settings.json` might contain:

```json
{"displayName":"Router relay","mdnsName":"router-relay","startupDelaySeconds":30}
```

## Device configuration and password

GET/PATCH `/config` response and writable fields:

```json
{"displayName":"Router relay","mdnsName":"router-relay","startupDelaySeconds":30}
```

PATCH accepts any nonempty subset. The display name is 1–64 bytes without
control characters. The hostname is 1–63 letters, digits or hyphens, without
leading/trailing hyphens or `.local`; it is normalized to lowercase. Startup
delay is an integer **1–4294967 seconds**, applied on the next controller boot.
Names are trimmed like the web form. Changing the hostname changes its mDNS
address; use the current IP address if your old hostname stops resolving.

PUT `/password` requires both fields:

```json
{"currentPassword":"your-current-password","newPassword":"your-new-password"}
```

The new password must be 8–128 bytes. A successful response is
`{"updated":true}`. The current password is required in addition to the API
key. Password verification shares the web login's one-second retry limit.
Neither passwords nor their hashes are returned by any API read.

## Wi-Fi configuration

PUT `/wifi`:

```json
{"ssid":"My 2.4 GHz Wi-Fi","password":"network-password"}
```

SSID is 1–32 bytes. Password is 8–63 printable characters, 64 hexadecimal
digits, or `""` for an open network. Spaces and capitalization are preserved.
The response is **202 Accepted**, including `operationId`, `operationStatus`
and `pending`. Credentials are committed only after a successful connection.
A second configuration attempt while one is pending returns 409.

Poll GET `/wifi` using the same key. The operation states are `idle`, `queued`,
`connecting`, `saved`, `connection_failed`, or `save_failed`. Completion requires
the matching `operationId` and `operationStatus: "saved"`; being connected
alone does not prove that the new credentials were saved. Operation IDs are
local to a boot session. Failed credentials leave the previous saved network intact.

Changing networks can disconnect the client before it receives the response.
The recovery hotspot remains available during the attempt. Reconnect on the
new network, use the new IP/mDNS address, or join the setup hotspot to inspect
the result. Wi-Fi passwords are never included in responses.

## Clock and NTP

PATCH `/time` accepts any nonempty subset of:

```json
{"ntpServer":"pool.ntp.org","syncHours":1,"timeZone":"America/Chicago"}
```

`syncHours` is an integer **1–6**. `ntpServer` is a hostname or IPv4 address;
URLs and IPv6 literals are not accepted. Choose `timeZone` from GET
`/timezones`, which returns `{"timeZones":["Africa/Abidjan", "..."]}`.
Changes apply immediately and restart NTP synchronization.

GET `/time` returns those settings plus `synced`, `localTime` and `unixTime`.
Before synchronization the last two fields are null. `localTime` includes a
numeric UTC offset, for example `2026-11-03T09:30:00-0600`; `unixTime` is UTC
epoch seconds. NTP failures do not prevent reading the clock with an otherwise
usable key.

## Schedules and concurrent edits

A schedule request object contains exactly:

```json
{"hour":3,"minute":15,"delaySeconds":10,"days":[1]}
```

The hour is **0–23**, minute **0–55 in steps of 5**, and power on delay is one
of **5, 10, 15, 30, 60** seconds. Days are distinct integers:

| Day | Value |
| --- | --- |
| Sunday | 0 |
| Monday | 1 |
| Tuesday | 2 |
| Wednesday | 3 |
| Thursday | 4 |
| Friday | 5 |
| Saturday | 6 |

`[1]` runs weekly on Monday. `[1,3,5]` runs on Monday, Wednesday and Friday.
**`[]` and `[0,1,2,3,4,5,6]` both run every day.** Use at most 32 schedules.
Two schedules can share a time if their effective days do not overlap.

GET `/schedules` returns:

```json
{"schedules":[{"id":0,"hour":3,"minute":15,"delaySeconds":10,"days":[1]}]}
```

It also returns an **ETag response header**. Send that exact value, including
its quotes, in **If-Match** when adding, updating, deleting or replacing
schedules. A missing header returns 428; a stale value returns 412 without
changing anything. Read again and reconcile your edit after a 412. Schedule IDs
are zero-based list positions and shift when an entry is deleted. The ETag
prevents acting on a position from an outdated list. Changes from the web UI
also invalidate the ETag. Time-setting changes can invalidate it as well.

POST `/schedules` adds one request object. PUT `/schedules/0` replaces that
entry. DELETE `/schedules/0` removes it. All successful writes return the current
collection and a fresh ETag; creation returns 201 and other writes return 200.
Fields such as response-only `id` must be removed from request objects.

PUT `/schedules` replaces the entire collection in one validated flash commit:

```json
{"schedules":[
  {"hour":3,"minute":15,"delaySeconds":10,"days":[1]},
  {"hour":4,"minute":0,"delaySeconds":5,"days":[0,6]}
]}
```

`{"schedules":[]}` deletes all schedules. Invalid entries or overlapping
days/times reject the whole replacement. Schedules use local time, wait for NTP
after boot, skip missed occurrences, and execute at most once per local date
during a boot session. Existing countdowns take priority. A manual relay write
cancels a countdown. These rules match the web interface.

Python example using the included client:

```python
import os
from examples.api_client import RelayApi, ApiError

api = RelayApi("http://esp-relay.local/api/v1", os.environ["ESP_RELAY_API_KEY"])
current, headers = api.request("GET", "/schedules")
try:
    updated, headers = api.request("POST", "/schedules", {
        "hour": 3, "minute": 15, "delaySeconds": 10, "days": [1]
    }, etag=headers["ETag"])
except ApiError as error:
    if error.status == 412:
        print("Schedules changed; read them again and reconcile the edit.")
    else:
        raise
```

## Relay and countdown control

PUT `/relay` with `{"on":true}` or `{"on":false}` sets the relay and cancels
any active countdown. GET `/relay` returns:

```json
{"on":true,"timerActive":false,"secondsRemaining":0}
```

POST `/timer` with `{"seconds":10}` immediately turns OFF, then ON after ten
seconds. Seconds must be an integer **1–4294967**. Starting another countdown
replaces the current one. DELETE `/timer` cancels the countdown and leaves the
relay in its current state; use PUT `/relay` with `{"on":true}` to turn ON
explicitly. These endpoints operate the relay's connected equipment.

## Programmatic key management

GET `/keys` lists metadata, never stored key secrets. POST `/keys` issues a new
key. Each example below is a complete request body:

```json
{"label":"Home automation","expiration":{"mode":"none"}}
```

```json
{"label":"Temporary integration","expiration":{"mode":"days","days":7}}
```

```json
{"label":"Date-limited job","expiration":{"mode":"date","date":"2030-01-31"}}
```

Only fields relevant to the chosen expiration mode are accepted. A successful
201 response includes `id`, the one-time `token`, and the current `keys` array.
Key metadata contains `id`, `label`, `expirationMode`, `status`, `createdAt` and
`expiresAt`. UTC timestamps use ISO 8601 with `Z`. `createdAt` is null if a
no-expiration key was created before NTP; `expiresAt` is null for no expiration.
Date-based keys also include `expiresOn` and `expirationTimeZone`. Status is
`active`, `expired`, or `clock_unavailable`.

DELETE `/keys/{id}` revokes the corresponding key immediately. IDs are stable
16-character hexadecimal strings, separate from the secrets. A key can revoke
itself; its delete succeeds, and subsequent requests with it return 401.

Key creation is not idempotent. If the connection drops during issuance, list
the keys, revoke the inaccessible new key if one exists, and generate a
replacement. Do not blindly retry POST after an uncertain outcome.

## Responses, errors and retries

Errors have one consistent JSON shape:

```json
{"error":{"code":"invalid_request","message":"Description of the problem."}}
```

| Status | Meaning |
| --- | --- |
| 200 | Successful read/update/delete. |
| 201 | A schedule or API key was created. |
| 202 | Wi-Fi configuration was accepted; inspect its eventual result. |
| 400 | Invalid JSON, unsupported fields/types/values, malformed headers or framing. |
| 401 | Missing, invalid, revoked or expired API key. Includes WWW-Authenticate. |
| 403 | Current password verification failed. |
| 404 | Unknown resource, key or schedule. |
| 405 | Wrong method; the Allow header lists supported methods. |
| 408 | Request timed out. |
| 409 | Key/schedule capacity reached, schedules overlap, or Wi-Fi change is busy. |
| 411 | A JSON write omitted Content-Length. |
| 412 | Schedule ETag is stale. |
| 413 | Body exceeds 4096 bytes. |
| 415 | A write did not use application/json. |
| 428 | A schedule write omitted If-Match. |
| 429 | Password retry limit; Retry-After: 1. |
| 431 | Header size limit exceeded. |
| 500 | Storage or response allocation failed; the requested setting was not acknowledged as saved. |
| 503 | NTP clock unavailable, setup hotspot unavailable, or memory temporarily unavailable; Retry-After: 5. |

Settings are validated before being saved. Each resource update is atomic;
changes across separate resources are separate operations. Unchanged settings
do not require another EEPROM flash write. Failed saves restore the EEPROM
buffer, so a later unrelated save cannot commit a rejected change. Flash
hardware failure or power loss during erase/write can still damage saved data.

Use bounded retries with delay for reads that encounter temporary connection
errors or 503. Re-read settings after an uncertain write before retrying it.
POST key creation, schedule addition and countdown restarts are not safe for
blind retries. Avoid continuous writes or using flash storage as a heartbeat.
All responses are marked no-store. The controller handles requests serially;
polling about once per second is sufficient for status/countdowns.
