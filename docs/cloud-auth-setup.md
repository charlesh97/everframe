# EverFrame Cloud + Website Auth — Setup Guide

> Audience: you, doing the website changes and cloud deploys before firmware
> lands. Updated 2026-08-09 to the **Bearer-secret model** (device holds a
> 256-bit secret, server stores SHA256, every poll is authenticated). This
> supersedes the bearer-less `deviceId`-only draft from `ae0a395`.
> Stack: **Next.js 15** (`website/`) on **Firebase App Hosting**,
> **Firebase Auth (Google)**, **Firestore**, **Cloud Storage**
> (`everframe-photo-bucket`), **Cloud Functions** (`cloud/cloud_function/`).
> Project: `everframe-479214` (us-central1).
> Visual: `docs/everframe-secure-flow.excalidraw` (4 phases).

This is the companion to `esp32-provisioning-redesign.md`. That doc
owns the device side; this one owns everything you touch in console +
`website/` + `cloud/` and how to test it without reflashing.

---

## 0) Where things live

```
website/                      Next.js app (App Hosting)
  src/firebase/config.ts      → imports  ../../firebase-applet-config.json
  src/firebase/index.ts       → initializeApp() with no args in prod,
                                fallback to firebaseConfig locally
  src/components/auth/login-form.tsx   ← GoogleSignInButton (done in 4b611e8)
  src/components/auth/auth-gate.tsx    ← AuthGate wall (done in 4b611e8)
  src/app/activate/page.tsx            ← QR claim page (done in 4b611e8)
  src/app/dashboard/page.tsx  ← upload → users/{uid}/images/... (done in 4b611e8)
  firestore.rules             ← user-ownership + isActivationClaim (done in 4b611e8)
  storage.rules               ← per-user prefix (done in 4b611e8)
  apphosting.yaml             ← maxInstances: 1
  .gitignore                  ← firebase-applet-config.json is ignored
cloud/cloud_function/main.py  ← existing pipeline (random photo → Gemini → save)
  NEW: registerDevice + devicePoll (this doc, §4)
```

Secrets are **not** in repo. `website/firebase-applet-config.json`
is gitignored (the `90a288b` scrub). Prod gets config from
App Hosting env injection. Local dev reads that JSON.

All frames hit **one** Cloud Function endpoint (`devicePoll` + `registerDevice`)
and the function authenticates via `Authorization: Bearer <secret>` (SHA256
compare), then routes `deviceId → userId → manifest/image` via Firestore.

---

## 0b) Auth model — read this first

```
Device (NVS)                 Firestore                  Website (Google)
  deviceId 128-bit  ───────→ devices/{id}
  secret   256-bit  ──SHA256→ tokenHash (stored)         userId (Google uid)
  code     6-char   ───────→ activationCode              claims via /activate?code=
                              status: pending → claimed   updateDoc({userId, claimed})
                              userId: null → uid

Every device request:  Authorization: Bearer <secret>  (TLS)
Server: SHA256(secret) == tokenHash?  no → 401  yes → authorized
Image itself: signed URL 15-min, no auth header needed (capability)
```

- `deviceId` alone is **not** auth — it's an identifier, enumerable. `secret`
  is the password. Firestore only stores `SHA256(secret)` so a DB leak doesn't
  let an attacker impersonate devices.
- Website never sees `secret`. Claim is `pending→claimed` on the Firestore doc
  via `isActivationClaim()` rule; device discovers claim on its next
  authenticated poll (returns `{claimed:true, hash, url}`).
- Guessing `deviceId` without secret → `401`. Intercepted signed URL → usable
  for ~15 min for that one image only.

---

## 1) One-time console setup (10 min, do once)

### 1a) Firebase project

You already have `everframe-479214`. Confirm in console:

- Firebase Console → Project Settings → General → Project ID is
  `everframe-479214`, region `us-central1` (matches `main.py`).

### 1b) Enable Google sign-in

Firebase Console → **Authentication → Sign-in method**:

- [ ] Enable **Google**. Add support email, authorized domain for
  local dev: `localhost`.
- [ ] Leave **Email/Password** enabled until you delete the old form,
      or disable it now — your call. Blueprint declares `auth.providers: ["google"]`
      already. The website now only shows the Google button (commit `4b611e8`).
