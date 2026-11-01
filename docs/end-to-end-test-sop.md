# EverFrame end-to-end acceptance SOP

**Version:** 2026-10-19
**Environment:** production project `everframe-479214`, `https://everframe.hoodcharlesd.me`
**Hardware:** XIAO ESP32S3, SW1 on GPIO1/D0, 7.3-inch 800×480 six-color e-paper

This is the source-of-truth procedure for accepting the firmware → cloud →
website → frame flow. Run it in order. Later tests deliberately change pairing
and NVS state, so do not skip the recorded checkpoints.

## 1. What this SOP proves

The run passes only when all critical checks below pass:

- Google sign-in protects the dashboard and logout returns to the login page.
- A clean frame provisions Wi-Fi, registers a secret hash, shows a pairing code,
  and can be claimed without exposing its device secret.
- JPEG, PNG, and iPhone HEIC/HEIF imports decode; the owner chooses a 3:5 crop;
  a high-resolution source is proportionally downsampled before cropping.
- Upload or **Send to frame** queues fresh Gemini artwork, converts it with the
  production serpentine Atkinson pipeline, and publishes the manifest last.
- The frame downloads exactly 192,000 bytes, verifies the manifest SHA-256,
  sends those same bytes to the display, commits the hash, and sleeps.
- A short press creates a five-minute update window without requiring the frame
  to remain online continuously.
- Device removal is owner-only, preserves photos, returns the frame to pending,
  and permits re-pairing with the same code.
- A 10-second factory reset creates a new device identity while the old cloud
  record remains removable from the dashboard.
- Expected recovery behavior preserves the currently displayed e-paper image.

## 2. Current contracts and timing

| Item | Expected value |
|---|---|
| Import formats | `.jpg`, `.jpeg`, `.png`, `.heic`, `.heif` |
| Import limit | 75 MB per file |
| Browser working preview | Whole image, at most 4096 px on longest edge |
| Uploaded crop | 960×1600 JPEG, portrait 3:5 |
| Gemini model | `gemini-3.1-flash-image`, Vertex `global` endpoint |
| Model composition | 2:3 portrait with a 3:5 safe area, then deterministic fit |
| Panel payload | 800×480 native order, six-color 4-bpp, exactly 192,000 bytes |
| Dither | Portrait-first serpentine Atkinson, then clockwise native rotation |
| Signed download URL | 15 minutes |
| Normal cloud/device poll cadence while awake | 15 seconds |
| Dashboard status refresh | 10 seconds |
| Short-press update window | 5 minutes |
| Initial unclaimed/claim window | 30 minutes |
| Successful/unchanged sleep | Dashboard preset: manual, 12 hours, daily, or weekly |
| Retry sleep after terminal cloud failure | 1 hour |
| Scheduled generation | Dispatcher at 00:00 and 12:00 UTC; filters by device preset |
| Device photo storage | No photo files; one transient 192 KB PSRAM buffer + last SHA in NVS |

Every frame owned by one account currently receives the same owner manifest.
Per-device playlists, retention cleanup, and generated-art deletion are not yet
implemented.

## 3. Equipment and accounts

Have these ready before changing device state:

- EverFrame connected with a known-good USB **data** cable.
- The display visible and a phone camera available for evidence.
- A phone that can join the temporary `EverFrame-XXXX` Wi-Fi network.
- Home 2.4 GHz Wi-Fi name and password.
- A signed-in Google account authorized for EverFrame.
- Terminal at `/Users/charleslab/git/everframe`.
- One portrait JPEG, one PNG, and one real iPhone HEIC/HEIF.
- Preferably one 30–50 MP portrait source for the downsample/crop check.
- A deliberately unsupported small file such as GIF for negative validation.

Do not paste device secrets, Wi-Fi passwords, Firebase tokens, or signed URLs
into the test record. Device IDs and six-character activation codes are public
identifiers and may be recorded.

### Operator rules and stop conditions

- Keep the serial monitor running through physical button tests; screen-only
  observations cannot distinguish a cloud, download, SPI, or sleep failure.
- Photograph the screen before every destructive identity/display transition.
- D1 unpairs a real device; E1 erases its NVS; J3 deletes a cloud photo. Perform
  only the specifically named target and verify its ID/name first.
- F uploads and every explicit send may incur Storage and one Gemini generation.
  I6 intentionally incurs two. Do not repeatedly click a queued action.
