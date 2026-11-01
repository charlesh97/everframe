# EverFrame ESP32 — Provisioning Redesign Spec

> Status: **Partially implemented / board-tested build checklist**. Updated 2026-10-17
> to the **Bearer-secret model** (see §1 Security). Previous revision was
> `ae0a395`; this supersedes its auth sections.
> Firmware: `embedded/everframe-esp32` (IDF 6.0.2) on XIAO ESP32S3 + 800×480
> 6-color e-paper (7in3e). Current provisioning: `wifi_prov.c` / `wifi_sta.c`
> + `main.c` boot decision. E-paper driver is live (20 MHz / 32 KB DMA).

This doc is the build spec for the 4-step button→QR→Wi-Fi→activation flow
plus the secure device credential. Treat it as the PR description when the
work lands. Companion: `docs/cloud-auth-setup.md` (website + cloud),
`docs/everframe-secure-flow.excalidraw` (visual, 4 phases).

---

## 1) Target flow (with secure auth)

```
BOOT (any wake)
  │
  ├─ Button held 3 s ────────────────────────────────┐
  │                                                  ▼
  │                                          PROVISIONING
  │                                          ┌──────────────────────┐
  │                                          │ 1. Identity          │
  │                                          │    device_ensure_    │
  │                                          │    identity() ensures│
  │                                          │    deviceId 128-bit  │
  │                                          │  + secret 256-bit    │
  │                                          │  (NVS, generated     │
  │                                          │   once, never leaves │
  │                                          │   in clear)          │
  │                                          │    tokenHash=SHA256( │
  │                                          │     secret) for reg  │
  │                                          │ 2. Render QR + AP    │
  │                                          │    e-paper: Wi-Fi QR │
  │                                          │    "WIFI:T:nopass;   │
  │                                          │     S:EverFrame-XXXX;;│
  │                                          │    + text:           │
  │                                          │    "Join EverFrame-  │
  │                                          │     XXXX → 192.168.4.1"│
  │                                          │ 3. Start SoftAP       │
  │                                          │    EverFrame-XXXX    │
  │                                          │    (open, ch 1, /24) │
  │                                          │    DNS *:53 → 4.1    │
  │                                          │    HTTP :80 captive  │
  │                                          └────────┬─────────────┘
  │                                                   │ POST /config SSID+pass
  │                                          ┌────────▼─────────────┐
  │                                          │ 4. Try STA connect    │
  │                                          │    bounded attempt    │
  │                                          │    with status events │
  │                                          │    timeout 30 s       │
  │                                          └────────┬─────────────┘
  │                                                   │
  │                              fail ┌───────────────┴───────────────┐ success
  │                                   ▼                               ▼
  │                          back to (2) QR+AP              REGISTER + CONNECTED
  │                          (keep AP up,                   ┌──────────────────┐
  │                           rewrite e-paper                │ POST /register   │
  │                           "Connect failed"               │ Device {id,      │
  │                           "— retry")                    │  tokenHash,      │
  │                                                         │  code: A7F3K9}   │
  │                                                         │ → Firestore      │
  │                                                         │   pending        │
  │                                                         │                  │
  │                                                         │ "Connected ✓"    │
  │                                                         │ Activation code: │
  │                                                         │   A7F3K9         │
  │                                                         │ [QR: https://    │
  │                                                         │  everframe.hood  │
  │                                                         │  activate?code=  │
  │                                                         │  A7F3K9 ]        │
  │                                                         │ "Enter this at    │
  │                                                         │  charlesd.me/    │
  │                                                         │  activate"       │
  │                                                         └────────┬─────────┘
  │                                                                  │
  │                                                                  ▼
  │                                                         poll /devicePoll
  │                                                         GET + Bearer secret
  │                                                         every 30 s (setup)
  │                                                         or every wake
  │                                                         until claimed
  │                                                         → {claimed:false}
  │                                                         then {hash, url}
  │
  └─ Normal wake (no button) ──► check NVS wifi_ssid/pass ──► STA connect ──►
     if no creds → PROVISIONING(2) above
     if creds but no IP after timeout → fall through to PROVISIONING(2)
     (old creds stay in NVS, form pre-filled)
     if creds + IP → GET /devicePoll + Bearer secret → verify → fetch if needed
```

