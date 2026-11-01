# EverFrame cloud + website setup

This guide covers the one-time Firebase/GCP setup and end-to-end verification.
The implementation and artifact contract are documented in
[`cloud/README.md`](../cloud/README.md). The firmware state machine is in
[`esp32-provisioning-redesign.md`](esp32-provisioning-redesign.md).

## 1. One-time console setup

Project: `everframe-479214`; region: `us-central1`; bucket:
`everframe-photo-bucket`.

1. Enable Google in Firebase Authentication. Authorize `localhost`, the App
   Hosting domain, and any custom production domain.
2. Enable Cloud Functions, Cloud Run, Eventarc, Pub/Sub, Cloud Scheduler,
   Firestore, Cloud Storage, Vertex AI, and IAM Service Account Credentials APIs.
3. Give the Cloud Functions runtime service account:
   - Storage Object Admin on the image bucket
   - Cloud Datastore User
   - Vertex AI User
   - Service Account Token Creator on itself (needed for V4 signed URLs)
4. Create Firestore and Storage in the same project if they do not exist.

The HTTP functions are public at the IAM layer by design. Application-level
authentication is enforced as follows:

- `registerDevice`: one-time ID + SHA-256 secret-hash registration
- `devicePoll`: raw device secret over TLS, verified with constant-time compare
- `claimDevice`: Firebase ID token, verified by Firebase Admin
- `removeDevice`: Firebase ID token plus an owned device ID; clears ownership but preserves the hardware credential

## 2. Website local configuration

The Cloudflare build uses the same public Firebase options. Put production
values in the ignored `website/.env.production.local`; for local development,
put them in `website/.env.local`:

```dotenv
NEXT_PUBLIC_FIREBASE_API_KEY=...
NEXT_PUBLIC_FIREBASE_AUTH_DOMAIN=everframe-479214.firebaseapp.com
NEXT_PUBLIC_FIREBASE_PROJECT_ID=everframe-479214
NEXT_PUBLIC_FIREBASE_STORAGE_BUCKET=everframe-photo-bucket
NEXT_PUBLIC_FIREBASE_MESSAGING_SENDER_ID=...
NEXT_PUBLIC_FIREBASE_APP_ID=...
```

If `claimDevice` uses a different URL, also set:

```dotenv
NEXT_PUBLIC_CLAIM_DEVICE_URL=https://us-central1-everframe-479214.cloudfunctions.net/claimDevice
```

Then verify:

```bash
cd website
npm ci
npm run typecheck
npm run build
npm run dev
```

Production uses Vinext's Cloudflare Worker adapter:

```bash
npm run build:vinext
npx cf auth login
npm run deploy:vinext
```

The Worker name and custom domain live in `website/cloudflare.config.ts`, so the
deployment is reproducible rather than configured only in the dashboard.

The gallery queries `/images` with `userId == auth.uid`. The browser can write
only `users/{uid}/images/**`; daily frame artifacts and manifests are read-only
to the owner and writable only through Admin SDK code.

The dashboard also has a per-owner style library at
`users/{uid}/generationStyles/{styleId}`. Each record carries the owner ID,
short label, style clause, and timestamps; rules constrain labels to 60 and
prompts to 160 characters. This is a style selection contract, not permission
to replace the server's image-generation instruction. The style library is
ready for the AI stage but does not enable it by itself.

## 3. Rules deployment

```bash
firebase deploy --config website/firebase.json --only firestore:rules,storage
```

Expected checks:

- unauthenticated reads are denied
- an authenticated `/images` query scoped to the caller succeeds
- another user's image record or Storage prefix is denied
- every client `list /devices` request is denied
- an owner may directly `get` their claimed device document
- clients cannot write `users/{uid}/daily/**` or `manifest.json`
- owners can read and edit only their own `generationStyles` records; a style
  cannot change owner or creation timestamp after it is created

Activation does not require a Firestore client query. `/activate` sends the
signed-in user's ID token and six-character code to `claimDevice`.

