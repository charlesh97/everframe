# EverFrame — Docs

> You are here. Pick your starting point by what you want to do.

| I want to… | Start here |
|------------|-----------|
| **Set up cloud + website** (run the thing) | [`cloud-auth-setup.md`](cloud-auth-setup.md) — console clicks, Google sign-in, deploy functions |
| **See the live GCP deployment record** | [`gcp-deployment-2026-10-17.md`](gcp-deployment-2026-10-17.md) — resources, IAM, commands, tests, rollback IDs |
| **Build or change firmware** (button, QR, display, auth) | [`esp32-provisioning-redesign.md`](esp32-provisioning-redesign.md) — state machine, NVS, file checklist |
| **Release firmware or check known issues** | [`firmware-errata.md`](firmware-errata.md) — version policy, supported hardware, errata, and limitations |
| **Acceptance-test the complete product** | [`end-to-end-test-sop.md`](end-to-end-test-sop.md) — physical frame, website, cloud, recovery, removal, and evidence checklist |
| **See how it all fits together** | [Website activation flowchart](https://excalidraw.com/#json=arr4bjbIGcGfSb-QoUbpL,07p7lUawkm2_jj5j_VflbQ) + [Secure device auth flowchart](https://excalidraw.com/#json=Izs2P1VGbzmWCG6pmb7t5,dg3ddAt_Qn7-ZQipjy-0iw) — open in browser, also saved as `.excalidraw` in this folder |
| **Deploy without reading both specs** | [Runbook](#runbook) below — copy-paste sequence |

---

## Runbook — start to finish

Do these in order. Each step says what to run and how to know it worked.

### 0. Prereqs

```bash
# from repo root ~/git/everframe
npm --version && node --version   # Node 22+ (required by Vinext)
firebase --version 2>&1 | head -1 # Firebase CLI; or use gcloud
gcloud --version 2>&1 | head -1   # optional, for --allow-unauthenticated deploys
```

### 1. Firebase console — 10 minutes, one time

1. Firebase Console → `everframe-479214` → **Authentication → Sign-in method** → enable **Google**. Add `localhost` + `everframe-479214.web.app` to authorized domains.
2. **Firestore** + **Storage** (`everframe-photo-bucket`, `us-central1`) already exist — just confirm they're there.
3. **IAM** → Cloud Functions service account → `storage.objectAdmin` + `firestore.user` + `firebaseauth.viewer` (+ existing `aiplatform.user` / topic-scoped `pubsub.publisher`).

*Details:* [cloud-auth-setup §1](cloud-auth-setup.md#1-one-time-console-setup-10-min-do-once)

### 2. Website local — website is the owner portal

```bash
# 1) Firebase web config — get from Project Settings → Your apps → SDK setup
cat > website/.env.local <<'ENV'
NEXT_PUBLIC_FIREBASE_API_KEY=AIza…
NEXT_PUBLIC_FIREBASE_AUTH_DOMAIN=everframe-479214.firebaseapp.com
NEXT_PUBLIC_FIREBASE_PROJECT_ID=everframe-479214
NEXT_PUBLIC_FIREBASE_STORAGE_BUCKET=everframe-photo-bucket
NEXT_PUBLIC_FIREBASE_MESSAGING_SENDER_ID=…
NEXT_PUBLIC_FIREBASE_APP_ID=1:…:web:…
ENV
# never commit .env.local — it is gitignored

# 2) install + typecheck + build
cd website && npm ci && npm run build
# expect: 2 warnings (missing config in CI + pre-existing _query) → build passes

# 3) run locally
npm run dev   # http://localhost:3000
# /           → Google button only
# /dashboard  → redirects to / if not signed in (AuthGate)
# /activate?code=A7F3K9 → calls authenticated claimDevice after sign-in
```

Production is a Vinext Cloudflare Worker:

```bash
npm run typecheck
npm run build:vinext
npx cf auth login       # first deployment or after token expiry
npm run deploy:vinext
```

The checked-in Worker configuration attaches
`https://everframe.hoodcharlesd.me`. Public Firebase production values are read
from the ignored `.env.production.local` during the build.

*Details:* [cloud-auth-setup §2](cloud-auth-setup.md#2-website--already-landed-in-4b611e8-what-to-verify) · website changes landed in `4b611e8`

### 3. Rules — deploy

```bash
firebase deploy --config website/firebase.json --only firestore:rules,storage
# The website is deployed separately to Cloudflare at everframe.hoodcharlesd.me.
```

Verify in console **Rules Playground**:
- unauthed `get /users/{uid}` → denied, authed own → allowed
- authed `list /devices` → denied (activation lookup is server-side)
- authed `/images` query constrained to `userId==auth.uid` → own records only

*Details:* [cloud-auth-setup §3](cloud-auth-setup.md#3-rules-deployed-in-4b611e8--verify-in-console)

### 4. Cloud Functions — processing + authenticated device delivery

The cloud source now exposes six HTTP functions and one Pub/Sub processor:

- `POST /registerDevice` — `{"deviceId":32hex, "tokenHash":64hex SHA256(secret), "activationCode":"A7F3K9"}` → `pending`
- `POST /claimDevice` — Firebase ID token + activation code → atomic `pending` → `claimed`
- `POST /removeDevice` — Firebase ID token + owned device ID → atomic `claimed` → `pending`; preserves the device credential for re-pairing
- `POST /refreshDevice` — Firebase ID token + owner image path → queue an immediate targeted conversion; a newer request ID supersedes any queued, processing, or ready request
- `GET /deviceStatus` — Firebase ID token → owner-safe list of linked devices and delivery state
- `GET  /devicePoll` — headers `Authorization: Bearer <64hex secret>` + `X-Device-Id` → `{claimed:false}` or `{claimed:true, hash, url}` (15-min signed URL)
- `everframeProcessor` — scheduled Pub/Sub job; selects each owner's enabled art, optionally transforms it with Gemini, emits the exact 192,000-byte panel frame, then atomically publishes the owner manifest

```bash
./cloud/deploy.sh

# pure local verification (does not call GCP or Gemini)
python3 -m venv .venv-cloud && . .venv-cloud/bin/activate
pip install -r cloud/cloud_function/requirements.txt
PYTHONPATH=cloud/cloud_function python -m unittest discover \
  -s cloud/cloud_function/tests -v
```

`--allow-unauthenticated` on the HTTP deployments is intentional: `registerDevice`
bootstraps device auth, `devicePoll` verifies the device secret, and the owner
endpoints verify a Firebase ID token. See [`cloud/README.md`](../cloud/README.md) for IAM,
the artifact contract, targeted processor messages, and smoke testing.

Test without hardware (see copy-paste curl in docs):

```bash
SECRET=$(openssl rand -hex 32); HASH=$(echo -n "$SECRET" | xxd -r -p | shasum -a 256 | awk '{print $1}')
DEVICE=a3f9e2c1b4d60718293a4b5c6d7e8f90
curl -X POST https://us-central1-everframe-479214.cloudfunctions.net/registerDevice \
  -H "Content-Type: application/json" \
  -d "{\"deviceId\":\"$DEVICE\",\"tokenHash\":\"$HASH\",\"activationCode\":\"A7F3K9\"}"  # → {"ok":true}
curl -H "Authorization: Bearer $SECRET" -H "X-Device-Id: $DEVICE" \
  https://us-central1-everframe-479214.cloudfunctions.net/devicePoll  # → {"claimed":false}
# wrong secret → {"error":"token mismatch"} 401
# then claim at /activate?code=A7F3K9 (signed in), poll again → {"claimed":true, "hash":…, "url":…}
```

*Details:* [cloud pipeline](../cloud/README.md) · [esp32 spec §1b security model](esp32-provisioning-redesign.md#1b-security-model--why-this-and-not-bare-deviceid)

### 5. Firmware — when you pick up the board

`embedded/everframe-esp32` targets IDF 6.0.2 on the XIAO ESP32S3 + 800×480
6-color e-paper. The table below is the integration ledger; “implemented” means
the code builds, while “board verified” means it has also run on the attached
frame.

| Block | Status | What exists / what is missing |
|-------|--------|-------------------------------|
| Device identity | **Board verified** | Persistent 128-bit `deviceId`, 256-bit secret, activation code, and `SHA256(secret)` are implemented. Secret/hash are not logged. |
| Button + PSRAM | **Partially board verified** | SW1 is **GPIO1/D0, active-high, internal pull-down**. EXT0 short-press wake is board verified: it reconnects, polls immediately, skips an unchanged frame, and returns to deep sleep. A continuous 3 s hold enters provisioning and a 10 s hold factory-resets; those two hold durations still need physical verification. All 8 MB PSRAM passed the boot memory test. |
| Wi-Fi station | **Board verified** | Existing saved credentials connected and received DHCP. Initial connection is bounded to 30 s and falls back to provisioning; after the first IP, normal link-loss reconnects continue indefinitely. The timeout path still needs a physical bad-credential test. |
| Captive portal | **Implemented, integration pending** | Form now stores only SSID/password and never logs the password. `wifi_sta_try_once()` exists, but submit still restarts instead of tearing down AP, validating STA inline, and returning to AP on failure. |
| Provisioning display + QR | **Board verified** | Espressif QR generation, 192 KB PSRAM framebuffer, Wi-Fi QR screen, activation-code screen, native-frame display, refresh, and panel sleep are implemented. The captive portal scans nearby networks in AP+STA mode, returns unique SSIDs strongest-first, supports rescan and hidden-network entry, and marks unsupported open networks. A live scan returned 18 networks; the factory-new frame then paired and restored its photo automatically. |
| Device cloud client | **End-to-end board verified** | Wi-Fi starts the runtime, which registers over TLS and polls every 15 seconds while awake. A claimed device downloads the signed 192,000-byte artifact, verifies SHA-256, refreshes the panel, and persists `last_image_hash`. Dashboard presets select manual-only, 12-hour, daily, or weekly wake. Each online poll synchronizes server time and supplies an absolute next target so slow-clock drift does not accumulate. Independent runtime hashes proved that all 192,000 verified HTTPS bytes reached the SPI driver unchanged; SPI errors propagate instead of being ignored. |
| Cloud functions | **Deployed and live-artifact tested** | All six HTTP functions and the processor are active under the dedicated runtime identity. Register/poll reached the real board; Pub/Sub converted a real upload into an exact 192,000-byte frame and atomically published its manifest. IAM, Scheduler, CORS, event IDs, signed delivery, and Gemini 3.1 Flash Image generation are configured. |
| Website owner flow | **Deployed; upload verified** | `everframe.hoodcharlesd.me` serves Google sign-in, activation, dashboard, and assets through Worker `everframe` version `a0992a98-37d0-40e6-ad1c-10280a37f8a1`. A real upload produced the expected owner-scoped Firestore record and 960×1600 JPEG; JPEG/PNG/HEIC/HEIF validation, editable portrait 3:5 cropping, immediate refresh queueing, and explicit latest-request-wins replacement are implemented. |

Flowchart: **button hold 3 s → QR+AP → STA → POST register → "Connected A7F3K9" + poll (Bearer, 30 s) → claimed → signed URL → display**

> Current normal build: `CONFIG_EVERFRAME_DISABLE_DEEP_SLEEP`,
> `CONFIG_EVERFRAME_DISPLAY_SELF_TEST`, and
> `CONFIG_EVERFRAME_EMBEDDED_HALF_FRAME_TEST` are all disabled. Successful
> delivery sleeps until its selected absolute target; a short-button wake polls every 15 seconds for
> up to five minutes.

> Embedded panel diagnostic history (October 17, 2026): an earlier firmware
> build enabled `CONFIG_EVERFRAME_EMBEDDED_HALF_FRAME_TEST=y`
> and temporarily bypasses identity, Wi-Fi, cloud polling, downloads, and PSRAM.
> It embeds the verified 192,000-byte `e67fc45c...5976` frame in flash. The first
> successful isolation test sent 240 source rows as individual 400-byte
> transactions at the normal 20 MHz SPI clock with a 50 ms delay per row, filled
> the other 240 rows white, refreshed once, and idled. The white half was uniform
> with a sharp boundary and the image half rendered without the prior
> vertical banding. A full 480-row test with identical pacing restored the
> artifact, so the half-frame result did not isolate transport timing; it showed
> that the artifact depends on the dense full-frame pixel pattern. An
> alternating-white-row
> test removed the broad bands while washing out the image. Directly decoding
> the original 192 KB frame then exposed the same directional stripes in the
> framebuffer, identifying Pillow's dither orientation as a contributor. A
> full-density serpentine candidate did not materially improve the panel. The
> active diagnostic dithers in portrait orientation first, then rotates the
> palette indexes into native panel memory. Its
> embedded SHA-256 is `aaffadf12d20ae5289bd70db219d80b33c268df9d55b11725acb044a5dedbb12`;
> the production Python converter now reproduces those exact 192,000 bytes.
> A normal full-frame SPI submission with no per-row delay was visually not
> clean. The active diagnostic therefore uses 50 ms row pacing to display four
> numbered full-frame candidates in order: nearest measured color with no
> dither, CC0 blue-noise-mask dithering, serpentine Burkes, and serpentine
> Atkinson. Each uses the same measured Spectra 6 palette, crop, tone mapping,
> rotation, and packing. Options 1–3 remain visible for 30 seconds after their
> refresh; option 4 remains on the panel when the firmware becomes idle. Direct
> physical review selected option 4 as the production default. The Python cloud
> converter now reproduces the unlabeled Atkinson candidate byte-for-byte; the
> accepted test image produces SHA-256
> `b51a5f065e634aa47f58a1a1f9b800b53c4735d9744c51184cf65f489105360b`.
> Disable this option to restore the cloud runtime.

*Details:* [esp32-provisioning-redesign.md](esp32-provisioning-redesign.md) (full state machine, NVS map, verified SW1 wiring, and remaining checklist)

### 6. End-to-end (with hardware)

- Hold button 3 s → e-paper `WIFI:T:nopass;S:EverFrame-XXXX;;` → phone joins AP → `http://192.168.4.1` → enter home Wi-Fi.
- Tap button while the frame is asleep → reconnect to saved Wi-Fi → open a five-minute update window → poll every 15 seconds until a newly queued frame arrives → refresh and sleep immediately. If nothing new arrives, sleep when the window expires.
- STA connects → `POST /registerDevice` → "Connected ✓ A7F3K9" + QR `everframe.hoodcharlesd.me/activate?code=A7F3K9`.
- Scan QR → Google sign-in → **Link device** → device's next Bearer poll returns `{claimed:true, hash, url}` → fetch 192 KB → display → deep sleep.
- Dashboard **Remove** → authenticated ownership check → cloud record returns to `pending` without deleting its secret or gallery, and clears the prior display acknowledgment → frame shows the same activation code and upgrades to a 30-minute activation window. Showing the code invalidates the device's cached photo hash; `devicePoll` trusts only a hash reported by the device and never substitutes an older cloud value. A successful re-pair therefore downloads and restores the current photo before sleeping. A physical 10-second factory reset instead creates a new identity; remove the old offline record from the dashboard separately.

### Photo capacity and delivery model

The dashboard does not push original photos directly to the ESP. Uploads are
stored in the owner's Cloud Storage gallery, `refreshDevice` queues conversion,
the processor publishes one current per-owner manifest, and the ESP pulls the
manifest's signed 192 KB panel artifact. The scheduled job runs at 00:00 and
12:00 UTC, then generates only for owners whose selected device schedule is
due. Device wake targets are 15 minutes later so the new artifact is ready
before the frame checks. Weekly runs use Sunday; manual-only frames are skipped.

The ESP persistently stores **zero photo files**. It temporarily holds one 192 KB
frame in PSRAM while refreshing the panel, while NVS keeps only the last frame's
SHA-256 hash. The e-paper panel itself retains the displayed picture without
power. Cloud gallery size is currently limited by Storage quota/cost rather than
an application cap; daily artifacts currently accumulate. Every frame claimed
by the same owner receives the same current manifest. Per-device playlists,
ordering, retention limits, and artifact lifecycle cleanup are not implemented.

If another photo is sent while a request is queued, processing, or ready, the
dashboard reports that the pending photo was replaced. Both AI invocations may
still incur cost if the older one had started, but only the newest request ID
may update status, publish a deliverable manifest, or reach the frame.

### What not to do

- Don't commit `website/.env.local` or any private server credentials.
- Don't put `bucket/token` back in the portal — routing is `deviceId→userId` now.
- Don't log `device_secret`; never `==` compare hashes — use `hmac.compare_digest`.
- Don't issue long-lived signed URLs (15 min is plenty).

---

## Map

```
docs/
  README.md                         ← you are here
  cloud-auth-setup.md               ← console + website + cloud + test plan (§5)
  gcp-deployment-2026-10-17.md      ← live deployment record + IAM + rollback
  esp32-provisioning-redesign.md    ← firmware spec + state machine + NVS + pull order
  everframe-flow.excalidraw         ← website activation flowchart
  everframe-secure-flow.excalidraw  ← secure device auth flowchart (Bearer)
cloud/cloud_function/
  main.py                           ← scheduled per-user processor + exported entry points
  epaper.py                         ← deterministic 800×480, six-color, 4-bpp conversion
  device_auth.py                    ← register, claim/remove/status, refresh queue, authenticated device poll
  requirements.txt                  ← + google-cloud-firestore
website/src/
  components/auth/login-form.tsx    ← GoogleSignInButton
  components/auth/auth-gate.tsx     ← wall around /dashboard + /activate
  app/activate/page.tsx             ← QR destination + claim transaction
  app/dashboard/page.tsx            ← per-user Storage paths
  firestore.rules / storage.rules   ← owner-only records and paths; no device listing
```

## Glossary (one line each)

- **deviceId** — 128-bit random per frame (32 hex), persistent in NVS.
- **secret** — 256-bit random per frame (64 hex), persistent in NVS, sent only as `Bearer` over TLS.
- **tokenHash** — `SHA256(secret)` stored in Firestore — DB leak can't impersonate device.
- **activationCode** — 6 chars (first 6 of `deviceId`, uppercased) — what the human types.
- **signed URL** — unguessable `?Signature=RSA…&Expires=…`, 15-min capability for one image.
