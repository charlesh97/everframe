# EverFrame Cloud + Website Auth — Setup Guide

> Audience: you, doing the website changes by hand before firmware lands.
> Stack: **Next.js 15** (`website/`) on **Firebase App Hosting**,
> **Firebase Auth (Google)**, **Firestore**, **Cloud Storage**
> (`everframe-photo-bucket`), **Cloud Functions** (`cloud/cloud_function/`).
> Project: `everframe-479214` (us-central1).

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
  src/components/auth/login-form.tsx   ← email form today
  src/firebase/provider.tsx   ← onAuthStateChanged → useUser()
  src/app/dashboard/page.tsx  ← upload → Storage + Firestore
  firestore.rules             ← user-ownership model
  storage.rules               ← bucket everframe-photo-bucket
  apphosting.yaml             ← maxInstances: 1
  .gitignore                  ← firebase-applet-config.json is ignored
cloud/cloud_function/main.py  ← Pub/Sub handler (random photo → Gemini → save)
```

Secrets are **not** in repo. `website/firebase-applet-config.json`
is gitignored (that's the `90a288b` scrub). Prod gets config from
App Hosting env injection. Local dev reads that JSON.

All frames will hit **one** Cloud Function endpoint and the function
routes `deviceId → userId → manifest/image` via Firestore. No per-frame
bucket/token in firmware after the redesign.

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
      already.
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
  before deploying.

### 1d) Service account roles

Cloud Console → IAM → find the Cloud Functions service account
(`everframe-479214@appspot.gserviceaccount.com` or
`PROJECT_ID@appspot.gserviceaccount.com`):

- `roles/storage.objectAdmin` on `everframe-photo-bucket` (read + write images, generate signed URLs)
- `roles/datastore.user` or `roles/firestore.user` (read/write `/devices`, `/images`)
- `roles/aiplatform.user` (Vertex AI Gemini/Imagen — for the existing pipeline)
- `roles/secretmanager.secretAccessor` if you keep `get_secret()` in `main.py`
- `roles/pubsub.publisher` if you keep the Pub/Sub notify path

---

## 2) Website: Google-OAuth-only (what you're about to change)

### 2a) Replace the login form

**File:** `website/src/components/auth/login-form.tsx`

Current: `signInWithEmailAndPassword` with email+password inputs.
Replace with:

```tsx
'use client'
import { GoogleAuthProvider, signInWithPopup } from 'firebase/auth'
import { useAuth } from '@/firebase'
import { Button } from '@/components/ui/button'

export function GoogleSignInButton() {
  const auth = useAuth()
  const provider = new GoogleAuthProvider()
  // Optional: force account picker each time
  // provider.setCustomParameters({ prompt: 'select_account' })
  const onClick = async () => {
    await signInWithPopup(auth, provider)
    // onAuthStateChanged in provider.tsx will fire; router can push /dashboard
  }
  return <Button onClick={onClick}>Continue with Google</Button>
}
```

Keep `src/firebase/non-blocking-login.tsx` helpers if you like, but add:

```ts
export function initiateGoogleSignIn(auth: Auth) {
  const p = new GoogleAuthProvider()
  signInWithPopup(auth, p).catch(e => console.error(e))
}
```

Update `website/firebase-blueprint.json` `auth.providers` already says
`["google"]` — no change needed there.

### 2b) Gate the app

**File:** `website/src/app/dashboard/layout.tsx` (or `page.tsx`)

```tsx
'use client'
import { useUser } from '@/firebase'
import { useRouter } from 'next/navigation'
import { useEffect } from 'react'