- [ ] Add authorized domains: `everframe-479214.web.app`,
      `everframe-479214.firebaseapp.com`, and later your custom domain.
- [ ] Under Authentication → Settings → Authorized domains, add `localhost:3000`
      if not present.

### 1c) Firestore + Storage

Should already exist, but confirm:

- Firestore → Data → collections `users`, `devices`, `images`, `abstractArt`
  can be created on first write (rules create them).
- Storage → bucket `everframe-photo-bucket` exists in `us-central1`.
- Copy `storage.rules` and `firestore.rules` from repo — review §3 below
  before deploying. The rules in `4b611e8` already include the claim logic.

### 1d) Service account roles

Cloud Console → IAM → find the Cloud Functions service account
(`everframe-479214@appspot.gserviceaccount.com` or
`PROJECT_ID@appspot.gserviceaccount.com`):

- `roles/storage.objectAdmin` on `everframe-photo-bucket` (read + write images, generate signed URLs)
- `roles/datastore.user` or `roles/firestore.user` (read/write `/devices`, `/images`)
- `roles/aiplatform.user` (Vertex AI Gemini/Imagen — for the existing pipeline)
- `roles/secretmanager.secretAccessor` if you keep `get_secret()` in `main.py`
- `roles/pubsub.publisher` if you keep the Pub/Sub notify path

No extra role for `registerDevice`/`devicePoll` verification — it uses Firestore
`tokenHash` compare, not IAM.

---

## 2) Website — already landed in 4b611e8 (what to verify)

You asked for "whatever we need on the website side" — this section documents
what shipped in `4b611e8` and what remains.

### 2a) What shipped (no action unless you want to tweak)

| File | What it does |
|------|--------------|
| `src/components/auth/login-form.tsx` | `GoogleSignInButton` (`GoogleAuthProvider` + `signInWithPopup`, `prompt:'select_account'`). Old email export kept as alias. |
| `src/firebase/non-blocking-login.tsx` | `initiateGoogleSignIn()` helper; legacy `initiateEmail*` kept as `@deprecated`. |
| `src/components/auth/auth-gate.tsx` | `AuthGate` wall: `useUser()` → `isUserLoading` → `user`? show / redirect to `/`. Wraps dashboard. |
| `src/app/dashboard/layout.tsx` | Now `<FirebaseClientProvider><AuthGate>{children}</AuthGate></FirebaseClientProvider>`. |
| `src/app/dashboard/page.tsx` | Upload path → `users/${uid}/images/...` (was flat `images/...`). Matches per-user routing. |
| `src/app/activate/page.tsx` | New. QR destination `?code=A7F3K9`. `useSearchParams()` pre-fill, `query(... where code, where pending)` + `updateDoc({userId, claimed})` → `router.push('/dashboard')`. Suspense-wrapped for Next 15. |
| `firestore.rules` | Added `isActivationClaim()` (pending→claimed, `userId==auth.uid`) + `allow list` for the claim query. |
| `storage.rules` | Added `match /users/{userId}/**` scoped to `auth.uid==userId`, kept flat `images/` for migration. |

### 2b) Gate the app (already applied — verify)

The gate is live. Check: unauthed visit to `/dashboard` or `/activate` →
`router.replace('/')` with no flash (returns `null`). `/` stays public.

### 2c) Config file that makes it run locally

`website/src/firebase/config.ts`:

```ts
import firebaseAppletConfig from '../../firebase-applet-config.json'
export const firebaseConfig = firebaseAppletConfig
```

`website/src/firebase/index.ts` already does the right thing — try
`initializeApp()` with no args (App Hosting injects), catch and fall back
to that JSON locally:

```ts
try { firebaseApp = initializeApp() }
catch { firebaseApp = initializeApp(firebaseConfig) }
```

**You need** `website/firebase-applet-config.json` locally (gitignored).
Create it once — never commit:

```bash
# Firebase Console → Project Settings → General → Your apps → SDK setup
# Copy the firebaseConfig object and save as:
cat > website/firebase-applet-config.json <<'JSON'
{
  "apiKey": "AIza…",
  "authDomain": "everframe-479214.firebaseapp.com",
  "projectId": "everframe-479214",
  "storageBucket": "everframe-photo-bucket.appspot.com",
  "messagingSenderId": "…",
  "appId": "1:…:web:…"
}
JSON
```