**Invariants**

- E-paper never blanks on failure. Re-render the QR screen with an error banner.
- One endpoint for all frames: device stores `wifi_ssid/pass + deviceId + secret`
  only. Bucket/token fields are removed from the portal — routing is
  `deviceId → userId` via Firestore, image via signed URL.
- Every poll is authenticated: `Authorization: Bearer <secret>` over TLS.
  `tokenHash = SHA256(secret)` is what Firestore stores — guessing `deviceId`
  without the secret → `401`. Intercepted signed URLs are 15-min capabilities.
- Deep sleep is the resting state. AP mode lives 30 min then deep-sleeps (EXT0
  button wake). Normal cycle is `wake → wifi_init → poll (Bearer) → hash
  compare → wifi_stop → display (~25 s BUSY) → deep sleep`.

---

## 1b) Security model — why this and not bare deviceId

| What | How | Why |
|------|-----|-----|
| Device credential | `deviceId` 128-bit + `secret` 256-bit, both `esp_random()` on first boot, stored in NVS | `deviceId` alone is enumerable (32 hex chars). `secret` is the password. |
| Firestore stores | `tokenHash = SHA256(secret)` hex, never the secret | Dump of Firestore doesn't let an attacker impersonate a device (one-way hash). Same as password hashing. |
| Every poll | `Authorization: Bearer <secret>` over `esp_http_client` + mbedTLS cert bundle (TLS 1.2, 1–2 s handshake) | Server recomputes `SHA256(secret)` and compares to stored `tokenHash`. Mismatch → `401`. No enumeration. |
| Image fetch | Server-side signed URL `?Expires=…&Signature=RSA…`, 15-min expiry, no auth header needed | Capability URL: unguessable, short-lived. Log leak is harmless after expiry. |
| Claim | Website sends its Firebase ID token + activation code to `claimDevice`; the function atomically changes `pending` → `claimed` | Website never sees the device secret and clients cannot list device records. Device discovers claim on its next authenticated poll (`{claimed:true, hash, url}`). |

Attacker table:

| Knows | Can do |
|-------|--------|
| `deviceId` alone | `401` — no secret |
| `deviceId` + `activationCode` | nothing — code is for the website claim, not device auth |
| Firestore `tokenHash` | cannot derive `secret` (SHA256 one-way) |
| Intercepted signed URL | fetch *that one image* for ~15 min, then dead |
| Physical NVS dump | owns *that one device* only |

---

## 2) Verified hardware hooks

| Item | Where | What to verify |
|------|-------|----------------|
| **SW1** (Y78B23110FP, `Button_Switch_SMD:SW_Push_1P1T`) | PCB + continuity/net audit | **XIAO D0 / GPIO1**, active-high through 1 kΩ to 3V3. No external pull-down is visible, so firmware enables the internal pull-down. |
| Wake capable | ESP32-S3 | GPIO1 is RTC-capable. Use EXT0 wake at **high** level. |
| LED | `GPIO21` active-low | Already used as provisioning/connected indicator. Keep. |
| Display | 800×480, 4 bpp = 192 KB, `EPD_SPI_MAX_TRANSFER` 32 KB, S3 `SPI_LL_DMA_MAX_BIT_LEN = 1<<18` bits | QR must be rendered to framebuffer via `GUI_Paint` / `Fonts` (existing `Fonts/*.c`, `GUI/GUI_Paint.c`). |
| PSRAM | XIAO ESP32-S3R8 | 8 MB octal PSRAM at 80 MHz is enabled. The attached board completed the ESP-IDF boot memory test. |

The firmware defaults in `button.h` match this board. A future board revision
can override `BUTTON_GPIO` and `BUTTON_ACTIVE_LEVEL` at build time.