export default function DashboardGate({ children }: { children: React.ReactNode }) {
  const { user, isUserLoading } = useUser()
  const router = useRouter()
  useEffect(() => {
    if (!isUserLoading && !user) router.replace('/')
  }, [isUserLoading, user, router])
  if (isUserLoading) return <p>Loading…</p>
  if (!user) return null
  return <>{children}</>
}
```

Marketing page `/` stays public; `/dashboard` and `/activate` require `user`.

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

### 2d) Storage upload path (per-user, not flat)

Today `dashboard/page.tsx` does:

```ts
storagePath = `images/${imageName}.png`         // flat, shared
```

Change to per-user (rules + device lookup both expect this):

```ts
const { user } = useUser()
storagePath = `users/${user.uid}/images/${imageName}.png`
storagePathDaily = `users/${user.uid}/daily/${date}.bin` // for the 192 KB bin
```

The cloud pipeline should write its daily `.bin` and `manifest.json`
under the same `users/{uid}/` prefix (see §4).

### 2e) New route: `/activate` (device claim)

**File:** `website/src/app/activate/page.tsx` (new)

- Reads `?code=A7F3K9` from `searchParams`.
- Shows Google sign-in if `!user`.
- Input pre-filled with `code`, button "Link device".
- On submit: query Firestore `devices` where `activationCode == code && status == 'pending'`
  (single `getDocs` with `where`), then `updateDoc(deviceRef, { userId: user.uid, status: 'claimed', deviceName: optional })`.
- Success → redirect to `/dashboard` (or `/devices` tab when you add it).

This is where the QR on the e-paper lands:
`https://everframe-479214.web.app/activate?code=A7F3K9`.

---

## 3) Rules (deploy after editing)

### Firestore — `website/firestore.rules`

Key points (already in repo, keep):

- `users/{userId}` → `isOwner(userId)` only.
- `devices/{deviceId}` → `existingDocIsOwnedBy(request.auth.uid)` on get/update/delete,
  `newDocHasCorrectOwner` on create, and `isUserIdImmutable()`. Listing disabled.
- `images/{imageId}` → `isSignedIn()` (shared gallery today).
- `abstractArt/{id}` → all `false` (schema missing `userId` — intentional firewall).

**Add** for the claim flow — allow an authenticated user to claim a pending device:

```js
// inside match /devices/{deviceId}
allow update: if isSignedIn()
  && resource.data.status == 'pending'
  && request.resource.data.userId == request.auth.uid
  && request.resource.data.status == 'claimed'
  && isUserIdImmutable() == false  // allow setting userId once from null/empty
```

Simpler: keep your current `isExistingDoc() && existingDocIsOwnedBy` for normal
updates, add a second `allow update` for the pending→claimed transition:

```js
allow update: if isSignedIn() && resource.data.status == 'pending'
  && request.resource.data.status == 'claimed'
  && request.resource.data.userId == request.auth.uid;
```

Deploy:

```bash
firebase deploy --only firestore:rules
```

### Storage — `website/storage.rules`

Current: `match /b/{bucket}/o/{allPaths=**} { allow read, write: if request.auth != null }`
on `everframe-photo-bucket`.