- Stop immediately on token/secret disclosure, ownership mismatch, wrong-device
  removal, manifest SHA mismatch, wrong payload length, or a partial/torn panel
  write. Preserve logs and do not continue with another refresh over evidence.
- A cloud/UI error must not justify editing IAM, rules, manifests, or production
  Firestore by hand during this acceptance run.

## 4. Evidence worksheet

Create one row per test while we run. `Blocker` means stop before a destructive
next phase; `Follow-up` means the remaining flow can continue.

| ID | Result | Actual observation / timestamp | Evidence | Severity if failed |
|---|---|---|---|---|
| A1–A6 |  |  | terminal | Blocker |
| B1–B5 |  |  | serial/photo | Blocker |
| C1–C5 |  |  | browser | Blocker |
| D1–D7 |  |  | browser/serial | Blocker |
| E1–E8 |  |  | phone/serial/photo | Blocker |
| F1–F9 |  |  | browser | Blocker |
| G1–G10 |  |  | cloud/serial/photo | Blocker |
| H1–H9 |  |  | serial/browser | Blocker |
| I1–I6 |  |  | browser/cloud | Follow-up |
| J1–J8 |  |  | browser/serial | Blocker |
| K1–K7 |  |  | browser/serial | Blocker |
| L1–L8 |  |  | mixed | Follow-up |

## 5. Phase A — non-destructive readiness

### A1. Confirm the board is visible

Connect or briefly replug the ESP, then run:

```bash
ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null
```

**Pass:** exactly one expected serial port appears. If none appears, change the
cable/USB port and stop; do not guess a port.

### A2. Load ESP-IDF and identify the port

```bash
export IDF_PATH=/Users/charleslab/.espressif/v6.0.2/esp-idf
export IDF_PYTHON_ENV_PATH=/Users/charleslab/.espressif/python_env/idf6.0_py3.10_env
source "$IDF_PATH/export.sh"
export EVERFRAME_PORT=/dev/cu.usbmodemXXXX
```

Replace the final value with the port from A1.

### A3. Confirm this is a normal production-flow build

```bash
rg -n 'CONFIG_EVERFRAME_(DISPLAY_SELF_TEST|DISABLE_DEEP_SLEEP|EMBEDDED_HALF_FRAME_TEST)' \
  embedded/everframe-esp32/sdkconfig
```

**Pass:** all three options say `is not set`. Any enabled diagnostic bypasses
part of this SOP and must be disabled before continuing.

### A4. Run automated regression suites

```bash
python3 -m venv .venv-cloud
. .venv-cloud/bin/activate
pip install -r cloud/cloud_function/requirements.txt
PYTHONPATH=cloud/cloud_function python -m unittest discover \
  -s cloud/cloud_function/tests -v

cd website
npm ci
npm run typecheck
npm run build:vinext
cd ..
```

**Pass:** 31 cloud tests pass; TypeScript and the Vinext build pass. Vinext
sourcemap and large HEIC chunk warnings are known non-blocking warnings.

### A5. Confirm production cloud state

```bash
gcloud functions list --v2 --regions=us-central1 \
  --project=everframe-479214 --format='table(name,state)'

gcloud scheduler jobs describe everframe-twice-daily \
  --location=us-central1 --project=everframe-479214 \
  --format='yaml(schedule,timeZone,state,scheduleTime)'

gcloud run services describe everframeprocessor \
  --region=us-central1 --project=everframe-479214 \
  --format='yaml(spec.template.spec.containers[0].env,status.latestReadyRevisionName)'
```

**Pass:** the processor plus `registerDevice`, `devicePoll`, `claimDevice`,
`removeDevice`, `refreshDevice`, and `deviceStatus` are `ACTIVE`; schedule is
`0 0,12 * * *`; generation is `true`; model is `gemini-3.1-flash-image`; image
location is `global`.

### A6. Confirm the live site and auth boundary

```bash
curl -sS -o /dev/null -w '%{http_code}\n' \
  https://everframe.hoodcharlesd.me/
curl -sS -o /dev/null -w '%{http_code}\n' \
  https://us-central1-everframe-479214.cloudfunctions.net/deviceStatus
```

**Pass:** site is `200`; unauthenticated status is `401`.

## 6. Phase B — flash and establish a serial baseline

### B1. Build the exact working tree

```bash
cd embedded/everframe-esp32
idf.py fullclean build
```

**Pass:** build finishes without errors and the app fits its partition.

