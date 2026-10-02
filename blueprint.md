# avagenc — Pairing Web App Blueprint

Technical blueprint for the **avagenc web app** that pairs a physical device
(Waveshare ESP32‑S3, xiaozhi firmware) to a logged‑in avagenc user account.

This is the *frontend* companion to [`backend_pairing.md`](./backend_pairing.md)
(the REST contract) and the firmware (`main/device_binding.cc`,
`main/system_info.cc` → `GetDeviceCode`). Read those first — this document assumes
those endpoints exist and only describes the web side.

> **The web app's single job in the pairing flow:** take a logged‑in user + a
> `(device_id, pairing_code)` pair (from a scanned QR or typed code), confirm intent,
> and call `POST /api/v1/devices/bind` with the *user's* auth. Everything else
> (token issuance, device→user mapping) is backend. The web app never talks to the
> device directly.

---

## 1. Where this fits

```
 ESP32 LCD                    User's phone                 avagenc web app            avagenc backend
 ─────────                    ────────────                 ───────────────            ───────────────
 shows QR(pair_url) ───scan──►  opens URL ───────────────►  /pair?d=..&c=..                 |
   + short code                                             │ logged in? ──no──► /login ────► auth
   AVA-XXXXX-XXXXX                                           │ yes                            |
                                                             │ show confirm card              |
                                                             │ user taps "Pair" ─── POST /devices/bind ──►  writes device↔user
                                                             │                                             mints device_token
                                                             │ ◄──────────── 200 success ─────────────────|
 GET /status (poll) ◄──────────────────────────────────────────────────── bound:true + token ────────────|
 saves token, hides QR
```

The QR on the LCD encodes **`pair_url`**, which the backend built as:

```
https://app.avagenc.com/pair?d=<device_id>&c=<pairing_code>
```

So the web app's entry point is the route **`/pair`** reading `d` (device id, MAC with
colons) and `c` (short pairing code). That's the contract you must not break.

---

## 2. Scope — what the web app must contain

**Must‑have (pairing MVP):**
1. **`/pair`** — the deep‑link landing page. Reads `d` + `c`, gates on login, shows a
   confirm card, calls `bind`, renders success/error states.
2. **Auth** — login/session so `POST /devices/bind` can carry the user's bearer token.
   Must support "return to `/pair?...` after login" (redirect preservation).
