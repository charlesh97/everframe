# EverFrame — Docs

> You are here. Pick your starting point by what you want to do.

| I want to… | Start here |
|------------|-----------|
| **Set up cloud + website** (run the thing) | [`cloud-auth-setup.md`](cloud-auth-setup.md) — console clicks, Google sign-in, deploy functions |
| **Build or change firmware** (button, QR, display, auth) | [`esp32-provisioning-redesign.md`](esp32-provisioning-redesign.md) — state machine, NVS, file checklist |
| **See how it all fits together** | [Website activation flowchart](https://excalidraw.com/#json=arr4bjbIGcGfSb-QoUbpL,07p7lUawkm2_jj5j_VflbQ) + [Secure device auth flowchart](https://excalidraw.com/#json=Izs2P1VGbzmWCG6pmb7t5,dg3ddAt_Qn7-ZQipjy-0iw) — open in browser, also saved as `.excalidraw` in this folder |
| **Deploy without reading both specs** | [Runbook](#runbook) below — copy-paste sequence |

---

## Runbook — start to finish

Do these in order. Each step says what to run and how to know it worked.

### 0. Prereqs

```bash
# from repo root ~/git/everframe
npm --version && node --version   # Node 20+
firebase --version 2>&1 | head -1 # Firebase CLI; or use gcloud
gcloud --version 2>&1 | head -1   # optional, for --allow-unauthenticated deploys
```

### 1. Firebase console — 10 minutes, one time

1. Firebase Console → `everframe-479214` → **Authentication → Sign-in method** → enable **Google**. Add `localhost` + `everframe-479214.web.app` to authorized domains.
2. **Firestore** + **Storage** (`everframe-photo-bucket`, `us-central1`) already exist — just confirm they're there.
3. **IAM** → Cloud Functions service account → `storage.objectAdmin` + `firestore.user` (+ existing `aiplatform.user` / `pubsub.publisher` / `secretmanager.secretAccessor`).

*Details:* [cloud-auth-setup §1](cloud-auth-setup.md#1-one-time-console-setup-10-min-do-once)

### 2. Website local — website is the owner portal

```bash
# 1) Firebase web config — get from Project Settings → Your apps → SDK setup
cat > website/firebase-applet-config.json <<'JSON'
{ "apiKey":"AIza…", "authDomain":"everframe-479214.firebaseapp.com",
  "projectId":"everframe-479214", "storageBucket":"everframe-photo-bucket.appspot.com",
  "messagingSenderId":"…", "appId":"1:…:web:…" }
JSON
# never commit this file — it's gitignored (scrubbed in 90a288b)

# 2) install + typecheck + build
cd website && npm ci && npm run build
# expect: 2 warnings (missing config in CI + pre-existing _query) → build passes

# 3) run locally
npm run dev   # http://localhost:3000
# /           → Google button only
# /dashboard  → redirects to / if not signed in (AuthGate)
# /activate?code=A7F3K9 → handles claim after you sign in
```

*Details:* [cloud-auth-setup §2](cloud-auth-setup.md#2-website--already-landed-in-4b611e8-what-to-verify) · website changes landed in `4b611e8`

### 3. Rules + Hosting — deploy

```bash
firebase deploy --only firestore:rules,storage,hosting
# paste the Hosting URL here and I'll sanity-check auth
```

Verify in console **Rules Playground**:
- unauthed `get /users/{uid}` → denied, authed own → allowed
- authed `update /devices/{id}` `pending→claimed` with `userId==auth.uid` → allowed

*Details:* [cloud-auth-setup §3](cloud-auth-setup.md#3-rules-deployed-in-4b611e8--verify-in-console)

### 4. Cloud Functions — authenticated device auth

Two new HTTP functions in `cloud/cloud_function/device_auth.py` (alongside existing `main.py` / `hello_pubsub`):

- `POST /registerDevice` — `{"deviceId":32hex, "tokenHash":64hex SHA256(secret), "activationCode":"A7F3K9"}` → `pending`
- `GET  /devicePoll` — headers `Authorization: Bearer <64hex secret>` + `X-Device-Id` → `{claimed:false}` or `{claimed:true, hash, url}` (15-min signed URL)

```bash
gcloud functions deploy registerDevice --gen2 --runtime python311 --region us-central1 \
  --source cloud/cloud_function --entry-point register_device --trigger-http --allow-unauthenticated \
  --set-env-vars PROJECT_ID=everframe-479214,BUCKET_NAME=everframe-photo-bucket

gcloud functions deploy devicePoll --gen2 --runtime python311 --region us-central1 \
  --source cloud/cloud_function --entry-point device_poll --trigger-http --allow-unauthenticated \
  --set-env-vars PROJECT_ID=everframe-479214,BUCKET_NAME=everframe-photo-bucket
# --allow-unauthenticated is correct — auth is Bearer secret + SHA256(tokenHash) compare, not IAM
```

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

*Details:* [cloud-auth-setup §4](cloud-auth-setup.md#4-cloud-two-functions-one-endpoint-pattern-every-call-authenticated) · [esp32 spec §1b security model](esp32-provisioning-redesign.md#1b-security-model--why-this-and-not-bare-deviceid)

### 5. Firmware — when you pick up the board

`embedded/everframe-esp32` (IDF 6.0.2, XIAO ESP32S3 + 800×480 6-color e-paper). Current build `d24fef2` has Wi-Fi provisioning; next is button→QR→Bearer polling.

| Order | Module | What it does |
|-------|--------|--------------|
| 1 | `device_identity.c` | `deviceId` 128-bit + `device_secret` 256-bit generation, `tokenHash=SHA256(secret)`, activation code |
| 2 | `button.c` + wake | 3 s hold → AP, 10 s → factory erase, `ext0` wake from deep sleep |
| 3 | `qrcodegen` + `display_provisioning.c` | vendored QR → e-paper (Wi-Fi QR + claim QR) |
| 4 | `cloud_client.c` | `registerDevice` + Bearer `devicePoll` + signed-URL 192 KB fetch (mbedTLS + cert bundle) |
| 5 | `wifi_prov.c` trim + `wifi_sta_try_once` + `main.c` wiring | SSID/pass-only portal, inline STA try, 30 min AP poll loop |

Flowchart: **button hold 3 s → QR+AP → STA → POST register → "Connected A7F3K9" + poll (Bearer, 30 s) → claimed → signed URL → display**

*Details:* [esp32-provisioning-redesign.md](esp32-provisioning-redesign.md) (full checklist, NVS map, wiring notes for SW1/GPIO0)

### 6. End-to-end (with hardware)

- Hold button 3 s → e-paper `WIFI:T:nopass;S:EverFrame-XXXX;;` → phone joins AP → `http://192.168.4.1` → enter home Wi-Fi.
- STA connects → `POST /registerDevice` → "Connected ✓ A7F3K9" + QR `everframe.app/activate?code=A7F3K9`.
- Scan QR → Google sign-in → **Link device** → device's next Bearer poll returns `{claimed:true, hash, url}` → fetch 192 KB → display → deep sleep.

### What not to do

- Don't commit `website/firebase-applet-config.json` or any `apiKey`.
- Don't put `bucket/token` back in the portal — routing is `deviceId→userId` now.
- Don't log `device_secret`; never `==` compare hashes — use `hmac.compare_digest`.
- Don't issue long-lived signed URLs (15 min is plenty).

---

## Map

```
docs/
  README.md                         ← you are here
  cloud-auth-setup.md               ← console + website + cloud + test plan (§5)
  esp32-provisioning-redesign.md    ← firmware spec + state machine + NVS + pull order
  everframe-flow.excalidraw         ← website activation flowchart
  everframe-secure-flow.excalidraw  ← secure device auth flowchart (Bearer)
cloud/cloud_function/
  main.py                           ← existing: Pub/Sub hello_pubsub pipeline
  device_auth.py                    ← new: register_device + device_poll (HTTP, Bearer)
  requirements.txt                  ← + google-cloud-firestore
website/src/
  components/auth/login-form.tsx    ← GoogleSignInButton
  components/auth/auth-gate.tsx     ← wall around /dashboard + /activate
  app/activate/page.tsx             ← QR destination + claim transaction
  app/dashboard/page.tsx            ← per-user Storage paths
  firestore.rules / storage.rules   ← isActivationClaim + per-user prefix
```

## Glossary (one line each)

- **deviceId** — 128-bit random per frame (32 hex), persistent in NVS.
- **secret** — 256-bit random per frame (64 hex), persistent in NVS, sent only as `Bearer` over TLS.
- **tokenHash** — `SHA256(secret)` stored in Firestore — DB leak can't impersonate device.
- **activationCode** — 6 chars (first 6 of `deviceId`, uppercased) — what the human types.
- **signed URL** — unguessable `?Signature=RSA…&Expires=…`, 15-min capability for one image.