## 4. Cloud deployment

Review the defaults at the top of `cloud/deploy.sh`, then run:

```bash
./cloud/deploy.sh
```

This deploys `everframeProcessor`, `registerDevice`, `devicePoll`,
`claimDevice`, `removeDevice`, `refreshDevice`, and `deviceStatus`, creates the
`scheduler-event` topic if needed, and creates or
updates a once-daily UTC Scheduler job.

For a custom site domain, include it in the `claimDevice` function's
`ALLOWED_ORIGINS` environment variable. Values are comma-separated origins with
no trailing slash.

## 5. Verification without hardware

Run the pure unit suite first:

```bash
python3 -m venv .venv-cloud
. .venv-cloud/bin/activate
pip install -r cloud/cloud_function/requirements.txt
PYTHONPATH=cloud/cloud_function python -m unittest discover \
  -s cloud/cloud_function/tests -v
```

Register and poll a throwaway device:

```bash
SECRET=$(openssl rand -hex 32)
HASH=$(printf '%s' "$SECRET" | xxd -r -p | shasum -a 256 | awk '{print $1}')
DEVICE=a3f9e2c1b4d60718293a4b5c6d7e8f90

curl -X POST https://us-central1-everframe-479214.cloudfunctions.net/registerDevice \
  -H 'Content-Type: application/json' \
  -d "{\"deviceId\":\"$DEVICE\",\"tokenHash\":\"$HASH\",\"activationCode\":\"A7F3K9\"}"

curl https://us-central1-everframe-479214.cloudfunctions.net/devicePoll \
  -H "Authorization: Bearer $SECRET" \
  -H "X-Device-Id: $DEVICE"
```

The first poll returns `{"claimed":false}`. Visit `/activate?code=A7F3K9`, sign
in, and link it. Upload at least one gallery image, then trigger a targeted run:

```bash
gcloud pubsub topics publish scheduler-event \
  --project everframe-479214 \
  --message '{"userId":"YOUR_FIREBASE_UID"}'
```

After processing, `devicePoll` returns a 64-character hash and a 15-minute URL.
The URL must download exactly 192,000 bytes. A subsequent poll can send
`X-Current-Image-Hash: <hash>` and should receive `upToDate: true`.

## 6. Current deployment and what remains

The GCP backend, rules, IAM, Pub/Sub topic, once-daily Scheduler job, $10/month
alerting budget, and Cloudflare owner portal were deployed on 2026-10-17. The
signed-download path passed a disposable production smoke test with AI
generation disabled. The website is live at
`https://everframe.hoodcharlesd.me`. See
[`gcp-deployment-2026-10-17.md`](gcp-deployment-2026-10-17.md) for the exact
resource inventory, commands, test evidence, and rollback rulesets.

Work that remains is:

- run the signed-in owner claim/upload browser flow
- finish wiring the compiled ESP32 cloud client into the runtime state machine:
  register, activation/poll UI, verified 192,000-byte download, display commit,
  `last_image_hash`, and deep sleep
- physically verify button hold/reset/wake and bad-credential recovery paths
- run one paid Vertex image-generation smoke test only after the non-AI path
  is stable

Do not commit `.env.local`, a service-account key, or a device secret. Device
secrets belong only in ESP32 NVS; Firestore stores only their SHA-256 hashes.

### Source deletion and derived artifacts

The browser currently deletes only the original gallery object and its
`/images/{id}` document. Daily `.bin`/preview artifacts and the manifest are
deliberately not client-deletable, because device delivery depends on them. The
processor currently does not persist a standalone Gemini image for an original
photo; any model output is transient before conversion. Thus a deleted source
may remain visible on a device only until the next successful render replaces
the current manifest. If persistent generated images are introduced, use a
server-side, retryable cascade keyed by the source image ID/path to remove every
generated child and artifact; do not loosen Storage rules to let the browser
delete `daily/**` directly.