3. **Manual code entry** — `/pair` with no/partial params, or a dedicated
   `/pair/manual`, where the user types the short code (camera‑can't‑scan fallback).
4. **Result states** — success, already‑bound, expired code, invalid code, network error.

**Should‑have (device management):**
5. **`/devices`** — list the user's bound devices (from a backend "my devices" endpoint),
   show name/model/last‑seen, and **unbind** (`POST /api/v1/devices/unbind`).
6. **Rename / friendly name** per device (optional backend field).

**Nice‑to‑have:**
7. In‑browser QR scanner (`/pair/scan`) so the *phone's* browser can scan a second
   device's LCD without a native app.
8. i18n (Indonesian default + English).

---

## 3. Recommended tech stack

| Concern | Recommendation | Why |
|---|---|---|
| Framework | **Next.js 14/15 (App Router) + TypeScript** | File routing gives you `/pair` and `/devices` cheaply; server components + route handlers let you keep the user token server‑side (never exposed to JS); great for deep‑link SEO/meta and universal‑link `.well-known` files. |
| Styling | **Tailwind CSS** | Matches the dark avagenc theme already used in the captive‑portal HTML (indigo→cyan accent). Fast to reach parity. |
| Data fetching | **TanStack Query** (client) + Next route handlers (server proxy) | Polling, retry, and cache states map cleanly onto the pairing UX. |
| Auth | Your existing avagenc auth. If none yet: **Auth.js (NextAuth)** or a hosted IdP (Clerk/Supabase Auth). Token stored in an **httpOnly cookie**, not localStorage. |
| QR scanning (optional) | `@zxing/browser` or `html5-qrcode` | Pure‑web camera scan for the `/pair/scan` path. |
| Deploy | Vercel / Netlify / container behind your existing infra | App Router works anywhere Node runs; static‑export is *not* enough because you need the server‑side bind proxy. |

**Alternative if you want to stay minimal / no Node runtime:** a single‑page
**Vite + React** app that calls the backend directly from the browser. This is simpler
to host (static files + your API) but the user token lives in the browser and the bind
call is a direct CORS request to the backend. Acceptable, but the Next.js "server proxy"
pattern below is safer. **Pick Next.js unless you have a strong reason not to.**

The rest of this blueprint assumes **Next.js App Router + TypeScript + Tailwind**.

---

## 4. Route map

```
app/
  layout.tsx                # theme, fonts, <Providers> (QueryClient, auth)
  page.tsx                  # marketing / redirect to /devices when logged in
  login/
    page.tsx                # email/OTP/OAuth; preserves ?next=
  pair/
    page.tsx                # ★ deep-link landing: reads ?d=&c=, the state machine
    manual/
      page.tsx              # type the short code (fallback)
    scan/
      page.tsx              # optional in-browser QR scanner
  devices/
    page.tsx                # list bound devices, unbind
  api/                      # Next route handlers = server-side proxy to backend
    bind/route.ts           # POST -> backend /devices/bind (adds user bearer)
    devices/route.ts        # GET  -> backend "my devices"
    unbind/route.ts         # POST -> backend /devices/unbind
lib/
  backend.ts                # typed fetch wrappers to the avagenc backend
  auth.ts                   # session/user-token helpers (server)
  device-id.ts              # parse/normalize/format device_id + device_code
components/
  PairCard.tsx  DeviceRow.tsx  CodeInput.tsx  StatusScreen.tsx  Brand.tsx
```

**Why the `app/api/*` proxy layer:** the browser calls *your own* Next server, which
attaches the user's session token and forwards to the avagenc backend. The user token
never reaches client JS, CORS is a non‑issue, and you can add rate‑limiting/validation
in one place. If you go the Vite/SPA route, drop this layer and call the backend
directly with the token from the auth SDK.

---

## 5. The `/pair` flow — state machine

`/pair?d=<device_id>&c=<pairing_code>` drives a small, explicit state machine. Model it
as one discriminated union so every screen is exhaustive:

```
type PairState =
  | { k: 'parsing' }              // reading + validating query params
  | { k: 'invalid_params' }       // missing/garbage d or c  -> offer manual entry
  | { k: 'need_login' }           // not authenticated -> redirect to /login?next=
  | { k: 'confirm' }              // logged in, show "Pair AVA-XXXXX-XXXXX?"
  | { k: 'binding' }              // POST /bind in flight
  | { k: 'success' }              // bound; tell user the device will update shortly
  | { k: 'already_bound_self' }   // this user already owns it -> success-ish
  | { k: 'error_expired' }        // 410 -> ask them to look at the device for a new code
  | { k: 'error_not_found' }      // 404 -> code wrong/expired session
  | { k: 'error_conflict' }       // 409 -> bound to a different account
  | { k: 'error_network' }        // transient -> retry button
```

**Transitions**

1. `parsing`: validate `d` with `isValidDeviceId` and `c` with `isValidPairingCode`.
   - invalid → `invalid_params` (link to `/pair/manual`).
2. Check session (server component or `useSession`).
   - not logged in → store the full current URL and go to `/login?next=<encoded>`; on
     return, resume at `confirm`.
3. `confirm`: render a card with the human device code (`AVA-XXXXX-XXXXX`, derived from
   `d` — see §8) and a **Pair** button. Show which account they're binding to
   ("Signed in as user@email"). This explicit confirm step is the anti‑mistake guard.
4. On **Pair** → `binding` → `POST /api/bind { device_id, pairing_code }`.
5. Map response → `success` / `already_bound_self` / `error_*` (see §9 mapping table).
6. `success`: copy = "Device paired. Your avagenc device will finish setting up in a few
   seconds." (The device's own `/status` poll flips its screen — the web app does **not**
   poll the device.)

**Important UX note:** the web app has *no* signal from the device. Do not build a
"waiting for device to confirm" spinner that polls anything — once `bind` returns 200,
you're done; the device picks it up on its next poll. Optionally you *can* poll a
backend "is this device bound" read to show a green "device is online" checkmark, but
that's cosmetic.

---

