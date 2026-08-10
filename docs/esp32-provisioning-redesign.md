# EverFrame ESP32 — Provisioning Redesign Spec

> Status: **Spec / build checklist** — no code changed yet.
> Firmware: `embedded/everframe-esp32` (IDF 6.0.2) on XIAO ESP32S3 + 800×480
> 6-color e-paper (7in3e). Current provisioning: `wifi_prov.c` / `wifi_sta.c`
> + `main.c` boot decision. E-paper driver is live (20 MHz / 32 KB DMA).

This doc is the build spec for the 4-step flow you just defined plus the
activation-code display. Treat it as the PR description when the work lands.

---

## 1) Target flow

```
BOOT (any wake)
  │
  ├─ Button held 3 s ────────────────────────────────┐
  │                                                  ▼
  │                                          PROVISIONING
  │                                          ┌──────────────────────┐
  │                                          │ 1. Render QR + AP    │
  │                                          │    e-paper: Wi-Fi QR  │
  │                                          │    "WIFI:T:nopass;   │
  │                                          │     S:EverFrame-XXXX;;│
  │                                          │    + text:           │
  │                                          │    "Join EverFrame-  │
  │                                          │     XXXX → 192.168.4.1"│
  │                                          │ 2. Start SoftAP       │
  │                                          │    EverFrame-XXXX    │
  │                                          │    (open, ch 1, /24) │
  │                                          │    DNS *:53 → 4.1    │
  │                                          │    HTTP :80 captive  │
  │                                          └────────┬─────────────┘
  │                                                   │ POST /config SSID+pass
  │                                          ┌────────▼─────────────┐
  │                                          │ 3a. Try STA connect   │
  │                                          │    5 fast retries,    │
  │                                          │    then 5 s slow      │
  │                                          │    timeout 15 s       │
  │                                          └────────┬─────────────┘
  │                                                   │
  │                              fail ┌───────────────┴───────────────┐ success
  │                                   ▼                               ▼
  │                          back to (1) QR+AP              CONNECTED screen
  │                          (keep AP up,                   ┌──────────────────┐
  │                           rewrite e-paper                │ "Connected ✓"    │
  │                           "Connect failed                │ Activation code: │
  │                            — retry")                    │   A7F3K9         │
  │                                                         │ [QR: https://    │
  │                                                         │  everframe.app/  │
  │                                                         │  activate?code=  │
  │                                                         │  A7F3K9 ]        │
  │                                                         │ "Enter this at    │
  │                                                         │  everframe.app/  │
  │                                                         │  devices"        │
  │                                                         └──────────────────┘
  │                                                                   │
  │                                                                   ▼
  │                                                          poll /devices
  │                                                          every 1 h until
  │                                                          claimed (light
  │                                                          sleep, radio off)
  │
  └─ Normal wake (no button) ──► check NVS wifi_ssid/pass ──► STA connect ──►
     if no creds → PROVISIONING(1) above
     if creds but no IP after timeout → fall through to PROVISIONING(1)
     (old creds stay in NVS, form pre-filled)
```

**Invariants**

- E-paper never blanks on failure. Re-render the QR screen with an error banner.
- One endpoint for all frames: device stores `wifi_ssid/pass + device_id` only.
  Bucket/token fields are removed from the portal — routing is `deviceId → userId`
  via Firestore, signed URL for the image.
- Deep sleep is the resting state. AP mode lives 30 min then deep-sleeps (EXT0
  button wake). Normal cycle is `wake → wifi_init → fetch hash → wifi_stop →
  display (~25 s BUSY) → deep sleep`.

---

## 2) Hardware hooks to confirm before coding