### B2. Flash and monitor

Wake/replug immediately before flashing if the board has entered deep sleep:

```bash
idf.py -p "$EVERFRAME_PORT" flash monitor
```

Exit the monitor with `Ctrl+]` when instructed later.

### B3. Record the baseline identity without recording secrets

Record only:

- `deviceId` from `Device identity ready`.
- Six-character `activation` code.
- PSRAM byte count.
- Whether boot chose station or provisioning mode.

**Never record:** the device secret or its hash.

### B4. Confirm normal runtime endpoints

The current build uses the stable direct Cloud Run URLs for registration and
polling. There must be no repeated TLS handshake error.

**Pass:** logs reach `Device registration is ready`; if Wi-Fi is saved, they
also reach `Station connection is ready` and a device poll state.

### B5. Preserve the current display baseline

Photograph the current screen before any refresh. Note orientation, broad
vertical/horizontal bands, clipped subjects, and obvious color substitution.
The e-paper image must remain visible even after the ESP sleeps or is unplugged.

## 7. Phase C — website authentication and navigation

### C1. Signed-out landing page

Open `https://everframe.hoodcharlesd.me` in a private browser window.

**Pass:** only the simple Google sign-in experience is offered. Directly opening
`/dashboard` does not reveal owner data.

### C2. Google sign-in

Sign in and reach the dashboard.

**Pass:** the header exposes **Add Photos** and **My Device**; no activation or
OAuth secret is shown.

### C3. Header behavior

Click **Everframe**, **Add Photos**, and **My Device** in turn.

**Pass:** the brand does not log the user out; Add Photos opens import;
My Device opens the gallery/device status view.

### C4. Logout behavior

Click logout, then use the browser Back button and try `/dashboard` directly.

**Pass:** the login page returns and cached owner content is not exposed.

### C5. Sign back in

Return to **My Device** and record every device card, shortened ID, awake/sleep
badge, and status copy. This is the pre-removal inventory.

## 8. Phase D — remove and re-pair the existing identity

Run this before factory reset so we can prove reuse of the same identity.

### D1. Remove from the dashboard

On the card matching the B3 identity, click **Remove**. Read the confirmation,
confirm it says photos remain, then approve.

**Pass:** a success toast appears and the card disappears within one status
refresh. Gallery photos remain.

### D2. Wake the removed frame

Short-press SW1 for less than three seconds and watch serial output.

**Pass:** it logs a button wake, authenticates with its still-valid secret,
receives the pending state, and displays the same activation code recorded in
B3. Once pending is observed, the short-button window must upgrade to a
30-minute activation window. Transient poll failures must retry within that
window instead of putting the frame to sleep.

### D3. Reject an incorrect code

Open `/activate`, enter a different well-formed six-character code, and submit.

**Pass:** the UI reports that no pending device was found; no device appears.

### D4. Reclaim the real code

Enter the displayed code and submit while the frame is still awake.

**Pass:** the site confirms the link, the device card returns, and the frame's
next poll changes from pending to claimed without another button press.

### D5. Reuse protection

Submit the same code again after it is claimed.

**Pass:** it cannot be claimed a second time while already owned.

### D6. First-refresh behavior

Showing the activation screen invalidates the saved photo hash. Claim queues a
refresh. If the gallery contains an enabled photo, expect the status sequence
`linked/queued → processing → ready → displayed`, followed by an automatic
192,000-byte download that replaces the activation code without another button
press. If there is no photo, expect `waiting_for_photo` instead of an error.

### D7. Owner isolation

The automated production test already proves another Firebase owner receives
404 when attempting removal and can claim only after the real owner removes the
device. Do not create a second personal Google account just to repeat it.

## 9. Phase E — physical provisioning and factory reset

This phase erases Wi-Fi, device identity, activation code, and last-image SHA.
The e-paper picture itself remains because the panel is bistable.

### E1. Ten-second factory reset

From sleep or while reconnecting, press and continuously hold SW1 through boot
for about 10 seconds. Release promptly when serial prints
`Factory reset requested; erasing NVS and restarting`.

**Pass:** NVS is erased, the board restarts, and the new `deviceId` and
activation code differ from B3. Avoid holding through a second full 10-second
cycle.

### E2. Old cloud record behavior

Refresh **My Device**.

**Pass:** the old identity remains as an offline cloud card; physical reset
cannot authenticate a cloud deletion. Remove this old card from the dashboard.