## 6. Auth & the `next` redirect

The whole flow hinges on the user being logged in *before* `bind`. Requirements:

- `/login` accepts `?next=<url-encoded return path>` and, after successful auth,
  `router.replace(next)`. Validate `next` is a **same‑origin relative path** (starts with
  `/pair`) to avoid open‑redirect.
- Prefer an **httpOnly, Secure, SameSite=Lax cookie** session. The `bind` proxy reads it
  server‑side.
- If a user follows the QR while logged out on their phone, the sequence is:
  `/pair?d=&c=` → `/login?next=/pair?d=&c=` → auth → back to `/pair?d=&c=` at `confirm`.
  Preserve the query string exactly (it carries the pairing code).

---

## 7. API layer

### 7.1 Server proxy — `app/api/bind/route.ts`

```ts
import { NextRequest, NextResponse } from 'next/server';
import { getServerSession } from '@/lib/auth';
import { normalizeDeviceId, isValidPairingCode } from '@/lib/device-id';

const BACKEND = process.env.AVAGENC_BACKEND_URL!; // e.g. https://api.avagenc.com

export async function POST(req: NextRequest) {
  const session = await getServerSession();
  if (!session) {
    return NextResponse.json({ error: 'unauthorized' }, { status: 401 });
  }

  const { device_id, pairing_code } = await req.json();
  const id = normalizeDeviceId(device_id);
  if (!id || !isValidPairingCode(pairing_code)) {
    return NextResponse.json({ error: 'invalid_request' }, { status: 400 });
  }

  const res = await fetch(`${BACKEND}/api/v1/devices/bind`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      Authorization: `Bearer ${session.accessToken}`, // the USER's token
    },
    body: JSON.stringify({ device_id: id, pairing_code }),
  });

  const data = await res.json().catch(() => ({}));
  return NextResponse.json(data, { status: res.status }); // pass status through for §9
}
```

### 7.2 Client call (TanStack Query)

```ts
function useBindDevice() {
  return useMutation({
    mutationFn: async (v: { device_id: string; pairing_code: string }) => {
      const r = await fetch('/api/bind', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(v),
      });
      const body = await r.json().catch(() => ({}));
      if (!r.ok) throw Object.assign(new Error(body.error ?? 'error'), { status: r.status, body });
      return body;
    },
  });
}
```

The same proxy pattern applies to **`/api/devices`** (GET, list) and **`/api/unbind`**
(POST). All three attach the user bearer server‑side.

---

## 8. `device_id` / `device_code` helpers — `lib/device-id.ts`

The firmware sends the MAC as `device_id` (lowercase hex, colon‑separated) and shows a
friendly `device_code` = `AVA-XXXXX-XXXXX` (Crockford base32 of the 48‑bit MAC, see
`main/system_info.cc::GetDeviceCode`). The web app should:

- **Normalize** any incoming `d`: lowercase, strip whitespace, accept both `a1:b2:...`
  and `a1b2...` and re‑insert colons. Reject if not 12 hex nibbles.