| Item | Where | What to verify |
|------|-------|----------------|
| **SW1** (Y78B23110FP, `Button_Switch_SMD:SW_Push_1P1T`) | pcb/everframe_board at 59.69,38.1, wires `(53.34,30.48)-(53.34,38.1)` etc. | Net name at SW1 pins — not labeled in sch dump. Meter SW1 → XIAO header pin. Likely `GPIO0 / BOOT` (standard on XIAO). Confirm pin number, active level (expect **active-low**, pulled up). |
| Wake capable | ESP32-S3 | Whatever pin SW1 lands on must be RTC GPIO for `esp_sleep_enable_ext0/1_wakeup`. GPIO0 qualifies. If SW1 is not RTC, add `gpio_hold` or move wiring in next spin. |
| LED | `GPIO21` active-low | Already used as provisioning/connected indicator. Keep. |
| Display | 800×480, 4 bpp = 192 KB, `EPD_SPI_MAX_TRANSFER` 32 KB, S3 `SPI_LL_DMA_MAX_BIT_LEN = 1<<18` bits | QR must be rendered to framebuffer via `GUI_Paint` / `Fonts` (existing `Fonts/*.c`, `GUI/GUI_Paint.c`). |

If SW1 is confirmed GPIO0 active-low, the spec below uses that. If it is a
different GPIO, change one `#define` — no logic change.

---

## 3) NVS layout (namespace `everframe`)

Current keys: `wifi_ssid`, `wifi_pass`, `bucket_name`, `bucket_token`,
`device_name`, `prov_done`, `last.image.hash` (planned).

**After this redesign**

| Key | Type | Purpose |
|-----|------|---------|
| `wifi_ssid` | str 32 | STA SSID (portal input) |
| `wifi_pass` | str 64 | STA password |
| `device_id` | str 33 | 128-bit hex, generated once per device (lowercase, persistent) |
| `activation_code` | str 7 | 6-char uppercase derived from `device_id` (e.g. `A7F3K9`) |
| `prov_done` | u8 | 1 = has Wi-Fi creds (boot chooses STA); 0/unset = provisioning |
| `last_image_hash` | str 64 | SHA of last displayed bin — skip refresh if unchanged |
| _(remove)_ `bucket_name`, `bucket_token` | — | No longer per-device. Cloud resolves `deviceId → userId → bucket path`. Keep read fallback for 1 boot during migration, then delete. |
| `device_name` | str 63 | Optional, keep if you want per-frame naming from portal (defer). |

`device_id` is generated with `esp_random()` and stored on first boot that
enters provisioning (or on factory flash). `activation_code = base32(device_id)[0:6]`
or first 6 of `device_id` uppercased — deterministic per device, no server round-trip
to generate.

---

## 4) File-level changes (checklist)

All paths under `embedded/everframe-esp32/main/`.

### 4a) New modules

- **`button.h/.c`** — `button_init()`, `button_was_held_on_boot(ms)`,
  `button_poll_held(ms)` (debounced), `button_enable_wakeup()` for deep sleep.
  Handles active-low + internal pull-up, 50 ms debounce, 3000 ms threshold.
  Deep-sleep wake via `esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0)`.

- **`display_provisioning.h/.c`** — owns all e-paper screens in this flow.
  Depends on `ePaper_Config.h`, `GUI_Paint.h`, `Fonts/fonts.h`, and a vendored
  QR generator (see 4c). Screens:
  - `display_show_qr_ap(ssid)` — white background, centered QR (Wi-Fi), SSID
    text, "→ http://192.168.4.1"
  - `display_show_connecting(ssid)` — spinner/text "Connecting to <SSID>…"
  - `display_show_connected(code)` — "Connected ✓", activation code large font,
    claim QR (`https://everframe.app/activate?code=CODE`)
  - `display_show_connect_failed(ssid)` — error banner + QR again
  Uses existing `EPD_Task` pattern but called synchronously: `EPD_Module_Init()`,
  `EPD_7IN3E_Init()` / `EPD_7IN3E_Clear(WHITE)` / `EPD_7IN3E_Display(frame)` with
  `EPD_SPI_WriteData` 32 KB chunks, then `EPD_7IN3E_Sleep()` / `EPD_Module_Exit()`
  or keep awake during AP mode (trade power vs 25 s per refresh).

- **`device_identity.h/.c`** — `device_ensure_identity()` — reads `device_id`
  from NVS or generates + commits. Derives and stores `activation_code`. Logs
  both once at `INFO`.

