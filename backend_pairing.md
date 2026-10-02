# avagenc — Device ↔ User Pairing Backend Contract

This document is the implementation spec for the **avagenc backend** REST API that
pairs a physical device (Waveshare ESP32‑S3, xiaozhi firmware) to an avagenc user
account, and for how the realtime bridge authenticates a paired device.

It is written to match the firmware exactly (`main/device_binding.cc`,
`main/protocols/websocket_protocol.cc`). If you change a field name here, change it
in the firmware too.

---

## 1. Concept

- The **device identity key** is the WiFi **MAC address** (`Device-Id`), burned in
  eFuse — stable across reflash / NVS erase.
- The device never asserts a `user_id`. Instead it earns an opaque **`device_token`**
  at pairing time and presents it at runtime; the **backend resolves the user** from
  that token. (A raw user id from the device would be trivially spoofable.)
- Pairing uses a short‑lived **`pairing_code`** + a **QR code** shown on the LCD. The
  user, already logged into the avagenc app/web, scans it and confirms — that call
  carries the *user's* auth and is what actually writes the `device ↔ user` row.

### Flow

```
 Device (ESP32)              avagenc backend                 User's phone / web
 ──────────────              ───────────────                 ──────────────────
   | no token in NVS               |                                 |
   |── POST /pair/start ──────────►| create pairing_session          |
   |◄── pairing_code, pair_url ────| (status=pending, TTL)           |
   |                               |                                 |
   | show QR(pair_url) + code      |                                 |
   |                               |        scan QR ─────────────────|
   |                               |◄── GET /pair page (pair_url) ───|
   |                               |    user is logged in            |
   |                               |◄── POST /devices/bind ──────────|  (user auth)
   |                               |  validate code → write d↔u      |
   |                               |  issue device_token             |
   |── GET /devices/{id}/status ──►|                                 |
   |◄── bound:true, device_token ──|                                 |
   | save token to NVS, hide QR    |                                 |
   |                               |                                 |
   |═ WS connect: Bearer token ═══►| resolve user_id from token      |
```

---

## 2. Conventions

| Item | Value |
|---|---|
| Base URL | configured on device via `CONFIG_AVAGENC_API_BASE_URL` or NVS `avagenc/api_url` (e.g. `https://api.avagenc.com`, or `http://IP:PORT` for dev). No trailing slash. |
| Content type | `application/json` (request and response) |
| Device id format | lowercase hex with colons, e.g. `a1:b2:c3:d4:e5:f6`. Treat **case‑insensitively**. Note the path form `/devices/a1:b2:c3:d4:e5:f6/status` contains colons — legal in a path segment (RFC 3986 `pchar`); make sure your router captures it (or accept the `Device-Id` header as the source of truth). |
| Time | all timestamps ISO‑8601 UTC; all durations in **seconds** |

### Headers sent by the **device** on every call (`/pair/start`, `/status`)

```
Device-Id: a1:b2:c3:d4:e5:f6        # MAC — the identity key
Client-Id: 1f3c...-...-...           # per-install UUID (NVS), informational
User-Agent: esp32-s3-touch-lcd-1.85c/1.0.0    # <BOARD_NAME>/<fw_version>
Content-Type: application/json
```

> The device is **unauthenticated** at `/pair/start` and `/status` (it has no token
> yet). Identity is the `Device-Id`. Protect these endpoints with rate limiting and,
> for production, an optional eFuse HMAC challenge (see §8).

---

## 3. Endpoints

### 3.1 `POST /api/v1/devices/pair/start`  — called by the **device**

Starts (or returns the active) pairing session for this device.

**Request body**
```json
{
  "device_id": "a1:b2:c3:d4:e5:f6",
  "device_code": "AVA-7F3K9-Q2A5C",
  "model": "esp32-s3-touch-lcd-1.85c",
  "fw_version": "1.0.0"
}
```
- `device_code` — human‑friendly code derived from the MAC (Crockford base32),
  format `AVA-XXXXX-XXXXX`. Deterministic; useful for support/display. Not secret.

