# avagenc — Local Device Pairing (Mobile App Contract)

This document is the implementation spec for the **avagenc Android app** to pair a
physical device (Waveshare ESP32‑S3, avagenc firmware) to a logged‑in user, over
the **local WiFi network** — no backend involved.

It matches the firmware exactly (`main/user_pairing.cc`). If a field name changes
here, it changes in the firmware too.

> **Two setup paths.** This doc (`avagenc_pair`) covers a device that is **already
> on WiFi** and needs a user (or a user switch). For a **brand‑new device with no
> WiFi**, use the combined WiFi+user flow in **`provisioning.md`** (`avagenc_provision`),
> which sets WiFi and user in one scan. Branch on the QR's `type` field.

> Status: **no backend / DB yet.** The device stores the `user_id` locally (NVS)
> and, at call time, sends it to the realtime bridge as the `X-User-Id` header.
> When the backend exists, only the *source of truth* for the mapping moves — the
> device↔phone handshake below can stay the same.

---

## 1. Concept

- When the device boots with **no stored user**, it:
  1. joins WiFi (already provisioned),
  2. starts a small **HTTP server on the LAN** (default port **8080**),
  3. mints a **one‑time pairing code** (6 chars, e.g. `7F3K9Q`),
  4. shows a **QR code** on its screen encoding how to reach it + that code.
- The phone (same WiFi, user logged in) **scans the QR**, then **POSTs the
  `user_id`** straight to the device, echoing the code as proof it actually saw
  the screen (header `X-Pair-Code`).
- The device validates the code, stores the `user_id`, replies success, hides the
  QR and continues booting.

### Flow

```
 Device (ESP32)                         Phone (avagenc Android app)
 ──────────────                         ───────────────────────────
  | no user in NVS                              |
  | start HTTP server :8080                     |
  | show QR { device_id, ip, port, code, ... }  |
  |                                             |
  |                     scan QR  ───────────────|  parse JSON payload
  |                                             |
  |◄──── GET  http://ip:8080/api/v1/pair/info ──|  (optional reachability check)
  |───── 200 { device_id, paired:false } ──────►|
  |                                             |
  |◄──── POST http://ip:8080/api/v1/pair ───────|  X-Pair-Code: <code>
  |      { "user_id": "...", ... }              |  body = user_id
  |  validate code → store user_id in NVS       |
  |───── 200 { success:true, device_id } ──────►|  show "Paired!"
  |  hide QR, continue boot                      |
  |                                             |
  |═ later: WS connect, header X-User-Id ═══════════════► realtime bridge
```