---

## 3) NVS layout (namespace `everframe`)

Current keys used by firmware: `wifi_ssid`, `wifi_pass`, `prov_done`,
`device_id`, `device_secret`, and `activation_code`. `last_image_hash` is still
planned. Legacy bucket/device-name keys may remain in existing NVS but are no
longer read or written by the portal.

**After this redesign**

| Key | Type | Purpose |
|-----|------|---------|
| `wifi_ssid` | str 32 | STA SSID (portal input) |
| `wifi_pass` | str 64 | STA password |
| `device_id` | str 33 | 128-bit hex (32 chars + NUL), generated once, lower-case, persistent |
| `device_secret` | str 65 | 256-bit hex (64 chars + NUL), generated once, never leaves except as Bearer over TLS |
| `activation_code` | str 7 | 6-char uppercase derived from `device_id` (first 6, uppercased) — deterministic, no server round-trip |
| `prov_done` | u8 | 1 = has Wi-Fi creds (boot chooses STA); 0/unset = provisioning |
| `last_image_hash` | str 64 | SHA of last displayed bin — skip refresh if unchanged |
| _(remove)_ `bucket_name`, `bucket_token` | — | No longer per-device. Cloud resolves `deviceId → userId → bucket path`. Keep read fallback for 1 boot during migration, then delete. |
| `device_name` | str 63 | Optional, keep if you want per-frame naming from portal (defer). |

`device_secret` generation: `esp_random()` × 8 → 32 bytes → hex. `tokenHash`
is computed on-device with `mbedtls_sha256()` only at registration time (or
recomputed each poll — cheap, ~µs). Never log either value.

---

## 4) File-level changes (checklist)

All paths under `embedded/everframe-esp32/main/`.

### 4a) New modules

- **`button.h/.c`** — `button_init()`, `button_was_held_on_boot(ms)`,
  `button_poll_held(ms)` (debounced), `button_enable_wakeup()` for deep sleep.
  Handles active-high + internal pull-down, 50 ms debounce, 3000 ms threshold.
  Deep-sleep wake via `esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 1)`.

- **`display_provisioning.h/.c`** — owns all e-paper screens in this flow.
  It uses the managed `espressif/qrcode` component and a compact built-in 3×5
  uppercase font, rendering directly into the native 4-bpp PSRAM frame. Screens:
  - `display_show_qr_ap(ssid)` — white background, centered QR (Wi-Fi), SSID
    text, "→ http://192.168.4.1"
  - `display_show_connecting(ssid)` — spinner/text "Connecting to <SSID>…"
    *(still missing)*
  - `display_show_connected(code)` — "Connected ✓", activation code large font,
    claim QR (`https://everframe.hoodcharlesd.me/activate?code=CODE`)
  - `display_show_connect_failed(ssid)` — error banner + QR again *(still missing)*
  Uses existing `EPD_Task` pattern but called synchronously: `EPD_Module_Init()`,
  `EPD_7IN3E_Init()` / `EPD_7IN3E_Clear(WHITE)` / `EPD_7IN3E_Display(frame)` with
  `EPD_SPI_WriteData` 32 KB chunks, then `EPD_7IN3E_Sleep()` / `EPD_Module_Exit()`
  or keep awake during AP mode (trade power vs 25 s per refresh).

- **`device_identity.h/.c`** — `device_ensure_identity()` — reads
  `device_id` + `device_secret` from NVS or generates both + commits.
  Derives and stores `activation_code` (first 6 of `device_id`, uppercased).
  Computes `tokenHash = hex(SHA256(device_secret))` on demand for
  `registerDevice`. Logs `deviceId` and `activationCode` once at `INFO`, never
  logs `secret` or `tokenHash`.

### 4b) Modified modules