### 4b) Modified modules

- **`main.c`**
  - At top of `app_main()` (after `nvs_flash_init()`), call
    `button_init()` + `device_ensure_identity()`.
  - Check `gpio_get_level(BUTTON_GPIO) == 0` for 3 s **before** the
    `provisioning_is_complete()` branch. If held, erase `prov_done` is NOT
    needed — instead set a `force_provisioning` flag and branch to
    `wifi_prov_start()` regardless of NVS. Optionally clear `wifi_ssid/pass` on
    10 s long-hold (factory reset).
  - Enable button wake before every `esp_deep_sleep_start()` in the idle paths.
  - Uncomment `EPD_Task` was disabled — provisioning screens now own display.

- **`wifi_prov.h/.c`** — big trim:
  - Remove form fields `bucket_name`, `bucket_token`, `device_name` from
    `SETUP_HTML` (keep `wifi_ssid`, `wifi_pass` only). Shrink `FORM_BODY_MAX_LEN`
    to ~600. New form: 2 inputs + helper text.
  - `save_configuration()` now only writes `wifi_ssid`, `wifi_pass` (+ `prov_done=1`
    last + `nvs_commit`). Never logs password.
  - Change success path: **do not `esp_restart()`**. Instead call into
    `wifi_sta_try_once(ssid, pass, timeout_ms)` (new helper in `wifi_sta`) so
    the same boot can show CONNECTED vs FAILED screens without a reboot.
  - On connect success → `display_show_connected(activation_code)` + keep
    `prov_done=1`, then deep-sleep or stay up for claim polling.
  - On failure → `display_show_connect_failed(ssid)` + loop back to QR+AP
    state (keep HTTP/DNS alive, do not reboot).
  - At startup, call `display_show_qr_ap(softap_ssid)` before `start_webserver()`
    / `start_dns_server()`. AP SSID is `EverFrame-<last4MAC>` as today
    (`make_softap_ssid()`).
  - AP idle timeout: `vTaskDelay` loop already exists — add 30-min counter then
    `esp_deep_sleep_start()` with button wake.

- **`wifi_sta.h/.c`**
  - Extract `wifi_sta_try_once()` (blocking connect with timeout, returns
    `ESP_OK` / `ESP_ERR_TIMEOUT`) for use by `wifi_prov` without entering the
    infinite retry loop.
  - Keep existing `wifi_sta_start()` infinite loop for normal boots. Add early
    fallback: if no IP within 30 s, return `ESP_ERR_TIMEOUT` so `main.c` can
    fall through to provisioning UI instead of spinning forever. Current code
    retries forever — change to bounded.

- **`ePaper_Config.h/.c`**
  - No pin change for this flow. Ensure `EPD_*` init can be called from
    `display_provisioning` without colliding with `wifi_prov` task. SPI bus is
    shared — provision display runs before Wi-Fi or with STA stopped.

- **`CMakeLists.txt`** — add new sources:
  `button.c`, `display_provisioning.c`, `device_identity.c`, `qrcodegen.c`.
  No new `PRIV_REQUIRES` beyond `esp_driver_gpio` already present; QR is header+one C file.

- **`sdkconfig`** — enable `CONFIG_ESP_WIFI_AUTH_WPA2_PSK` already set. Ensure
  `CONFIG_FREERTOS_HZ` sufficient for button polling. No other Kconfig needed
  except `CONFIG_ESP_WIFI_SSID/PASSWORD` defaults are ignored at runtime.

### 4c) QR generation (vendored, no component)

Vendor `qrcodegen.c/.h` from [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator)
(C, public domain, ~1 file + header). Alternatives like `esp_qrcode` pull `qrcode`
component with heap assumptions — nayuki is simpler for e-paper.

- Version: QR v3–v5 for short payloads (`WIFI:T:nopass;S:EverFrame-XXXX;;` ~30 B
  and `https://everframe.app/activate?code=A7F3K9` ~40 B). ECC `M`.