### E3. Provisioning screen and AP

With no saved provisioning flag, the frame starts its open
`EverFrame-XXXX` network and displays the Wi-Fi QR/instructions.

**Pass:** serial shows `Provisioning SoftAP ready`, the displayed SSID matches
the phone's network list, and the portal address is `http://192.168.4.1/`.

### E4. Captive portal

Join the AP using the phone. Allow the captive portal to open, or browse to
`http://192.168.4.1/`.

**Pass:** the setup form loads; an unknown path redirects to the form. Submitting
an empty SSID or password is rejected. The **Nearby networks** selector scans
automatically, de-duplicates SSIDs, sorts strongest-first, shows signal strength
and security state, and fills **Network name** when selected. **Rescan** refreshes
the list; manual entry remains available for hidden networks. Open networks are
shown but disabled because Everframe requires WPA2 or newer.

### E5. Bad-Wi-Fi recovery

Submit the correct SSID with a deliberately wrong password once.

**Pass:** the portal acknowledges the save and reboots; the station attempt is
bounded to 30 seconds; failure returns to the setup AP instead of hanging.

**Known limitation:** the portal currently saves before validation and reboots;
it does not test station connectivity inline while keeping the AP alive.

### E6. Correct Wi-Fi

Reconnect to the setup AP, enter correct credentials, and submit.

**Pass:** the device restarts, gets a DHCP address, logs
`Station connection is ready`, and never prints the Wi-Fi password.

### E7. Cloud registration and activation display

**Pass:** registration succeeds, the frame shows `Connected` plus the new
activation code and QR, and serial polls as pending without exposing a secret.

### E8. Claim the factory-new identity

Use the QR or `/activate`, claim the new code, and confirm its device card
appears. Leave it linked for the remaining phases.

## 10. Phase F — import, downsample, crop, and gallery

### F1. JPEG import

Select one portrait JPEG.

**Pass:** the 3:5 portrait crop editor appears before upload; drag and zoom work.

### F2. Large-photo downsample

Select the 30–50 MP photo. At zoom 1, inspect the initial composition.

**Pass:** the whole source composition is available in the crop editor; the UI
has not magnified a tiny 480×800 portion. Choose a crop containing recognizable
features near its edges.

### F3. PNG import

Import PNG and choose a crop.

**Pass:** transparent areas, if any, are flattened against white and upload as
JPEG rather than producing black/undefined panel regions.

### F4. HEIC/HEIF import

Import a real iPhone HEIC/HEIF.

**Pass:** it decodes in-browser, orientation is correct, and cropping works.
Allow extra time for the lazily loaded HEIC decoder.

### F5. Multi-photo crop

Select at least two valid photos together.

**Pass:** each photo has independent crop/zoom state; **Next** remains disabled
until the current crop exists; upload remains disabled until all crops exist.

### F6. Cancel path

Start another import, alter the crop, then cancel.

**Pass:** nothing uploads and existing gallery records remain unchanged.

### F7. Unsupported input

Select GIF or a text file renamed with a non-supported extension.

**Pass:** the UI reports JPEG/PNG/HEIC-only validation and uploads nothing.

### F8. Oversize validation

If a disposable image over 75 MB is available, select it.

**Pass:** it is rejected before decode/upload. Skip rather than manufacturing a
large file solely for this test.

### F9. Gallery presentation

After upload completes, open **My Device**.

**Pass:** cards are portrait 3:5 and the chosen crop is visible. The source used
by the current published manifest receives **On display**; use device status,
not this badge alone, to prove that the physical frame acknowledged it.

## 11. Phase G — cloud generation, byte integrity, and display quality

### G1. Choose one unmistakable source

Select exactly one gallery card and click **Send to frame**.

**Pass:** multiple selection disables sending with `Select one to send`; one
selection queues successfully.

### G2. Observe dashboard state

Expected sequence within normal processing time:

1. `Photo queued`.
2. `Preparing your photo for the display`.
3. `Photo is ready`.
4. After device poll: `Your frame is up to date` / displayed.

If the frame is sleeping, ready status should explicitly request a button press.

### G3. Observe processor logs

```bash
gcloud functions logs read everframeProcessor --gen2 \
  --region=us-central1 --limit=60
```

**Pass:** no Gemini, source-download, frame-upload, or manifest error. A failed
generation must not replace the previous manifest.

### G4. Capture the manifest

Find the Firebase UID in Firebase Authentication, then:

```bash
export EVERFRAME_UID='replace-with-firebase-uid'
gcloud storage cat \
  "gs://everframe-photo-bucket/users/$EVERFRAME_UID/manifest.json" \
  | tee /tmp/everframe-manifest.json
```

Record `sourcePath`, `style`, `hash`, `image`, `bytes`, and `createdAt`.

**Pass:** source matches the selected card; style is non-empty; bytes are
192000; format is `everframe-epd7in3e-4bpp`; event and timestamp are new.

### G5. Independently verify the artifact

```bash
export EVERFRAME_IMAGE_OBJECT="$(jq -r .image /tmp/everframe-manifest.json)"
gcloud storage cp \
  "gs://everframe-photo-bucket/$EVERFRAME_IMAGE_OBJECT" \
  /tmp/everframe-frame.bin
wc -c /tmp/everframe-frame.bin
shasum -a 256 /tmp/everframe-frame.bin
jq -r .hash /tmp/everframe-manifest.json
```

**Pass:** byte count is exactly 192000 and the local SHA equals manifest hash.

### G6. Wake delivery

If asleep, short-press SW1 once. Do not hold three seconds.

**Pass:** serial includes:

```text
Short button press woke the frame; checking cloud now
Button update window open for 5 minutes
Downloading a new 192000-byte frame
Verified HTTPS frame: 192000 bytes, SHA-256 <manifest hash>
New frame displayed and committed
Sleeping for <online-corrected seconds until the next schedule target>
```

### G7. Byte-boundary check

Compare the full 64-character serial SHA with G5. This proves the signed object
arrived intact. The firmware passes that verified 192 KB buffer directly to the
display path; there is no second image decode on-device.

### G8. Visual display acceptance

Photograph the panel straight-on in even light and compare with the generated
preview. Pass only if:

- portrait composition is upright in the intended frame orientation;
- subject placement matches the chosen crop/safe area;
- no broad periodic banding or torn rows appear;
- no half-screen boundary or numbered diagnostic overlay appears;
- edges are not a tiny over-zoomed crop of the original;
- black, white, red, yellow, blue, and green are plausible for Spectra 6;
- expected e-paper grain/dither is present without directional stripe artifacts.

### G9. Display acknowledgment

Within 10–20 seconds after the successful device poll is recorded, refresh My
Device.

**Pass:** state is displayed/up-to-date and its `displayedHash` matches the
current manifest hash. The gallery source badge identifies the published
manifest source and may appear before physical acknowledgment.

### G10. E-paper persistence

After serial reports deep sleep, disconnect USB power briefly only if the board
has no other power source.

**Pass:** the image remains on the e-paper panel without power.

## 12. Phase H — five-minute wake-window behavior

### H1. Up-to-date short press

With no newly queued image, short-press SW1.

**Pass:** it logs `Displayed image is current; waiting for a dashboard send`
instead of immediately sleeping.

### H2. Send while already awake

During that window, select a different single photo and click **Send to frame**.

**Pass:** the frame discovers it on a later 15-second poll, displays it, commits
the new hash, and sleeps immediately rather than waiting out five minutes.

### H3. Queue before wake

Allow the frame to sleep, send another photo first, wait for dashboard status
`ready`, then short-press once.

**Pass:** the next poll downloads the ready artifact and refreshes once.

### H4. No-update expiry

Short-press with no pending update and do not send anything for five minutes.

**Pass:** serial logs `Button update window expired without a new frame` and
then sleeps until the next absolute schedule target.

### H5. Debounce

Use one quick normal press.

**Pass:** it produces one wake cycle, not multiple rapid wake/reset cycles.

### H6. Three-second distinction

Hold SW1 for slightly more than three but less than ten seconds during wake.

**Pass:** it deliberately enters provisioning without erasing the device ID.
Power-cycle/restart without holding to resume saved Wi-Fi.

### H7. Dashboard awake indicator

While H1–H4 is active, refresh My Device.

**Pass:** it changes to **Awake** after a button poll and eventually returns to
**Sleeping** after `awakeUntil` expires.

### H8. Configurable timer wake

Test the dashboard options in this order: **Manual only**, **12 hours**,
**Daily**, and **Weekly**. Each change must show pending until the frame polls.
The authenticated response supplies `serverTime`, absolute `nextWakeAt`, and a
version. Firmware synchronizes online time, persists the preset/version, reports
the applied version, and sleeps for the corrected remaining duration. Manual
mode arms only button wake. Do not wait a full interval during the supervised
session; unit/build checks cover the schedule calculations and a later
overnight/weekly observation covers oscillator accuracy.