This is fine for website uploads (user's ID token). For the per-user layout above,
tighten when ready:

```js
match /b/{bucket}/o {
  match /users/{userId}/{allPaths=**} {
    allow read, write: if request.auth != null && request.auth.uid == userId;
  }
  // keep flat images readable during migration
  match /images/{allPaths=**} {
    allow read, write: if request.auth != null;
  }
}
```

Device fetches do **not** use Firebase Auth — they get a **signed URL** from
the Cloud Function (service-account-issued, 15-min expiry). So no Storage rule
change is needed for ESP32.

Deploy:

```bash
firebase deploy --only storage
```

---

## 4) Cloud: single endpoint for all frames

### 4a) New function: `devicePoll` (HTTP, not Pub/Sub)

All frames hardcode one URL. No bucket/token in firmware.

**Endpoint:** `https://us-central1-everframe-479214.cloudfunctions.net/devicePoll`
(or `everframe-479214.web.app/api/device/next` if you front with Hosting rewrites).

**Request:** `GET /devicePoll?deviceId=abc123…` (128-bit hex from NVS).

**Logic (Python sketch, adapt to your `main.py` style):**

```py
import json
from google.cloud import firestore, storage
import datetime

PROJECT_ID = "everframe-479214"
BUCKET_NAME = "everframe-photo-bucket"

db = firestore.Client(project=PROJECT_ID)
gcs = storage.Client(project=PROJECT_ID)

def device_poll(request):
    device_id = request.args.get("deviceId") or (request.get_json(silent=True) or {}).get("deviceId")
    if not device_id or len(device_id) < 16:
        return (json.dumps({"error": "missing deviceId"}), 400, {"Content-Type": "application/json"})

    doc = db.collection("devices").document(device_id).get()
    if not doc.exists or not doc.get("userId"):
        return (json.dumps({"claimed": False}), 200, {"Content-Type": "application/json"})

    user_id = doc.get("userId")
    manifest_blob = gcs.bucket(BUCKET_NAME).blob(f"users/{user_id}/manifest.json")
    if not manifest_blob.exists():
        return (json.dumps({"claimed": True, "hasImage": False}), 200, {"Content-Type": "application/json"})

    manifest = json.loads(manifest_blob.download_as_string())
    # manifest = {"image": "users/{uid}/daily/2026-08-10.bin", "hash": "sha256hex"}
    last_hash = doc.get("lastImageHash")
    if last_hash and last_hash == manifest["hash"]:
        return (json.dumps({"claimed": True, "upToDate": True, "hash": manifest["hash"]}), 200, {"Content-Type": "application/json"})

    # issue signed URL (15 min) for the 192 KB bin
    blob = gcs.bucket(BUCKET_NAME).blob(manifest["image"])
    url = blob.generate_signed_url(
        version="v4",
        expiration=datetime.timedelta(minutes=15),
        method="GET",
    )
    return (json.dumps({"claimed": True, "hash": manifest["hash"], "url": url}), 200, {"Content-Type": "application/json"})
```

Wire as HTTP function (`functions_framework.http` or `functions.https.onRequest`
if you move to Node). Give its service account `storage.objectViewer` +
`firestore.user`.

### 4b) Existing pipeline: write per-user

Update `cloud/cloud_function/main.py` `hello_pubsub` to write under
`users/{uid}/daily/{date}.bin` and `users/{uid}/manifest.json` instead of
flat bucket root. Pick `uid` from the image owner's Firestore doc or from the
Pub/Sub trigger payload — don't guess. If you keep the random-photo path for
now, write to a single owner's prefix and document whose.

### 4c) Hosting rewrites (optional polish)

In `website/firebase.json` (create if missing), you can alias:

```json
{
  "hosting": {
    "rewrites": [{ "source": "/api/device/next", "function": "devicePoll" }]
  }
}
```

Then firmware hits `https://everframe-479214.web.app/api/device/next?deviceId=…`
— same origin as the website, one domain in NVS/config.

### 4d) Deploy

```bash
# Functions (Gen2)
gcloud functions deploy devicePoll \
  --gen2 --runtime python311 --region us-central1 \
  --source cloud/cloud_function --entry-point device_poll \
  --trigger-http --allow-unauthenticated \
  --set-env-vars PROJECT_ID=everframe-479214,BUCKET_NAME=everframe-photo-bucket

# Or via Firebase CLI if you add functions/ to website/
firebase deploy --only functions:devicePoll
```

`--allow-unauthenticated` is correct — auth is `deviceId` lookup, not IAM.
Rate-limit in code if needed.

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
- [ ] Clicking "Sign in with Google" opens popup, completes, redirects to `/dashboard`.
- [ ] DevTools → Application → IndexedDB → `firebaseLocalStorageDb` has a user.
- [ ] Refresh `/dashboard` — still authed (onAuthStateChanged), no flicker to login.
- [ ] Sign out (add a button calling `signOut(auth)`), verify gate redirects to `/`.
- [ ] `npm run build` passes; `firebase emulators:start` if you use emulators.

If `firebase-applet-config.json` is missing, `npm run build` fails with
`Cannot find module '../../firebase-applet-config.json'` — create it per §2c.

### 5b) Firestore/Storage rules

In Firebase Console → Firestore → Rules Playground:

- [ ] Unauthed `get /users/uid` → denied.
- [ ] Authed `get /users/{own uid}` → allowed.
- [ ] Authed `create /devices/{randomId} {status:'pending', activationCode:'A7F3K9'}` → allowed via function/service account (client create is not used — device is created by `devicePoll` on first sight or by `/activate` claim).
- [ ] Authed `update /devices/{id}` pending→claimed with `userId == auth.uid` → allowed.
- [ ] Authed `update /devices/{otherUsersId}` → denied.

Storage console: upload a file to `users/{uid}/images/test.png` via the dashboard
upload flow, verify it appears under the correct prefix.

### 5c) `/activate` claim

- [ ] Visit `http://localhost:3000/activate?code=A7F3K9` while signed in → shows pre-filled code.
- [ ] Seed a `devices/{testDeviceId}` doc manually (Console → Firestore → Add document: `activationCode: "A7F3K9", status: "pending"`).
- [ ] Submit claim → doc becomes `status: "claimed", userId: "<your uid>"`.
- [ ] Revisit same code → shows "already claimed" (query returns 0).

### 5d) Cloud endpoint (after deploy)

```bash
# Unclaimed device
curl "https://us-central1-everframe-479214.cloudfunctions.net/devicePoll?deviceId=deadbeefdeadbeefdeadbeefdeadbeef"
# → {"claimed": false}

# Seed claimed device + manifest first
# Firestore: devices/{myDeviceId} { userId: "<uid>", status: "claimed", lastImageHash: "" }
# Storage: users/<uid>/manifest.json {"image":"users/<uid>/daily/2026-08-10.bin","hash":"abc"}
# Storage: users/<uid>/daily/2026-08-10.bin  (any 192 KB file for test)

curl "https://us-central1-everframe-479214.cloudfunctions.net/devicePoll?deviceId=myDeviceId"
# → {"claimed": true, "hash": "abc", "url": "https://storage.googleapis.com/...&Expires=..."}
curl -L -o /tmp/test.bin "<url>"
ls -lh /tmp/test.bin  # should be 192000 bytes for real art
```

### 5e) End-to-end (needs firmware per other doc)

After the ESP32 changes in `esp32-provisioning-redesign.md` land:

- [ ] Hold button 3 s → e-paper shows QR `WIFI:T:nopass;S:EverFrame-XXXX;;`.
- [ ] Phone scans QR, joins AP, portal at `http://192.168.4.1` shows SSID/pass only.
- [ ] Submit home Wi-Fi → STA connects, e-paper flips to "Connected ✓  A7F3K9" + claim QR.
- [ ] Scan claim QR → `/activate?code=A7F3K9` → signed in → device appears under account.
- [ ] Next deep-sleep wake → `GET /devicePoll?deviceId=…` → hash compare → display refresh.

---

## 6) What not to do

- Do not commit `firebase-applet-config.json`, `.env`, or any `apiKey` to git.
  `90a288b` already scrubbed this once.
- Do not put `bucket/token` back into the portal form — it defeats the
  single-endpoint routing.
- Do not issue long-lived Storage signed URLs — 15 min is plenty; device
  re-polls daily anyway.

---

## 7) Rollout order (recommended)

1. Land website Google auth + gate + `firebase-applet-config.json` instructions — deploy to App Hosting, test §5a–5c.
2. Deploy `devicePoll` + update `main.py` per-user write — test §5d with manual Firestore/Storage seeds.
3. Then implement `docs/esp32-provisioning-redesign.md` — wire button, QR, portal trim, activation screen.

When you push the website branch, run `firebase deploy --only hosting,firestore:rules,storage` and paste the Hosting URL here; I'll sanity-check the auth flow.