**Response 200 — pending (device not yet bound)**
```json
{
  "bound": false,
  "pairing_code": "7F3K9Q",
  "pair_url": "https://app.avagenc.com/pair?d=a1:b2:c3:d4:e5:f6&c=7F3K9Q",
  "poll_interval": 3,
  "expires_in": 600
}
```
| Field | Type | Notes |
|---|---|---|
| `pairing_code` | string | **required.** 6–8 chars, easy to read/type (avoid 0/O, 1/I/L). Shown under the QR. |
| `pair_url` | string | URL encoded in the QR. Opens the app (universal/deep link) or web `/pair` page, carrying `device_id` + `pairing_code`. If omitted, the device renders the QR from `pairing_code` alone. |
| `poll_interval` | number | seconds between `/status` polls. Firmware default 3 if missing; min clamped to 2. |
| `expires_in` | number | code TTL (seconds). Recommend **≥ 360**; firmware re‑requests a fresh code roughly every 300 s, so a shorter TTL just causes harmless refreshes. |

**Response 200 — already bound (idempotent)**
```json
{ "bound": true, "device_token": "dvt_9aF...opaque..." }
```
Firmware saves the token and skips pairing. Return this if a valid `device ↔ user`
row already exists for this `Device-Id`.

**Behaviour**
- If an unexpired pending session already exists for this device, **return the same
  `pairing_code`** (don't churn codes on every poll/retry).
- Create/lookup the `devices` row by `Device-Id`; store `device_code`, `model`,
  `fw_version`, `last_seen`.

Firmware treats non‑200, invalid JSON, or a missing `pairing_code` (when not bound)
as a failure and retries (up to 20× / ~100 s, then boots unbound).

---

### 3.2 `GET /api/v1/devices/{device_id}/status`  — called by the **device**

Polled until the device is bound.

**Response 200 — pending**
```json
{ "bound": false }
```

**Response 200 — bound**
```json
{
  "bound": true,
  "device_token": "dvt_9aF...opaque...",
  "user_id": "usr_123"
}
```
| Field | Type | Notes |
|---|---|---|
| `bound` | bool | **required.** |
| `device_token` | string | **required when bound.** Opaque, device‑scoped. Firmware stores it in NVS and sends it as `Authorization: Bearer`. |
| `user_id` | string | optional, for display only. Firmware does not trust it. |

Use the `Device-Id` header (or the path param) as the lookup key. Returning `bound:false`
for an unknown device is fine.

---

### 3.3 `POST /api/v1/devices/bind`  — called by the **app / web** (NOT the device)

This is the only call that writes the `device ↔ user` mapping. It carries the **user's**
auth, so the user that calls it becomes the owner.

**Headers**
```
Authorization: Bearer <user login/session token>
Content-Type: application/json
```

**Request body**
```json
{ "device_id": "a1:b2:c3:d4:e5:f6", "pairing_code": "7F3K9Q" }
```

**Response 200**
```json
{ "success": true, "device_id": "a1:b2:c3:d4:e5:f6" }
```

**Behaviour**
1. Authenticate the user from the bearer token → `user_id`.
2. Look up the pairing session by (`device_id`, `pairing_code`). Reject if not found,
   expired, or already consumed.
3. Write/replace the `device ↔ user` row (`device_id → user_id`).
4. Mint a `device_token` bound to (`device_id`, `user_id`); persist it.
5. Mark the pairing session `consumed`.
6. The device's next `/status` poll returns `bound:true` + the token.

**Errors** — see §7. Common: `404 pairing_not_found`, `410 pairing_expired`,
`409 already_bound` (if you disallow re‑binding to a different user without unbind).

---

### 3.4 `POST /api/v1/devices/unbind` — optional (app/web)

Lets a user release a device so it can be re‑paired (e.g. resale). Authenticated as
the owning user. After unbind, invalidate the `device_token` so the device falls back
to pairing on next boot (it will get rejected at the WS layer and should re‑pair — or
add a firmware path to clear the token on `401`).

```json
{ "device_id": "a1:b2:c3:d4:e5:f6" }
```

---

### 3.5 Runtime — realtime bridge auth

At runtime the device opens the audio WebSocket with:
```
Device-Id: a1:b2:c3:d4:e5:f6
Client-Id: <uuid>
Authorization: Bearer dvt_9aF...opaque...
Protocol-Version: 1
```
The bridge (currently the Go server) must:
1. Read the `Authorization` bearer → look up `device_token` → `(device_id, user_id)`.
2. Verify the token's `device_id` matches the `Device-Id` header (defence in depth).
3. Attach `user_id` to the session for all downstream logic.
4. Reject with `401` if the token is missing/invalid/revoked. (Optionally allow an
   unbound device into a limited "needs pairing" mode.)

This replaces the current static test user.

---

## 4. Data model (reference)

```
devices
  device_id      TEXT PRIMARY KEY      -- MAC, lowercased
  device_code    TEXT                  -- AVA-XXXXX-XXXXX (derived, informational)
  user_id        TEXT NULL             -- FK users.id; NULL until bound
  model          TEXT
  fw_version     TEXT
  bound_at       TIMESTAMPTZ NULL
  last_seen      TIMESTAMPTZ

pairing_sessions
  id             TEXT PRIMARY KEY
  device_id      TEXT                  -- FK devices.device_id
  pairing_code   TEXT                  -- short code (unique among active sessions)
  status         TEXT                  -- 'pending' | 'consumed' | 'expired'
  created_at     TIMESTAMPTZ
  expires_at     TIMESTAMPTZ
  consumed_at    TIMESTAMPTZ NULL

device_tokens
  token          TEXT PRIMARY KEY      -- opaque, store hashed (e.g. sha256)
  device_id      TEXT
  user_id        TEXT
  created_at     TIMESTAMPTZ
  revoked_at     TIMESTAMPTZ NULL
```

---

## 5. Pairing code & token rules

**`pairing_code`**
- 6–8 chars from an unambiguous alphabet (Crockford base32: `0-9 A-Z` minus `I L O U`).
- Unique among **active** (`pending`, unexpired) sessions so `(device_id, code)` and a
  bare `code` both resolve unambiguously.
- TTL `expires_in` ≥ 360 s (recommend 600 s). Reuse the active code on repeated
  `/pair/start` from the same device.
- Single‑use: `consumed` after a successful `/devices/bind`.

**`device_token`**
- Opaque, ≥ 128 bits of entropy, prefixed (e.g. `dvt_`). Store **hashed** at rest.
- Bound to `(device_id, user_id)`. Long‑lived; revoke on unbind.
- Never logged in full.

---

## 6. The `/pair` page / deep link (what the QR opens)

`pair_url` should open the avagenc app via universal/app link, falling back to a web
page, carrying `d=<device_id>` and `c=<pairing_code>`:

```
https://app.avagenc.com/pair?d=a1:b2:c3:d4:e5:f6&c=7F3K9Q
```

The page/app:
1. Ensures the user is logged in (redirect to login, then back).
2. Shows a confirmation ("Pair device AVA‑7F3K9‑Q2A5C to your account?").
3. On confirm → `POST /api/v1/devices/bind` with the user's bearer token.
4. Shows success; the device picks it up on its next poll.

Also provide a **manual entry** path (user types the short `pairing_code`) for when the
camera can't scan.

---

## 7. Error response shape

```json
{ "error": "pairing_expired", "message": "This code has expired. Generate a new one." }
```

| HTTP | `error` | When |
|---|---|---|
| 400 | `invalid_request` | malformed body / missing field |
| 401 | `unauthorized` | bind/unbind without a valid user token; WS with bad device_token |
| 404 | `pairing_not_found` | unknown `(device_id, pairing_code)` |
| 409 | `already_bound` | device already bound to another user (and re‑bind disallowed) |
| 410 | `pairing_expired` | code TTL passed |
| 429 | `rate_limited` | too many `/pair/start` or `/status` from one device/IP |

Device endpoints: return **200** for normal pending/bound states (don't use 404 for
"not bound yet"). The firmware only treats non‑200 / missing fields as transient errors.

---

## 8. Security notes

- **Identity at `/pair/start` & `/status` is just the MAC** → spoofable. Mitigate with
  per‑device rate limits and short code TTLs. For production, provision a per‑device
  secret in eFuse `USER_DATA` and require an HMAC‑SHA256 challenge — the firmware
  already has this path (`Activation-Version: 2`, `Serial-Number`, HMAC in `ota.cc`).
- **Binding requires user auth** (`/devices/bind`) — that's where ownership is decided.
- A `pairing_code` only lets someone *claim* a device into **their own** account; it
  grants no access to existing data. Still, keep codes short‑lived and single‑use.
- Store `device_token` hashed; support revocation (unbind / suspected compromise).
- Always cross‑check the WS `Device-Id` header against the token's device at runtime.

---

## 9. Implementation checklist

- [ ] `POST /api/v1/devices/pair/start` — upsert device, create/reuse pending session, return code + `pair_url` (or `bound:true` + token if already bound).
- [ ] `GET /api/v1/devices/{device_id}/status` — return `bound` (+ token when bound). Handle colons in the path / use `Device-Id` header.
- [ ] `POST /api/v1/devices/bind` — user‑authed; validate code, write `device↔user`, mint token, consume session.
- [ ] (optional) `POST /api/v1/devices/unbind`.
- [ ] `/pair` deep link + web page (login → confirm → bind), with manual‑code fallback.
- [ ] Bridge: validate `Authorization: Bearer <device_token>`, resolve `user_id`, cross‑check `Device-Id`, reject `401` on bad token.
- [ ] Background job to expire stale `pending` sessions.
- [ ] Rate limiting on `/pair/start` and `/status`.

---

## 10. Quick reference — example calls

```bash
# Device starts pairing
curl -X POST https://api.avagenc.com/api/v1/devices/pair/start \
  -H 'Device-Id: a1:b2:c3:d4:e5:f6' -H 'Content-Type: application/json' \
  -d '{"device_id":"a1:b2:c3:d4:e5:f6","device_code":"AVA-7F3K9-Q2A5C","model":"esp32-s3-touch-lcd-1.85c","fw_version":"1.0.0"}'
# → {"bound":false,"pairing_code":"7F3K9Q","pair_url":"https://app.avagenc.com/pair?d=a1:b2:c3:d4:e5:f6&c=7F3K9Q","poll_interval":3,"expires_in":600}

# App binds (user-authenticated)
curl -X POST https://api.avagenc.com/api/v1/devices/bind \
  -H 'Authorization: Bearer <USER_TOKEN>' -H 'Content-Type: application/json' \
  -d '{"device_id":"a1:b2:c3:d4:e5:f6","pairing_code":"7F3K9Q"}'
# → {"success":true,"device_id":"a1:b2:c3:d4:e5:f6"}

# Device polls
curl https://api.avagenc.com/api/v1/devices/a1:b2:c3:d4:e5:f6/status \
  -H 'Device-Id: a1:b2:c3:d4:e5:f6'
# → {"bound":true,"device_token":"dvt_9aF...","user_id":"usr_123"}
```

---

*Firmware references:* `main/device_binding.{h,cc}`,
`main/protocols/websocket_protocol.cc` (Bearer token), `main/system_info.cc`
(`GetDeviceCode`, `GetMacAddress`), Kconfig `CONFIG_USE_AVAGENC_DEVICE_BINDING` /
`CONFIG_AVAGENC_API_BASE_URL`.