**Verified 2026-11-01:** all four presets were applied by a physical ESP in the
listed order. Desired and applied schedule versions matched in Firestore. The
unchanged image did not refresh, and Weekly slept for 593,215 seconds toward
the next Sunday 12:15 UTC target after the five-minute button window expired.

### H9. Battery caveat

This SOP verifies state transitions, not battery capacity. Battery testing needs
current measurement across sleep, Wi-Fi association, Gemini wait/poll, download,
and e-paper refresh over multiple days.

## 13. Phase I — AI style management

### I1. Open Art Styles

**Pass:** the panel is private to the signed-in user and initially reports
`100/200`: 100 editable EverFrame starter styles in a 200-style library.

### I2. Starter library

Review the initial style library.

**Pass:** all 100 starter styles are visible as ordinary editable and removable
library entries. They are not backed by a hidden, undeletable duplicate pool.

### I3. Add and validate

Add one unmistakable custom style. Also try blank fields and a prompt over 160
characters.

**Pass:** valid style saves; invalid values remain unsaved with clear errors.

### I4. Edit

Change the custom label/prompt and save.

**Pass:** the updated value persists after closing and reopening the dialog.

### I5. Delete

Delete only the custom test style. Click **Clear all**, then cancel the
confirmation once. Reopen it and confirm the clear, then click **Restore 100
EverFrame styles**.

**Pass:** individual deletion works; clear requires an **Are you sure?**
confirmation; cancel preserves the library; confirmation removes every style
without a hidden fallback; restore repopulates the 100 EverFrame styles.

### I6. Fresh-generation behavior

Send the same source twice and compare manifest `eventId`, `createdAt`, `style`,
and hash.

**Pass:** each request performs fresh generation. Identical hashes are possible
but not expected; no cache contract exists yet. This step incurs two Gemini
generations, so run it once only.

## 14. Phase J — gallery controls and owner data

### J1. Single vs multiple selection

**Pass:** exactly one selected card enables **Send to frame**; multiple cards do
not accidentally send an ambiguous source.

### J1a. Replace an in-flight request

Send one photo, then send a different photo before the first request reaches
`displayed`. Refreshing the browser between the sends is allowed.

**Pass:** the second send reports **Pending photo replaced**. The older job may
finish paid generation, but its request ID cannot publish a deliverable
manifest, overwrite the newer status, or reach the frame. The final manifest's
`sourcePath` and `requestId` belong to the second selection.

### J2. Rotation control

Toggle a non-current card's **In rotation** switch off and reload.

**Pass:** it persists and scheduled selection excludes it. Toggle it back on
after the test.

### J3. Delete a non-current disposable photo

Delete only a test photo that is not marked **On display**.

**Pass:** both its Firestore gallery record and source Storage object disappear;
other gallery items and the current manifest remain.

### J4. Current-source deletion limitation

Do not use the sole/current photo for deletion acceptance. Generated daily
artifacts and the current manifest do not yet cascade-delete when a source is
removed. This is an acknowledged deferred lifecycle feature.

### J5. Multiple devices

If a second physical frame is available, pair it using **Add another frame**.

**Pass:** both device cards appear and both pull the same owner manifest. If no
second frame exists, rely on the passed disposable production lifecycle test.

### J6. Refresh after removal

Removing one of multiple devices must not delete photos, unpair the others, or
change the owner manifest.

### J7. Browser reload and session restoration

Reload My Device during queued, processing, and ready states when practical.

**Pass:** status is reconstructed from cloud state rather than lost client state.

### J8. Mobile layout

Repeat basic Add Photos, crop, My Device, and Remove confirmation checks at a
phone-width viewport.

**Pass:** 3:5 cards remain usable and Remove does not overflow its card.

## 15. Phase K — recovery and important failures

Use controlled failures only. Do not corrupt the production manifest or alter
IAM to manufacture a failure.

### K1. Wi-Fi unavailable at boot

Temporarily disable the test access point or use the bad credential from E5.

**Pass:** the initial attempt is bounded to 30 seconds and setup returns. Restore
connectivity immediately afterward.

### K2. Wi-Fi loss after first connection

After the device has received an IP, briefly disable the AP, then restore it.