- Rendering: `qrcodegen_encodeText(..., temp, qr, ECC_MEDIUM, ...)` →
  `qrcodegen_getSize(qr)` → paint black squares via `Paint_DrawPoint` /
  `Paint_DrawRectangle` at `scale = (available_px / qr_size)`, centered, white
  quiet zone 4 modules. Frame is 1-bit logical but packed 4 bpp for 7in3e — for QR
  screens just fill with `EPD_7IN3E_WHITE`/`BLACK` via existing palette.

### 4d) Wi-Fi QR payload

For open AP `EverFrame-XXXX`:

```
WIFI:T:nopass;S:EverFrame-XXXX;;        // minimal, most phones accept
```

If any phone fails to auto-join, fallback to plain URL QR (`http://192.168.4.1`)
is acceptable — phone joins manually then scans URL. Primary is Wi-Fi QR.

Claim QR (activation code display):

```
https://everframe.app/activate?code=A7F3K9
```

Frontend route `/activate` should read `code` query, prompt login if needed,
then run the claim transaction (see `docs/cloud-auth-setup.md`).

---

## 5) Button behavior detail

- **Normal boot, no press:** `provisioning_is_complete()` decides STA vs AP as today.
- **Button held 3 s at boot (or anytime before wifi init):** force `wifi_prov_start()`
  + render QR. Debounce 50 ms, require continuous low for 3000 ms.
- **10 s hold (factory):** `nvs_flash_erase()` + regenerate `device_id`, reboot
  into provisioning. Log `factory reset`.
- **Deep sleep:** before every `esp_deep_sleep_start()`, call
  `esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0)` (or `ext1` with mask). On wake,
  `esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0` → treat as button wake,
  go straight to provisioning.

Test matrix:

- [ ] Short tap (<500 ms) does NOT enter provisioning.
- [ ] 3 s hold from deep sleep wakes and shows QR.
- [ ] 3 s hold from running STA forces AP+QR without erasing creds (form pre-filled
  if `wifi_ssid` exists — nice to have).
- [ ] 10 s hold erases and shows new activation code (different `deviceId`).
- [ ] No button → normal STA connect still works (`Wi-FightTheFeeling2` in last log).

---

## 6) UI copy / e-paper notes

All text via `Fonts/font24.c` / `font20.c`. Keep Wi-Fi QR ≥ 180 px.

```
[QR]                  ← Wi-Fi join
EverFrame-XXXX
Go to http://192.168.4.1
Enter your home Wi-Fi

— after connect success —

Connected ✓  (large, centered)
Your code:  A7F3K9   (font24, 2× scale)
[QR]  everframe.app/activate?code=A7F3K9
Enter this code at everframe.app/devices
(while signed in with Google)

— after failure —

Could not connect to "<SSID>"
Check password and try again.     ← small
[QR again]
```

The e-paper holds its last image with zero power — failed states must still
rewrite the screen so the user sees the failure. Use `EPD_7IN3E_Clear(WHITE)`
before each screen.

---

## 7) Implementation order (one afternoon per bullet)

1. `device_identity` + `button` + wake (no display yet) — unit test via logs.
2. Vendor `qrcodegen`, add `display_provisioning` with QR render to framebuffer —
   verify a QR on the 7in3e by flashing a test screen.
3. Trim `wifi_prov` form to SSID/pass only, hook `wifi_sta_try_once`, wire the
   four screens + failure loopback.
4. Wire `main.c` force-provisioning on button, AP 30-min sleep, pre-fill.

---

## 8) Open questions (answer before PR)

- [ ] Confirm SW1 GPIO and active level. Update `BUTTON_GPIO` define.
- [ ] QR quiet-zone / centering on 800×480 — pick `scale` that leaves 1 cm border.
- [ ] AP lifetime: 30 min vs never-sleep while on USB power? `VBAT` detect could
  choose. Simplest: 30 min always, button re-wakes.
- [ ] `device_id` format: raw MAC+random vs pure random? Pure random is simpler
  and avoids MAC privacy. Recommend pure random 128-bit.