- **`main.c`**
  - At top of `app_main()` (after `nvs_flash_init()`), call
    `button_init()` + `device_ensure_identity()`.
  - Check `gpio_get_level(BUTTON_GPIO) == 1` for 3 s **before** the
    `provisioning_is_complete()` branch. If held, set `force_provisioning`
    and branch to `wifi_prov_start()` regardless of NVS. 10 s long-hold →
    `nvs_flash_erase()` + regenerate identity (factory reset).
  - Enable button wake before every `esp_deep_sleep_start()` in the idle paths.
  - Uncomment `EPD_Task` was disabled — provisioning screens now own display.

- **`wifi_prov.h/.c`** — big trim:
  - Remove form fields `bucket_name`, `bucket_token`, `device_name` from
    `SETUP_HTML` (keep `wifi_ssid`, `wifi_pass` only). Shrink `FORM_BODY_MAX_LEN`
    to ~600. New form: 2 inputs + helper text.
  - `save_configuration()` now only writes `wifi_ssid`, `wifi_pass` (+ `prov_done=1`
    last + `nvs_commit`). Never logs password.
  - Change success path: **do not `esp_restart()`**. Instead:
    1. `wifi_sta_try_once(ssid, pass, timeout_ms)` to validate.
    2. On success: `cloud_register_device(deviceId, tokenHash, activationCode)`
       via `esp_http_client` POST `.../registerDevice` (TLS + cert bundle).
       On `200` → `display_show_connected(code)`. `409` means the same
       `deviceId` exists with a different token hash and requires a factory reset.
    3. Enter poll loop: `GET /devicePoll` with `Authorization: Bearer <secret>`
       every 30 s until `claimed==true` or 30 min timeout → deep sleep.
  - On Wi-Fi failure → `display_show_connect_failed(ssid)` + loop back to QR+AP
    state (keep HTTP/DNS alive, do not reboot). On `401` from cloud → log
    `tokenHash mismatch — factory reset?` and re-register.
  - At startup, call `display_show_qr_ap(softap_ssid)` before `start_webserver()`
    / `start_dns_server()`. AP SSID is `EverFrame-<last4MAC>` as today
    (`make_softap_ssid()`).
  - AP idle timeout: `vTaskDelay` loop already exists — add 30-min counter then
    `esp_deep_sleep_start()` with button wake. Pending poll continues during
    the 30 min window.

- **`wifi_sta.h/.c`**
  - Extract `wifi_sta_try_once()` (blocking connect with timeout, returns
    `ESP_OK` / `ESP_ERR_TIMEOUT`) for use by `wifi_prov` without entering the
    infinite retry loop.
  - Keep existing `wifi_sta_start()` for normal boots, but add bounded fallback:
    if no IP within 30 s, return `ESP_ERR_TIMEOUT` so `main.c` can fall through
    to provisioning UI. This initial timeout is implemented; post-connect link
    loss keeps the existing retry loop. Add `Authorization` header helper for
    cloud polls.
  - Shared helper: `cloud_poll_device(deviceId, secret, out_claimed, out_hash, out_url)`
    — does `GET /devicePoll` with `Authorization: Bearer <secret>` +
    `X-Device-Id: <deviceId>`, parses JSON, handles `401`.

- **`cloud_client.h/.c`** *(new, or inside wifi_prov)*
  - `cloud_register_device()` — `POST /registerDevice` JSON
    `{deviceId, tokenHash, activationCode}` (no auth — idempotent create).
    Server stores `tokenHash`; idempotent same-identity registration returns
    `200`. `409` is an identity conflict, not success.
  - `cloud_poll_device()` — `GET /devicePoll` with Bearer, returns
    `{claimed, hash, signedUrl}` or `{claimed:false}`.
  - `cloud_fetch_image(signedUrl, out_buf)` — plain `GET` to
    `storage.googleapis.com` (no auth, URL is capability). Verify `Content-Length`
    == 192000 before display.

- **`ePaper_Config.h/.c`**
  - No pin change for this flow. Ensure `EPD_*` init can be called from
    `display_provisioning` without colliding with `wifi_prov` task. SPI bus is
    shared — provision display runs before Wi-Fi or with STA stopped.

