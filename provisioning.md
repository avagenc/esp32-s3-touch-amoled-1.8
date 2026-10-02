# avagenc — Combined WiFi + User Provisioning (Mobile App Contract)

This is the spec for the avagenc Android app to set up a **fresh device** (no WiFi
yet) in a single QR scan: join the device's hotspot, then push the **home WiFi
credentials + the logged‑in user's id** in one request. No backend involved; the
`user_id` is stored on the device (NVS).

Matches firmware: `main/boards/common/wifi_board.cc` (QR in AP mode) and
`components/78__esp-wifi-connect/wifi_configuration_ap.cc` (`/submit`).

> This is the **first‑time setup** path. For re‑pairing a device that is *already*
> on WiFi (e.g. switch user after Logout), use the LAN flow in `pairing.md`.

---

## 1. Flow

```
 Device (ESP32, no WiFi)                     Phone (avagenc app, user logged in)
 ───────────────────────                     ───────────────────────────────────
  | boot, no saved WiFi                              |
  | start OPEN SoftAP "avagenc-1A2B"                 |
  | show QR { ap_ssid, submit_url, device_id, ... }  |
  |                                                  |
  |                          scan QR ────────────────|  parse JSON
  |                                                  |  join ap_ssid via
  |◄──── phone joins open AP (WifiNetworkSpecifier) ─|  WifiNetworkSpecifier
  |                                                  |  user types home SSID+pass
  |                                                  |  (user_id is implicit)
  |◄──── POST http://192.168.4.1/submit ─────────────|  { ssid, password,
  |      connect to home WiFi (STA)                  |    user_id, display_name }
  |      store user_id in NVS                        |
  |───── 200 { "success": true } ───────────────────►|  unbind AP, show "Done"
  |  exit AP mode, run normally                       |
```

**Two things happen from one POST:** WiFi is configured **and** the user is bound.
After this, the device connects to your realtime bridge and sends the stored
`user_id` as the `X-User-Id` header (see `pairing.md` §7).

---

## 2. The QR payload (shown on the device screen in AP mode)

Compact JSON (UTF‑8). Parse as JSON; don't assume field order.

```json
{
  "v": 1,
  "type": "avagenc_provision",
  "device_id": "a1:b2:c3:d4:e5:f6",
  "device_code": "AVA-7F3K9-Q2A5C",
  "ap_ssid": "avagenc-1A2B",
  "submit_url": "http://192.168.4.1/submit"
}
```

| Field | Type | Notes |
|---|---|---|
| `v` | number | schema version (`1`). Reject unknown majors. |
| `type` | string | always `"avagenc_provision"`. Distinguishes it from `"avagenc_pair"` (the LAN re‑pair QR in `pairing.md`). Branch on this. |
| `device_id` | string | device MAC. Display / sanity only. |
| `device_code` | string | friendly code (`AVA-XXXXX-XXXXX`). Display only. |
| `ap_ssid` | string | **the open hotspot to join.** No password (open network). |
| `submit_url` | string | where to POST credentials. Always `http://192.168.4.1/submit` on this firmware. |

---

## 3. Join the hotspot (Android)

The device AP is **open** (no password). Prefer connecting **programmatically**
with `WifiNetworkSpecifier` (Android 10 / API 29+) so the user never leaves your
app, and so your POST is routed over the hotspot even though it has no internet.

```kotlin
// Android 10+ : request the device's open AP, bind our process to it.
val specifier = WifiNetworkSpecifier.Builder()
    .setSsid(apSsid)          // e.g. "avagenc-1A2B" from the QR
    .build()                  // open network → no passphrase

val request = NetworkRequest.Builder()
    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
    .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) // AP has none
    .setNetworkSpecifier(specifier)
    .build()

val cm = context.getSystemService(ConnectivityManager::class.java)
val callback = object : ConnectivityManager.NetworkCallback() {
    override fun onAvailable(network: Network) {
        cm.bindProcessToNetwork(network)   // route our HTTP over the hotspot
        // ...now POST to submit_url (below), then unbind + unregister.
    }
    override fun onUnavailable() { /* show "couldn't reach device" */ }
}
cm.requestNetwork(request, callback, /*timeoutMs=*/ 15_000)
```

After you get the `/submit` response, **undo the binding** and stop holding the
network so the phone returns to normal WiFi:

```kotlin
cm.bindProcessToNetwork(null)
cm.unregisterNetworkCallback(callback)
```