Verify `.gitignore` has `firebase-applet-config.json` (it does) and
`.env*`. Confirm with `git status --ignored`.

### 2d) What the website does NOT do (by design)

- Never handles Wi-Fi SSID/pass.
- Never sees `device_secret`.
- Never hardcodes per-frame config — the `/activate` claim is the only
  per-device action, and it's one `updateDoc` on a `pending` doc.

---

## 3) Rules (deployed in 4b611e8 — verify in console)

### Firestore — `website/firestore.rules`

- `users/{userId}` → `isOwner(userId)` only.
- `images/{imageId}` → `isSignedIn()` (shared gallery today).
- `devices/{deviceId}` → new logic:
  ```js
  function isActivationClaim() {
    return isSignedIn()
      && isExistingDoc()
      && resource.data.status == 'pending'
      && request.resource.data.status == 'claimed'
      && request.resource.data.userId == request.auth.uid;
  }
  // get: owner only
  // list: isSignedIn() — required for the /activate query (getDocs where code)
  // update: (owner && immutable) || isActivationClaim()
  ```
- `abstractArt/{id}` → all `false` (firewall, missing `userId`).

In Rules Playground, test an authed `update` of a `pending` doc setting
`status:'claimed'` + `userId: auth.uid` → should allow via `isActivationClaim`.

### Storage — `website/storage.rules`

```js
match /b/{bucket}/o {
  match /users/{userId}/{allPaths=**} {
    allow read, write: if request.auth != null && request.auth.uid == userId;
  }
  match /images/{allPaths=**} {
    allow read, write: if request.auth != null;
  }
  match /{allPaths=**} {
    allow read, write: if request.auth != null;
  }
}
```

Device fetches do **not** use Firebase Auth — they get a **signed URL** from
the Cloud Function (service-account-issued, 15-min expiry). No Storage rule
change is needed for ESP32.

Deploy (if you edit rules again):

```bash
firebase deploy --only firestore:rules,storage
```

---

## 4) Cloud: two functions, one endpoint pattern, every call authenticated

### 4a) New function: `registerDevice` (HTTP)

Device calls this once after Wi-Fi connects, before showing the activation
screen. No `Authorization` header — the secret hasn't been verified yet.
Idempotent: `409 already exists` is success (same `tokenHash`).

**Endpoint:** `POST https://us-central1-everframe-479214.cloudfunctions.net/registerDevice`
(or `everframe-479214.web.app/api/registerDevice` via Hosting rewrites).

**Request JSON:**

```json
{ "deviceId": "a3f9...32hex", "tokenHash": "7b...64hex SHA256(secret)", "activationCode": "A7F3K9" }
```

**Logic:**

```py
import hashlib, json
from google.cloud import firestore

PROJECT_ID = "everframe-479214"
db = firestore.Client(project=PROJECT_ID)

def register_device(request):
    body = request.get_json(silent=True) or {}
    device_id = (body.get("deviceId") or "").strip().lower()
    token_hash = (body.get("tokenHash") or "").strip().lower()
    code = (body.get("activationCode") or "").strip().upper()

    if not device_id or len(device_id) != 32 or not token_hash or len(token_hash) != 64 or not code or len(code) != 6:
        return (json.dumps({"error": "invalid deviceId/tokenHash/code"}), 400, {"Content-Type": "application/json"})

    ref = db.collection("devices").document(device_id)
    snap = ref.get()
    if snap.exists:
        # already registered — verify the hash matches (same device re-registering)
        if snap.get("tokenHash") and snap.get("tokenHash") != token_hash:
            return (json.dumps({"error": "tokenHash mismatch — factory reset?"}), 409, {"Content-Type": "application/json"})
        return (json.dumps({"ok": True, "deviceId": device_id, "code": snap.get("activationCode") or code}), 200, {"Content-Type": "application/json"})

    ref.set({
        "activationCode": code,
        "tokenHash": token_hash,
        "status": "pending",
        "createdAt": firestore.SERVER_TIMESTAMP,
    })
    return (json.dumps({"ok": True, "deviceId": device_id, "code": code}), 200, {"Content-Type": "application/json"})
```