**Pass:** firmware retries without erasing identity or the displayed image and
eventually reconnects.

### K3. Cloud poll failures

Do not take production down. Automated coverage verifies three consecutive poll
failures preserve the current image and lead to a one-hour retry sleep. Record
this as automated-only unless a local fault-injection build is prepared.

### K4. Bad signed download or SHA

Automated tests cover invalid manifest, missing/wrong-size artifact, and hash
mismatch. Do not replace the production manifest to test these. Required result:
no display commit and the prior e-paper image remains.

### K5. Gemini failure

Automated and previously observed live failure behavior proves that generation
errors set dashboard state to error and never publish a partial/new manifest.
Do not deploy an invalid model merely to repeat it.

### K6. Unauthorized device secret

Automated production and unit tests cover wrong-secret 401 behavior. Firmware
responds to unauthorized poll by attempting bounded re-registration twice;
persistent identity conflict requests a 10-second factory reset.

### K7. Upload/manifest atomicity

Unit tests verify frame upload precedes manifest publication, manifest failure
leaves the previous manifest active, and preview failure does not block a valid
panel artifact.

### K8. Page/API errors

Confirm unsupported upload, invalid pairing code, signed-out dashboard, and
wrong-owner removal all show bounded errors without losing existing photos or
the current display.

## 16. Phase L — final state and cleanup

### L1. Leave one intended physical identity paired

Remove obsolete/offline cards. Keep the factory-new identity from E8 linked.

### L2. Restore gallery test toggles

Turn intended photos back **In rotation** and delete only disposable non-current
test uploads.

### L3. Leave a known-good image displayed

Send the preferred photo, short-press if needed, and wait for
`New frame displayed and committed`.

### L4. Confirm sleep configuration

Serial must end with the selected online-corrected sleep duration (or the
manual-only button-wake message); no development keep-awake or embedded
diagnostic should be active.

### L5. Final cloud check

```bash
gcloud functions logs read everframeProcessor --gen2 \
  --region=us-central1 --limit=30
gcloud logging read \
  'resource.type="cloud_run_revision" AND severity>=ERROR' \
  --project=everframe-479214 --limit=30 \
  --format='value(timestamp,resource.labels.service_name,textPayload)'
```

Separate deliberate negative-test errors from unexpected production errors.

### L6. Final dashboard check

**Pass:** intended device appears once, sleeping/awake status is plausible,
current source is marked, no temporary device remains, and logout works.

### L7. Preserve evidence

Keep:

- test worksheet;
- serial excerpts containing public ID/code and SHA, with no secrets;
- manifest JSON and independent SHA result;
- generated preview and straight-on panel photo;
- exact firmware git status/commit and Cloud Run/Cloudflare revision IDs.

### L8. Acceptance decision

Release-candidate pass requires all Blocker rows passing. Follow-up failures may
be accepted only when recorded with reproduction steps and do not threaten
ownership, manifest atomicity, SHA integrity, or display correctness.

## 17. Automated coverage already completed

The local suite currently contains 40 tests covering:

- full register → claim → wrong-owner rejection → remove → unclaimed poll →
  reclaim lifecycle;
- owner-safe status with no secret/hash-code leakage;
- Pub/Sub failure vs advisory status-write failure;
- button presence and display acknowledgment;
- event parsing, event-id idempotency, source selection, and the combined
  100-included plus 100-personal style pool;
- Gemini failure and no-source state;
- frame, preview, and manifest failure ordering;
- exact 192,000-byte packing, controller nibble codes, portrait rotation,
  deterministic serpentine Atkinson output, preview decode, and bad inputs.

A disposable live production test also completed registration, owner A claim,
status listing, wrong-owner removal rejection, owner A removal, unclaimed
device poll, owner B reclaim, and full cleanup without touching the real frame.

## 18. Known deferred items — do not misclassify as regressions

- Generated daily artifacts currently accumulate; there is no retention job.
- Deleting a source photo does not cascade-delete generated artifacts.
- All devices belonging to one owner share one manifest; no per-device playlist.
- The captive portal saves/reboots before verifying Wi-Fi inline.
- HEIC support adds a large lazy-loaded browser chunk, but valid imports work.
- The web UI has no application-level cloud gallery count/storage quota yet.
- Battery-runtime measurement is separate from functional sleep/wake acceptance.
- The gallery **On display** badge currently means “source of the published
  manifest”; physical acknowledgment is proven separately by device status/hash.