> Alternative (simpler, less smooth): encode a standard Wi‑Fi QR
> `WIFI:T:nopass;S:avagenc-1A2B;;` so the OS camera offers to join, then run the
> POST. This firmware doesn't emit that format by default — ask if you want it.

---

## 4. `POST {submit_url}` — send WiFi creds + user

**Headers**
```
Content-Type: application/json
```

**Request body**
```json
{
  "ssid": "RumahVico-2G",      // REQUIRED — home WiFi SSID (2.4 GHz, see §6)
  "password": "secret123",     // home WiFi password ("" for open networks)
  "user_id": "usr_123",        // optional but expected — the logged-in user's id
  "display_name": "Vico"       // optional — shown on the device
}
```
- `ssid` ≤ 32 chars; `password` ≤ 64 chars; `user_id`/`display_name` ≤ 127 chars.
- If `user_id` is omitted the device still configures WiFi (identity can be added
  later via the LAN flow), but for the combined flow you should always include it.

**Response 200 — success**
```json
{ "success": true }
```
The device is now connected to the home WiFi **and** has stored the user. The AP
will shut down shortly after — treat 200 as done and disconnect from the AP.

**Response 200 — failure** (note: HTTP is still 200; check the `success` flag)
```json
{ "success": false, "error": "Failed to connect to the Access Point" }
```
| `error` | Meaning / action |
|---|---|
| `Invalid SSID` | SSID missing or > 32 chars. Fix and re‑POST. |
| `Failed to connect to the Access Point` | wrong password, out of range, or a **5 GHz** network. Ask the user to re‑check and re‑POST — the AP is still up. |

Retrying is safe: the device stays in AP mode until a submit succeeds.

---

## 5. Kotlin — the POST

```kotlin
private val http = OkHttpClient.Builder()
    .connectTimeout(5, TimeUnit.SECONDS)
    .readTimeout(15, TimeUnit.SECONDS)   // ConnectToWifi blocks until associated
    .build()

/** Call from inside onAvailable(), after bindProcessToNetwork(network). */
fun provision(submitUrl: String, ssid: String, pass: String,
              userId: String, displayName: String?): Result<Unit> {
    val body = JSONObject().apply {
        put("ssid", ssid)
        put("password", pass)
        put("user_id", userId)
        if (!displayName.isNullOrBlank()) put("display_name", displayName)
    }.toString().toRequestBody("application/json".toMediaType())

    val req = Request.Builder().url(submitUrl).post(body).build()
    return runCatching {
        http.newCall(req).execute().use { resp ->
            val json = JSONObject(resp.body?.string().orEmpty())
            if (json.optBoolean("success")) Unit
            else throw IllegalStateException(json.optString("error", "unknown_error"))
        }
    }
}
```

---

## 6. Caveats (please handle in the app)

- **No internet on the hotspot.** While bound to `avagenc-1A2B` the phone can't
  reach the internet — do all your work (the POST) inside that window, then unbind.
  `removeCapability(NET_CAPABILITY_INTERNET)` above stops Android from dropping the
  network for having no internet.
- **2.4 GHz only.** ESP32‑S3 can't join 5 GHz. If the user's home WiFi is 5 GHz the
  connect fails with `Failed to connect...`. Warn the user, or filter your SSID
  picker to 2.4 GHz where possible.
- **Permissions:** `ACCESS_FINE_LOCATION` (+ `NEARBY_WIFI_DEVICES` on Android 13+)
  are required for WiFi APIs. Request them before scanning/joining.
- **Timing:** `/submit` blocks a few seconds while the device associates to the
  home AP before replying — use a generous read timeout (≥ 10 s).
- **Cleartext HTTP:** the POST is to `http://192.168.4.1`. Allow cleartext for that
  IP via a network security config (see `pairing.md` §4 — add `192.168.4.0/24`).

---

## 7. When to use which QR

| Situation | QR `type` | Flow | Doc |
|---|---|---|---|
| Brand new device, no WiFi | `avagenc_provision` | join AP → POST `/submit` (WiFi + user) | this doc |
| Device already on WiFi, needs/changes user | `avagenc_pair` | same LAN → POST `/api/v1/pair` (user only) | `pairing.md` |

Branch on the `type` field after scanning. A single "Add / set up device" button in
the app can handle both by reading `type`.

---

*Firmware references:* `main/boards/common/wifi_board.cc` (`StartWifiConfigMode`
shows the QR), `components/78__esp-wifi-connect/wifi_configuration_ap.cc`
(`/submit` stores `user_id`), Kconfig `CONFIG_USE_AVAGENC_USER_PAIRING`.