### 4b) New function: `devicePoll` (HTTP, authenticated)

All frames hardcode one URL. Every call carries `Authorization: Bearer <secret>`.

**Endpoint:** `GET https://us-central1-everframe-479214.cloudfunctions.net/devicePoll`
(or `everframe-479214.web.app/api/device/next` via rewrite).

**Request headers:**

```
Authorization: Bearer <64-hex secret>
X-Device-Id: <32-hex deviceId>
```

`deviceId` may also be supplied as `?deviceId=` query param for `curl` testing,
but `Authorization` is required in either case.

**Logic:**

```py
import hashlib, hmac, json, datetime
from google.cloud import firestore, storage

PROJECT_ID = "everframe-479214"
BUCKET_NAME = "everframe-photo-bucket"

db = firestore.Client(project=PROJECT_ID)
gcs = storage.Client(project=PROJECT_ID)

def _unauth(msg="unauthorized"):
    return (json.dumps({"error": msg}), 401, {"Content-Type": "application/json"})

def device_poll(request):
    # --- auth: extract Bearer + deviceId ---
    auth = request.headers.get("Authorization", "")
    if not auth.startswith("Bearer "):
        return _unauth("missing Bearer token")
    secret = auth[len("Bearer "):].strip().lower()
    if len(secret) != 64:
        return _unauth("invalid token length")

    device_id = request.headers.get("X-Device-Id", "") or request.args.get("deviceId", "") or (request.get_json(silent=True) or {}).get("deviceId", "")
    device_id = device_id.strip().lower()
    if not device_id or len(device_id) != 32:
        return _unauth("missing deviceId")

    # --- load device, verify hash (constant-time) ---
    doc = db.collection("devices").document(device_id).get()
    if not doc.exists or not doc.get("tokenHash"):
        return _unauth("unknown device — register first")

    expected = doc.get("tokenHash").lower()
    computed = hashlib.sha256(bytes.fromhex(secret)).hexdigest()
    if not hmac.compare_digest(computed, expected):
        return _unauth("token mismatch")

    # --- pending → tell device it hasn't been claimed yet ---
    if doc.get("status") != "claimed" or not doc.get("userId"):
        return (json.dumps({"claimed": False}), 200, {"Content-Type": "application/json"})

    user_id = doc.get("userId")

    # --- manifest lookup, per-user ---
    manifest_blob = gcs.bucket(BUCKET_NAME).blob(f"users/{user_id}/manifest.json")
    if not manifest_blob.exists():
        return (json.dumps({"claimed": True, "hasImage": False}), 200, {"Content-Type": "application/json"})

    manifest = json.loads(manifest_blob.download_as_string())
    # manifest = {"image": "users/{uid}/daily/2026-08-10.bin", "hash": "sha256hex"}

    last_hash = doc.get("lastImageHash")
    if last_hash and last_hash == manifest["hash"]:
        return (json.dumps({"claimed": True, "upToDate": True, "hash": manifest["hash"]}), 200, {"Content-Type": "application/json"})

    # --- issue signed URL (15 min) for the 192 KB bin ---
    blob = gcs.bucket(BUCKET_NAME).blob(manifest["image"])
    url = blob.generate_signed_url(
        version="v4",
        expiration=datetime.timedelta(minutes=15),
        method="GET",
    )
    return (json.dumps({"claimed": True, "hash": manifest["hash"], "url": url}), 200, {"Content-Type": "application/json"})
```

Wire as HTTP function (`functions_framework.http` or `functions.https.onRequest`
if you move to Node). Its service account needs `storage.objectViewer` +
`firestore.user`. Use `hmac.compare_digest` — constant-time — never `==`.

### 4c) Existing pipeline: write per-user

Update `cloud/cloud_function/main.py` `hello_pubsub` to write under
`users/{uid}/daily/{date}.bin` and `users/{uid}/manifest.json` instead of
flat bucket root. Pick `uid` from the image owner's Firestore doc or from the
Pub/Sub trigger payload — don't guess. If you keep the random-photo path for
now, write to a single owner's prefix and document whose.