- **`CMakeLists.txt`** — add new sources:
  `button.c`, `display_provisioning.c`, `device_identity.c`, `cloud_client.c`.
  Add `PRIV_REQUIRES esp_driver_gpio mbedtls espressif__qrcode`; pin
  `espressif/qrcode ~0.1.0` in `idf_component.yml`.

- **`sdkconfig`** — enable `CONFIG_ESP_WIFI_AUTH_WPA2_PSK` already set. Ensure
  `CONFIG_MBEDTLS_HAVE_TIME` for `esp_crt_bundle_attach` and cert bundle.
  `CONFIG_FREERTOS_HZ` sufficient for button polling.

### 4c) QR generation

Use Espressif's managed `espressif/qrcode` component, which wraps Nayuki's
[QR-Code-generator](https://github.com/nayuki/QR-Code-generator). The dependency
is locked in `dependencies.lock`; its callback API lets the firmware paint each
module directly into the panel framebuffer.

- Version: QR v3–v5 for short payloads (`WIFI:T:nopass;S:EverFrame-XXXX;;` ~30 B
  and `https://everframe.hoodcharlesd.me/activate?code=A7F3K9` ~55 B). ECC `M`.
- Rendering: `esp_qrcode_generate()` invokes a display callback; use
  `esp_qrcode_get_size()` / `esp_qrcode_get_module()` and paint scaled black
  rectangles, centered with a four-module white quiet zone. The final frame is
  packed two 4-bit 7in3e palette pixels per byte.

### 4d) QR payloads

For open AP `EverFrame-XXXX`:

```
WIFI:T:nopass;S:EverFrame-XXXX;;        // minimal, most phones accept
```

If any phone fails to auto-join, fallback to plain URL QR (`http://192.168.4.1`)
is acceptable — phone joins manually then scans URL. Primary is Wi-Fi QR.

Claim QR (activation code display):

```
https://everframe.hoodcharlesd.me/activate?code=A7F3K9
```

Frontend route `/activate` reads `code` query, prompts Google login if needed,
then runs the claim transaction (see `docs/cloud-auth-setup.md` §2e).

---

## 5) Poll behavior detail (new for Bearer model)

```
Setup (pending)          Every 30 s: GET /devicePoll + Bearer secret
  → server: SHA256(secret) == tokenHash?  no → 401 (log, re-register)
                                          yes + status==pending → {claimed:false}
                                              display stays on QR+AP
                                              AP lives 30 min then deep sleep
                                              button re-wakes

Claimed                  Every wake (or 30 s during setup window after claim):
  → server: verify Bearer, lookup userId, read users/{uid}/manifest.json
          → compare manifest.hash vs device.lastImageHash (from pending doc or
            from device's NVS last_image_hash)
          → if equal → {upToDate:true} → sleep, no download
          → else → {hash, url: signed 15-min} → GET signed URL (no auth)
                  → verify 192000 bytes → display → save last_image_hash

Fail paths               No Wi-Fi → back to QR+AP (never blank)
                         401 → re-POST /registerDevice; 409 → factory reset
                         No manifest → {hasImage:false} → keep last image, sleep 1 h
```

`Authorization` header format:

```
Authorization: Bearer <64-hex-secret>
X-Device-Id: <32-hex-deviceId>
```

Server MUST use constant-time comparison for the hash.

---

## 6) Button behavior detail

- **Normal boot, no press:** `provisioning_is_complete()` decides STA vs AP as today.
- **Short button press from deep sleep:** continue through the normal saved-Wi-Fi
  path, poll the cloud immediately, refresh if the hash changed, then sleep again.
- **Button held 3 s at boot (or anytime before wifi init):** force `wifi_prov_start()`
  + render QR. Debounce 50 ms, require continuous high for 3000 ms.
- **10 s hold (factory):** `nvs_flash_erase()` + regenerate `deviceId+secret`,
  reboot into provisioning. Log `factory reset`.
- **Deep sleep:** before every `esp_deep_sleep_start()`, call
  `esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 1)` (or `ext1` with mask). On wake,
  `esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0` → inspect the hold
  duration. A released/short press performs a normal cloud check; a continuous
  3 s hold enters provisioning.

Test matrix:

- [x] Short tap from deep sleep wakes, polls immediately, skips the unchanged
  hash, and returns to sleep (verified on the attached board).
- [ ] 3 s hold from deep sleep wakes and shows QR.
- [ ] 3 s hold from running STA forces AP+QR without erasing creds (form pre-filled
  if `wifi_ssid` exists — nice to have).
- [ ] 10 s hold erases and shows new activation code (different `deviceId`+`secret`).
- [x] No button → normal STA connect works (`Wi-FightTheFeeling2` verified on the attached board).
- [ ] Pending poll with correct secret → `200 {claimed:false}`. Wrong secret → `401`.
- [ ] After website claim, next poll → `200 {claimed:true, hash, url}`.

---

## 7) UI copy / e-paper notes

The current implementation uses a compact built-in 3×5 font so the provisioning renderer has no dependency on the demo font tables. Keep the Wi-Fi QR ≥ 180 px.

```
[QR]                  ← Wi-Fi join
EverFrame-XXXX
Go to http://192.168.4.1
Enter your home Wi-Fi

— after Wi-Fi success + register —

Connected ✓  (large, centered)
Your code:  A7F3K9   (font24, 2× scale)
[QR]  everframe.hoodcharlesd.me/activate?code=A7F3K9
Enter this code at everframe.hoodcharlesd.me/activate
(while signed in with Google)

— after failure —

Could not connect to "<SSID>"
Check password and try again.     ← small
[QR again]

— pending (polling) banner —

Waiting for link…
Enter A7F3K9 at everframe.hoodcharlesd.me/activate
```

The e-paper holds its last image with zero power — failed states must still
rewrite the screen so the user sees the failure. Use `EPD_7IN3E_Clear(WHITE)`
before each screen.

---

## 8) Implementation status and remaining order

1. [x] `device_identity` + button boot paths. Identity and PSRAM are verified on
   the attached board; physical 3 s/10 s and EXT0 wake tests remain.
2. [x] Add a standalone `cloud_client` for register, Bearer poll, signed download,
   exact-size validation, and streaming SHA-256 validation. It builds but is not
   called by `main.c` yet.
3. [x] Trim the portal to SSID/password and add `wifi_sta_try_once()`.
4. [x] Add `display_provisioning` + Espressif QR generation. The Wi-Fi QR was
   encoded into a 192 KB PSRAM frame and refreshed successfully on the 7in3e;
   the activation screen builds but still needs flow-level exercise.
5. [ ] Replace portal restart with AP teardown → bounded STA try → AP recovery.
6. [ ] Add the device state-machine owner that configures cloud endpoints,
   registers, polls, downloads into PSRAM, displays, commits `last_image_hash`
   only after success, stops Wi-Fi, arms GPIO1-high wake, and deep-sleeps.
7. [ ] With cloud access, deploy functions/rules and run pending → claim → image
   end-to-end tests, including 401, 409, bad hash, truncated frame, and timeout.

---

## 9) Open questions (answer before PR)

- [x] Confirm SW1 GPIO and active level: GPIO1/D0, active-high, internal pull-down.
- [x] QR quiet-zone / centering on 800×480 — runtime scaling preserves the four-module quiet zone and leaves a wide display border.
- [ ] AP lifetime: 30 min vs never-sleep while on USB power? `VBAT` detect could
  choose. Simplest: 30 min always, button re-wakes.
- [ ] `device_secret` entropy: 256-bit hex vs 128-bit? 256-bit chosen for
  brute-force margin; 128-bit also fine for this threat model — either is ok,
  just keep the hex length consistent (64 vs 32 chars) and update the verify.
- [ ] `tokenHash` truncation: full SHA256 hex (64 chars) vs 32-char prefix?
  Full is recommended — Firestore field is cheap.