- **Derive the friendly code from the MAC** so the confirm card can show
  `AVA‑XXXXX‑XXXXX` (matches what's printed on the LCD → user recognizes it). Port
  `GetDeviceCode` to TS (same Crockford alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ`,
  pack 48 bits MSB‑first, emit 10 symbols, format `AVA-` + `[0:5]` + `-` + `[5:10]`).

```ts
const B32 = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'; // Crockford, matches firmware

export function normalizeDeviceId(raw: string): string | null {
  const hex = (raw ?? '').toLowerCase().replace(/[^0-9a-f]/g, '');
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g)!.join(':');
}

export function deviceCodeFromId(deviceId: string): string | null {
  const hex = deviceId.replace(/[^0-9a-f]/gi, '');
  if (hex.length !== 12) return null;
  let v = BigInt('0x' + hex);            // 48-bit MAC
  const sym: string[] = [];
  for (let i = 0; i < 10; i++) {         // 10 symbols * 5 bits = 50 bits, MSB-first
    sym.unshift(B32[Number(v & 31n)]);
    v >>= 5n;
  }
  const s = sym.join('');
  return `AVA-${s.slice(0, 5)}-${s.slice(5, 10)}`;
}

export const isValidPairingCode = (c: string) => /^[0-9A-HJ-NP-TV-Z]{6,8}$/i.test(c ?? '');
```

> Keep this in lockstep with the firmware. If you ever change the shift width or symbol
> count in `GetDeviceCode`, change it here. (Add a couple of golden test vectors on both
> sides so they can't drift.)

---

## 9. Error mapping (backend → UI)

Map `POST /devices/bind` responses (see `backend_pairing.md` §7) to states:

| HTTP / `error`            | State                | Message to user |
|---|---|---|
| `200 success:true`        | `success`            | "Device paired. It'll finish setup in a few seconds." |
| `200` (already yours)     | `already_bound_self` | "This device is already linked to your account." |
| `400 invalid_request`     | `invalid_params`     | "That link looks incomplete. Enter the code shown on your device." |
| `401 unauthorized`        | `need_login`         | (bounce to `/login?next=`) |
| `404 pairing_not_found`   | `error_not_found`    | "We couldn't find that code. Check the device screen for the current code." |
| `409 already_bound`       | `error_conflict`     | "This device is linked to another account. Ask the owner to unlink it first." |
| `410 pairing_expired`     | `error_expired`      | "That code expired. Your device shows a fresh one — scan it again." |
| `429 rate_limited`        | `error_network`      | "Too many attempts. Wait a moment and try again." |
| network / 5xx             | `error_network`      | Retry button. |

Every error state (except `need_login`) should offer **"Enter code manually"** and
**"Scan again"** as escape hatches.

---

## 10. Manual entry — `/pair/manual`

For "camera won't scan". A single `CodeInput` (segmented 6–8 char box, auto‑uppercase,
Crockford‑filtered) plus, when the URL had a `d`, we already know the device; when it
didn't, the user only has the short code and the backend must resolve `(code)` alone.

- If `d` is known → `POST /bind { device_id: d, pairing_code }`.
- If only the code is known → the backend must accept `bind` by bare code (backend
  contract §5 keeps codes unique among active sessions specifically to allow this).
  Confirm with backend which mode they implement; prefer always carrying `d` when the
  deep link provided it.

---

## 11. Device management — `/devices` (should‑have)

- `GET /api/devices` (proxy → backend "my devices") → list `{ device_id, device_code,
  model, name?, last_seen, bound_at }`.
- Row actions: **Rename** (optional backend PATCH), **Unbind** (`POST /api/unbind`
  → backend `/devices/unbind`), with a confirm dialog ("This will disconnect the device;
  it'll need to be paired again").
- Empty state links to "Pair a device" → explains looking at the device screen.

> Requires a backend "list my devices" endpoint that isn't in `backend_pairing.md` yet —
> add `GET /api/v1/me/devices` (user‑authed) to that contract if you build this page.

---

## 12. Mobile: web page vs native deep link

`pair_url` is a plain `https://app.avagenc.com/pair?...`. Two deployment options:

- **Web‑only (recommended to start):** the QR opens the mobile browser straight to
  `/pair`. Zero app install. This blueprint fully covers it.
- **Native app with universal/app links:** if/when you ship an avagenc mobile app, serve
  `/.well-known/apple-app-site-association` and `/.well-known/assetlinks.json` so the OS
  opens the app on `/pair` and falls back to the web page if the app isn't installed. The
  in‑app pairing screen does the exact same `bind` call. Keep the web `/pair` as the
  universal fallback either way.

---

## 13. Branding & theme

Match the captive‑portal HTML already themed in
`managed_components/78__esp-wifi-connect/assets/wifi_configuration.html`:

- Dark background `#0b1020`, surface `#161d33`, accent indigo `#6366f1` → cyan `#22d3ee`,
  danger `#fb7185`. Reuse as Tailwind theme tokens (`--ava-*`).
- Wordmark "avagenc" + placeholder "A" logo mark (swap for the real asset when provided).
- Big, legible confirm card; the device code shown in a monospace pill so it visually
  matches the LCD.

---

## 14. Security (web side)

- **User token stays server‑side** (httpOnly cookie + `app/api/*` proxy). Never put it in
  localStorage or a client‑readable place.
- **Validate `next`** on login redirect: same‑origin, must start with `/pair`. Blocks
  open‑redirect via the deep link.
- **Validate `d` and `c`** before calling the backend (regex in §8) — cheap DoS/garbage
  filter.
- **CSRF:** the bind proxy is a state‑changing POST; use SameSite=Lax cookies + a CSRF
  token (Auth.js provides one) or double‑submit. Because it's same‑site fetch, Lax is
  usually sufficient, but add the token for defense in depth.
- **Don't leak ownership:** on `409`, say "linked to another account" — never reveal
  *which* account/email.
- **Rate‑limit** the proxy routes (per session/IP) to mirror backend limits.
- A pairing code only lets someone claim a device into **their own** account (grants no
  access to other users' data) — but still treat codes as short‑lived secrets in logs
  (don't log full `c`).

---

## 15. Config — `.env`

```
AVAGENC_BACKEND_URL=https://api.avagenc.com      # server-side only (no NEXT_PUBLIC_)
NEXT_PUBLIC_APP_URL=https://app.avagenc.com      # for building absolute links
AUTH_SECRET=...                                   # Auth.js / session signing
# + your IdP creds if using OAuth
```

Keep `AVAGENC_BACKEND_URL` **server‑only** so the browser never sees the backend origin
directly. `app.avagenc.com` (this web app) and `api.avagenc.com` (backend) should be
distinct hosts; the LCD's `pair_url` points at `app.*`.

---

## 16. Accessibility & i18n

- Indonesian default (device caption is Indonesian: "Pindai untuk menghubungkan ke akun
  avagenc"), English secondary. Use `next-intl` or a simple dictionary.
- Confirm/success/error screens must be keyboard‑navigable and screen‑reader labelled;
  the `CodeInput` needs an accessible single logical input (don't ship 8 disconnected
  boxes without ARIA).
- Respect `prefers-color-scheme` but the brand is dark‑first.

---

## 17. Testing

- **Unit:** `deviceCodeFromId` golden vectors shared with firmware; `normalizeDeviceId`
  edge cases; `isValidPairingCode`.
- **State machine:** table test each `PairState` transition against mocked `bind`
  responses (200/400/401/404/409/410/429/5xx).
- **E2E (Playwright):** logged‑out → scan link → login → confirm → success; expired code
  path; manual entry path. Mock the backend with MSW.
- **Cross‑check with firmware:** stand up a stub backend (see next section) and run the
  real device against the real web app once.

---

## 18. Build order (recommended)

1. Scaffold Next.js + Tailwind + theme tokens; `Brand`, `StatusScreen`.
2. `lib/device-id.ts` + tests (get parity with firmware first — cheap and de‑risks).
3. Auth + `/login?next=` round‑trip.
4. `app/api/bind` proxy + `useBindDevice`.
5. `/pair` state machine (§5) + error mapping (§9).
6. `/pair/manual`.
7. `/devices` list + unbind (needs the backend list endpoint).
8. Optional `/pair/scan`, native universal links, i18n polish.

---

## 19. Implementation checklist

- [ ] `/pair` reads `?d=&c=`, validates, gates on login, confirms, binds, renders all states in §5/§9.
- [ ] `/login?next=` preserves and safely returns to the pairing URL.
- [ ] `app/api/bind` proxy attaches the **user** bearer server‑side; passes backend status through.
- [ ] `lib/device-id.ts` parity with `GetDeviceCode` (+ golden tests both sides).
- [ ] Manual entry (`/pair/manual`) with Crockford‑filtered `CodeInput`.
- [ ] Full error mapping (expired/not‑found/conflict/rate‑limit/network) with escape hatches.
- [ ] `/devices` list + unbind (add backend `GET /me/devices`).
- [ ] Dark avagenc theme parity with the captive portal.
- [ ] Security: httpOnly session, `next` allow‑list, CSRF, input validation, rate limit, no code/token logging.
- [ ] E2E happy path run against the real device once end‑to‑end.

---

*Contract references:* [`backend_pairing.md`](./backend_pairing.md) (REST),
`main/device_binding.cc` (device‑side flow), `main/system_info.cc::GetDeviceCode`
(friendly code), `main/protocols/websocket_protocol.cc` (runtime Bearer token).