The pipeline already does `resize → quantize 6-color E palette → pack 4bpp →
192 KB bin` server-side (device is a dumb terminal).

### 4d) Hosting rewrites (optional polish)

In `website/firebase.json` (create if missing), alias both functions:

```json
{
  "hosting": {
    "rewrites": [
      { "source": "/api/registerDevice", "function": "registerDevice" },
      { "source": "/api/device/next", "function": "devicePoll" }
    ]
  }
}
```

Then firmware hits `https://everframe-479214.web.app/api/device/next`
— same origin as the website, one domain in NVS/config.

### 4e) Deploy

```bash
# Functions (Gen2) — registerDevice
gcloud functions deploy registerDevice \
  --gen2 --runtime python311 --region us-central1 \
  --source cloud/cloud_function --entry-point register_device \
  --trigger-http --allow-unauthenticated \
  --set-env-vars PROJECT_ID=everframe-479214,BUCKET_NAME=everframe-photo-bucket

# devicePoll
gcloud functions deploy devicePoll \
  --gen2 --runtime python311 --region us-central1 \
  --source cloud/cloud_function --entry-point device_poll \
  --trigger-http --allow-unauthenticated \
  --set-env-vars PROJECT_ID=everframe-479214,BUCKET_NAME=everframe-photo-bucket

# Or via Firebase CLI if you add functions/ to website/
firebase deploy --only functions:registerDevice,functions:devicePoll
```

`--allow-unauthenticated` is correct — auth is `Bearer <secret>` in code,
not GCP IAM. Rate-limit in code if needed.

---

## 5) Test plan (run in order, no firmware needed for 5a–5c)

### 5a) Website auth

```bash
cd website
npm ci
npm run dev  # http://localhost:3000
```

Checklist:

- [ ] `/` loads without auth.
- [ ] Clicking "Continue with Google" opens popup, completes, redirects to `/dashboard` (via `AuthGate`).
- [ ] DevTools → Application → IndexedDB → `firebaseLocalStorageDb` has a user.
- [ ] Refresh `/dashboard` — still authed (onAuthStateChanged), no flicker to login.
- [ ] Sign out (add a button calling `signOut(auth)`), verify gate redirects to `/`.
- [ ] `npm run build` passes; `firebase emulators:start` if you use emulators.

If `firebase-applet-config.json` is missing, `npm run build` fails with
`Cannot find module '../../firebase-applet-config.json'` — create it per §1c.

### 5b) Firestore/Storage rules

In Firebase Console → Firestore → Rules Playground:

- [ ] Unauthed `get /users/uid` → denied.
- [ ] Authed `get /users/{own uid}` → allowed.
- [ ] Authed `update /devices/{id}` pending→claimed with `userId == auth.uid` → allowed (via `isActivationClaim`).
- [ ] Authed `update /devices/{otherUsersId}` → denied.
- [ ] Authed `list` query `devices where activationCode==X && status==pending` → allowed.

Storage console: upload a file to `users/{uid}/images/test.png` via the dashboard
upload flow, verify it appears under the correct prefix.

### 5c) `/activate` claim (no device needed — seed a doc)

- [ ] Visit `http://localhost:3000/activate?code=A7F3K9` while signed in → shows pre-filled code.
- [ ] Seed a `devices/{testDeviceId}` doc manually (Console → Firestore → Add document: `activationCode: "A7F3K9", status: "pending", tokenHash: "<64hex>"`).
- [ ] Submit claim → doc becomes `status: "claimed", userId: "<your uid>"`.
- [ ] Revisit same code → shows "already claimed" (query returns 0).

### 5d) Cloud endpoints (after deploy — no firmware)

Use a throwaway secret you generated with `openssl rand -hex 32`:

```bash
SECRET=$(openssl rand -hex 32)
HASH=$(echo -n "$SECRET" | xxd -r -p | shasum -a 256 | awk '{print $1}')
DEVICE=a3f9e2c1b4d60718293a4b5c6d7e8f90

# register
curl -X POST https://us-central1-everframe-479214.cloudfunctions.net/registerDevice \
  -H "Content-Type: application/json" \
  -d "{\"deviceId\":\"$DEVICE\",\"tokenHash\":\"$HASH\",\"activationCode\":\"A7F3K9\"}"
# → {"ok":true}

# poll while pending — should be claimed:false
curl "https://us-central1-everframe-479214.cloudfunctions.net/devicePoll" \
  -H "Authorization: Bearer $SECRET" -H "X-Device-Id: $DEVICE"
# → {"claimed":false}

# poll with wrong secret — should be 401
curl "https://us-central1-everframe-479214.cloudfunctions.net/devicePoll" \
  -H "Authorization: Bearer 00000000000000000000000000000000000000000000000000000000000000000000" \
  -H "X-Device-Id: $DEVICE"
# → {"error":"token mismatch"}  401

# claim via website: visit /activate?code=A7F3K9 while signed in → claim

# poll after claim — needs manifest seeded:
# Firestore: devices/{DEVICE} { userId: "<uid>", status: "claimed", lastImageHash: "" }
# Storage: users/<uid>/manifest.json {"image":"users/<uid>/daily/2026-08-10.bin","hash":"abc"}
# Storage: users/<uid>/daily/2026-08-10.bin  (any 192 KB file for test)

curl "https://us-central1-everframe-479214.cloudfunctions.net/devicePoll" \
  -H "Authorization: Bearer $SECRET" -H "X-Device-Id: $DEVICE"
# → {"claimed":true,"hash":"abc","url":"https://storage.googleapis.com/...&Expires=..."}
curl -L -o /tmp/test.bin "<url>"
ls -lh /tmp/test.bin  # should be 192000 bytes for real art

# idempotent: second poll with same manifest hash → upToDate
curl "https://us-central1-everframe-479214.cloudfunctions.net/devicePoll" \
  -H "Authorization: Bearer $SECRET" -H "X-Device-Id: $DEVICE"
# (after device or you set lastImageHash to "abc" on the doc) → {"upToDate":true}
```

### 5e) End-to-end (needs firmware per other doc)

After the ESP32 changes in `esp32-provisioning-redesign.md` land:

- [ ] Hold button 3 s → e-paper shows QR `WIFI:T:nopass;S:EverFrame-XXXX;;`.
- [ ] Phone scans QR, joins AP, portal at `http://192.168.4.1` shows SSID/pass only.
- [ ] Submit home Wi-Fi → STA connects, `POST /registerDevice` → e-paper flips to
      "Connected ✓  A7F3K9" + claim QR. Poll loop starts (`{claimed:false}`).
- [ ] Scan claim QR → `/activate?code=A7F3K9` → signed in → device appears under account.
- [ ] Next poll (30 s or next wake) → `GET /devicePoll + Bearer` → `{hash, url}` → display refresh.
- [ ] Deep-sleep wake next day → same `Bearer` poll → hash compare → display or sleep.

---

## 6) What not to do

- Do not commit `firebase-applet-config.json`, `.env`, or any `apiKey` to git.
  `90a288b` already scrubbed this once.
- Do not put `bucket/token` back into the portal form — it defeats the
  single-endpoint routing.
- Do not log or echo `device_secret` — only `deviceId` and `activationCode`
  are safe to log. `tokenHash` is in Firestore; keep it out of client logs.
- Do not use `==` for hash compare — use `hmac.compare_digest` (constant-time).
- Do not issue long-lived Storage signed URLs — 15 min is plenty; device
  re-polls daily anyway.

---

## 7) Rollout order (recommended)

1. Land website Google auth + gate + `firebase-applet-config.json` instructions
   — done in `4b611e8`, verify §5a–5c locally then `firebase deploy --only hosting,firestore:rules,storage`.
2. Deploy `registerDevice` + `devicePoll` (§4a–4b, §4e) — test §5d with
   `openssl rand -hex 32` + manual Firestore/Storage seeds, verify `401` on bad secret.
3. Then implement `docs/esp32-provisioning-redesign.md` — `device_identity` (secret
   generation + SHA256), `cloud_client` (register + Bearer poll), button, QR, portal trim.

When you push the website branch, run `firebase deploy --only hosting,firestore:rules,storage` and paste the Hosting URL here; I'll sanity-check the auth flow.