**Prerequisite:** the phone and the device must be on the **same WiFi/subnet**
during pairing (the phone connects directly to the device's LAN IP). If the phone
is on mobile data or a different SSID, pairing will fail to connect.

---

## 2. The QR payload

The QR encodes a compact JSON object (UTF‑8). Parse it as JSON — do **not** assume
field order.

```json
{
  "v": 1,
  "type": "avagenc_pair",
  "device_id": "a1:b2:c3:d4:e5:f6",
  "device_code": "AVA-7F3K9-Q2A5C",
  "ip": "192.168.1.42",
  "port": 8080,
  "code": "7F3K9Q",
  "pair_url": "http://192.168.1.42:8080/api/v1/pair"
}
```

| Field | Type | Notes |
|---|---|---|
| `v` | number | payload schema version. Currently `1`. Reject unknown majors. |
| `type` | string | always `"avagenc_pair"`. Use it to ignore unrelated QR codes. |
| `device_id` | string | the device MAC (lowercase, colon‑separated). Stable identity. |
| `device_code` | string | human‑friendly code derived from the MAC. Display only. |
| `ip` | string | device's current LAN IPv4. **Base your request URL on this.** |
| `port` | number | pairing HTTP port (default 8080). |
| `code` | string | **one‑time pairing code.** Send it back in `X-Pair-Code`. |
| `pair_url` | string | convenience: full URL for the POST. Equals `http://{ip}:{port}/api/v1/pair`. |

> The `ip` can change if the router re‑issues a lease. The device refreshes the QR
> automatically when that happens, so always build your request from the **freshly
> scanned** payload rather than caching an old IP.

---

## 3. Endpoints (served by the **device** on the LAN)

Base URL: `http://{ip}:{port}` from the QR. Content type `application/json`.
Cleartext HTTP on the LAN — see [§6 Security](#6-security-notes).

### 3.1 `GET /api/v1/pair/info` — optional reachability / identity check

Use this right after scanning to confirm you reached the right device before
posting anything. No auth, no side effects.

**Response 200**
```json
{
  "device_id": "a1:b2:c3:d4:e5:f6",
  "device_code": "AVA-7F3K9-Q2A5C",
  "model": "esp32-s3-touch-lcd-1.85c",
  "fw_version": "1.0.0",
  "paired": false
}
```
- `paired` — `true` if the device already has a stored user. If you scan a QR but
  `info` reports `paired:true`, the device is no longer waiting (already paired, or
  a stale QR). Tell the user to re‑trigger pairing (Logout on the device).

### 3.2 `POST /api/v1/pair` — send the user identity

The one call that binds the device to the user. **Idempotent-ish:** once the device
is paired it stops serving, so a second POST will simply fail to connect.

**Headers**
```
Content-Type: application/json
X-Pair-Code: 7F3K9Q          # the "code" from the QR (required)
```

**Request body**
```json
{
  "user_id": "usr_123",          // REQUIRED — the logged-in user's id
  "display_name": "Vico"         // optional — shown on the device
}
```
- If you can't set custom headers easily, you may instead put the code in the body
  as `"pairing_code": "7F3K9Q"`. The header takes precedence when both are present.

**Response 200 — success**
```json
{
  "success": true,
  "device_id": "a1:b2:c3:d4:e5:f6",
  "device_code": "AVA-7F3K9-Q2A5C",
  "display_name": "Vico"
}
```

**Error responses** (JSON body `{ "error": "...", "message": "..." }`)

| HTTP | `error` | When |
|---|---|---|
| 400 | `invalid_request` | missing/empty `user_id`, bad JSON, empty/oversized body (>2 KB) |
| 403 | `invalid_code` | `X-Pair-Code` doesn't match the code shown on the device |

There is **no 401** here — possession of the code (shown only on the device's
screen) is the authorization.

---

## 4. Android (Kotlin) reference

Scanning: use any QR scanner (e.g. ML Kit / ZXing) to get the raw string, then
parse it as JSON.

```kotlin
import okhttp3.*
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.util.concurrent.TimeUnit

data class PairTarget(val ip: String, val port: Int, val code: String, val deviceId: String)

fun parseQr(raw: String): PairTarget? {
    val o = runCatching { JSONObject(raw) }.getOrNull() ?: return null
    if (o.optString("type") != "avagenc_pair") return null
    return PairTarget(
        ip = o.getString("ip"),
        port = o.optInt("port", 8080),
        code = o.getString("code"),
        deviceId = o.getString("device_id"),
    )
}

private val http = OkHttpClient.Builder()
    .connectTimeout(4, TimeUnit.SECONDS)   // fail fast if wrong network
    .readTimeout(6, TimeUnit.SECONDS)
    .build()

/** Returns Result.success(deviceId) on success, or Result.failure with the error. */
fun pairDevice(target: PairTarget, userId: String, displayName: String?): Result<String> {
    val body = JSONObject().apply {
        put("user_id", userId)
        if (!displayName.isNullOrBlank()) put("display_name", displayName)
    }.toString().toRequestBody("application/json".toMediaType())

    val req = Request.Builder()
        .url("http://${target.ip}:${target.port}/api/v1/pair")
        .header("X-Pair-Code", target.code)
        .post(body)
        .build()

    return runCatching {
        http.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (resp.isSuccessful) {
                JSONObject(text).getString("device_id")
            } else {
                val err = runCatching { JSONObject(text).getString("error") }.getOrDefault("http_${resp.code}")
                throw IllegalStateException(err)   // e.g. "invalid_code"
            }
        }
    }
}
```

**Cleartext HTTP note (Android 9+):** requests to `http://` LAN IPs are blocked by
default. Allow cleartext for private ranges via a network security config, e.g.:

```xml
<!-- res/xml/network_security_config.xml -->
<network-security-config>
    <domain-config cleartextTrafficPermitted="true">
        <domain includeSubdomains="true">192.168.0.0/16</domain>
        <domain includeSubdomains="true">10.0.0.0/8</domain>
        <domain includeSubdomains="true">172.16.0.0/12</domain>
    </domain-config>
</network-security-config>
```
Reference it from `AndroidManifest.xml`:
`<application android:networkSecurityConfig="@xml/network_security_config" ...>`

---

## 5. UX & edge cases (app side)

- **Wrong network:** the POST connect will time out. Detect the timeout and prompt
  "Make sure your phone is on the same WiFi as the device."
- **`invalid_code` (403):** the QR was stale (device re‑minted a code, e.g. after a
  reboot). Ask the user to re‑scan.
- **`info.paired == true`:** device already has a user. To re‑pair, the user holds
  the device's on‑screen **Logout** (two‑tap confirm) → it reboots and shows a
  fresh QR.
- **Retry:** POST is safe to retry on network errors; on success the server stops,
  so a retry will just fail to connect — treat "connection refused after a prior
  success" as success if you already got a 200.
- **Don't cache the IP:** always use the IP from the most recent scan.

---

## 6. Security notes

- Pairing happens on the **local network**; authorization is **possession of the
  one‑time code** shown on the device screen (echoed in `X-Pair-Code`). Someone who
  can't see the screen can't pair.
- Traffic is **cleartext HTTP on the LAN**. The only secret exchanged is the
  `user_id` you choose to send — do **not** send passwords or long‑lived login
  tokens over this channel in the current (no‑backend) design.
- The code is single device‑session scoped: it's regenerated on each boot and the
  server stops the moment pairing succeeds.
- **Future hardening** (when the backend lands): switch the device→bridge identity
  from a raw `X-User-Id` to a backend‑issued opaque `device_token` (the device must
  not self‑assert a user id to a trusted server — it's spoofable). The phone would
  then also present its user auth so the backend, not the device, writes the
  mapping. See `backend_pairing.md` for that variant.

---

## 7. Runtime (after pairing) — for reference

Once paired, when the device opens the audio WebSocket to the bridge it adds:

```
Device-Id: a1:b2:c3:d4:e5:f6
Client-Id: <uuid>
X-User-Id: usr_123            # the value you posted during pairing
Protocol-Version: 1
```

The bridge should read `X-User-Id` to attach the session to that user. (Today the
bridge uses a single `X_USER_ID` env var for all sessions — moving it to read this
per‑connection header is the small server change that makes pairing end‑to‑end.)

---

## 8. Quick reference — example calls

```bash
# 1. Check reachability / identity (optional)
curl http://192.168.1.42:8080/api/v1/pair/info
# → {"device_id":"a1:b2:c3:d4:e5:f6","device_code":"AVA-7F3K9-Q2A5C",
#    "model":"esp32-s3-touch-lcd-1.85c","fw_version":"1.0.0","paired":false}

# 2. Pair (send the user id, echo the code from the QR)
curl -X POST http://192.168.1.42:8080/api/v1/pair \
  -H 'Content-Type: application/json' \
  -H 'X-Pair-Code: 7F3K9Q' \
  -d '{"user_id":"usr_123","display_name":"Vico"}'
# → {"success":true,"device_id":"a1:b2:c3:d4:e5:f6","device_code":"AVA-7F3K9-Q2A5C","display_name":"Vico"}

# Wrong code:
# → HTTP 403 {"error":"invalid_code","message":"pairing code does not match the one on the device screen"}
```

---

*Firmware references:* `main/user_pairing.{h,cc}` (local pairing server),
`main/protocols/websocket_protocol.cc` (`X-User-Id` at runtime),
`main/system_info.cc` (`GetDeviceCode`, `GetMacAddress`), Kconfig
`CONFIG_USE_AVAGENC_USER_PAIRING` / `CONFIG_AVAGENC_PAIRING_PORT`.
