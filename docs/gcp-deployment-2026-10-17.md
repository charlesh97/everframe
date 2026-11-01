# EverFrame GCP deployment record — 2026-10-17

This is the reproducible record of the first production backend deployment.
It describes both what changed in Google Cloud and why each resource exists.
No service-account key, device secret, signed URL, or Wi-Fi credential is
stored here.

## Result

Project `everframe-479214` (project number `85343234674`) now has a deployed,
smoke-tested device backend and a Cloudflare-hosted owner portal. Billing is
enabled, AI image generation is off, and a monthly alerting budget is in place.

| Resource | Live value |
|---|---|
| Region | `us-central1` |
| Firestore | Native mode, database `(default)`, location `nam5` |
| Image bucket | `everframe-photo-bucket`, location `US-EAST1` |
| Runtime identity | `everframe-functions@everframe-479214.iam.gserviceaccount.com` |
| Pub/Sub topic | `scheduler-event` |
| Scheduler | `everframe-twice-daily`, `0 0,12 * * *`, UTC; processor filters each device preset |
| AI generation | `GENERATE_ART=false` |
| Processor revision | `everframeprocessor-00006-nik` (measured palette, tone compression, serpentine Atkinson, atomic manifest) |
| Budget | USD $10/month alerts at 50%, 90%, and 100% (alert only; not a cap) |
| Owner portal | `https://everframe.hoodcharlesd.me` |
| Cloudflare Worker | `everframe`, version `b83305bd-61f1-4e7b-8d20-3764ed9fc5d5` |

The four HTTP compatibility URLs are:

```text
https://us-central1-everframe-479214.cloudfunctions.net/registerDevice
https://us-central1-everframe-479214.cloudfunctions.net/devicePoll
https://us-central1-everframe-479214.cloudfunctions.net/claimDevice
https://us-central1-everframe-479214.cloudfunctions.net/refreshDevice
```

The fourth function, `everframeProcessor`, is private and invoked through
Eventarc from Pub/Sub.

## How the deployed flow works

```text
ESP32 --POST registerDevice--> Firestore devices/{deviceId} (pending)
owner --Firebase Google ID token--> claimDevice --> device becomes claimed
owner upload --Firebase Google ID token--> refreshDevice --> targeted Pub/Sub event
ESP32 --Bearer device secret--> devicePoll
                               ├─ pending / no image / up to date
                               └─ 15-minute signed Cloud Storage URL

Cloud Scheduler (twice daily) --> Pub/Sub scheduler-event
  --> Eventarc --> everframeProcessor
  --> tone-compress + serpentine-Atkinson dither the 480x800 portrait source
      against the measured Spectra 6 ink palette
  --> rotate palette indexes to native 800x480 six-color frame
  --> upload frame first --> publish manifest last
```

The HTTP functions are public at the Cloud Run IAM/network layer because the
ESP has no Google identity. Authentication remains application-level:

- `registerDevice` accepts a device ID plus `SHA256(secret)` and detects an
  existing-ID/different-secret conflict.
- `devicePoll` verifies the raw device secret with a constant-time hash compare.
- `claimDevice` verifies a Firebase ID token and permits configured web origins.

## Existing state discovered before deployment

The project and billing account were already selected and active. The required
Cloud Functions, Cloud Run, Cloud Build, Artifact Registry, Eventarc, Pub/Sub,
Scheduler, Firestore, Storage, Vertex AI, IAM Credentials, Firebase, and
Identity Toolkit APIs were already enabled. Firestore and the real image bucket
also already existed.

The actual application bucket is `everframe-photo-bucket`. It is **not**
`everframe-photo-bucket.appspot.com`. Website and backend configuration must use
the exact real name.

The Firebase project already contained a web app named `everframe`. Its
generated SDK configuration points at the default Firebase bucket
`everframe-479214.firebasestorage.app`; EverFrame must override `storageBucket`
with `everframe-photo-bucket`.

## Runtime identity and IAM

A dedicated service account was created rather than running application code as
the broad default Compute or App Engine identities.

Project-level grants:

```text
roles/datastore.user
roles/aiplatform.user
roles/eventarc.eventReceiver
roles/firebaseauth.viewer
```

Resource-scoped grants:

```text
everframe-photo-bucket:
  roles/storage.objectAdmin

scheduler-event topic:
  roles/pubsub.publisher

everframe-functions service account on itself:
  roles/iam.serviceAccountTokenCreator

Cloud Run service everframeprocessor:
  roles/run.invoker
```

`Service Account Token Creator` lets the runtime call IAM `signBlob` for
short-lived V4 URLs without a downloaded key. The processor-only Run Invoker
binding lets Eventarc authenticate to the Gen2 function without making the
processor public.

No service-account JSON key was created.

## Deployment commands

The repeatable deployment is:

```bash
cd /Users/charleslab/git/everframe

FUNCTION_SERVICE_ACCOUNT="everframe-functions@everframe-479214.iam.gserviceaccount.com" \
GENERATE_ART=false \
IMAGE_MODEL=gemini-3.1-flash-image \
IMAGE_LOCATION=global \
ALLOWED_ORIGINS="https://everframe.hoodcharlesd.me,https://everframe-479214.web.app,https://everframe-479214.firebaseapp.com,http://localhost:3000" \
./cloud/deploy.sh
```

`cloud/deploy.sh` now defaults AI generation to false, caps the processor at one
instance and each HTTP service at two instances, creates/updates the schedule,
and ensures the scoped Eventarc-to-Cloud-Run invoker grant. When AI is enabled,
it configures `gemini-3.1-flash-image` on Vertex AI's global endpoint explicitly;
the processor asks Gemini for its closest supported portrait ratio (2:3), then
center-fits and Lanczos-downsamples it to EverFrame's 480×800 / 3:5 raster before
six-color conversion. The currently deployed revision remains non-AI until a
deliberate `GENERATE_ART=true` redeployment.

The processor uses 1 GiB memory, a 540-second timeout, retry delivery, and no
warm minimum instances. HTTP functions use 256 MiB, a 60-second timeout, and no
warm minimum instances.

## Firebase Rules deployment

The machine had a working Google Cloud login but no Firebase CLI login. Rules
were therefore compiled and released through the authenticated Firebase Rules
REST API. This is equivalent to a CLI rules deployment and preserves immutable
rulesets for rollback.

Current releases:

```text
cloud.firestore
  projects/everframe-479214/rulesets/a652e2ec-ad31-46cd-ac36-6cf546eeb1d0

firebase.storage/everframe-photo-bucket
  projects/everframe-479214/rulesets/56226dbf-3576-4682-b067-0131e8cd6026
```

Previous releases retained for rollback:

```text
cloud.firestore
  projects/everframe-479214/rulesets/d766ce6e-68ca-4f1e-b923-893dfc5830cc

firebase.storage/everframe-photo-bucket
  projects/everframe-479214/rulesets/50ba69be-69a5-494b-ac9d-f6c3c4ccc6de
```

The unrelated default bucket release
`firebase.storage/everframe-479214.firebasestorage.app` was intentionally left
unchanged.

For normal future deployments, after `firebase login`, use:

```bash
npx firebase-tools deploy --config website/firebase.json \
  --only firestore:rules,storage --project everframe-479214
```

Confirm that the CLI targets `everframe-photo-bucket`; otherwise deploy the
custom bucket release through the Rules API as done in this deployment.

## Firebase Authentication

The authorized-domain list now contains:

```text
localhost
everframe-479214.firebaseapp.com
everframe-479214.web.app
everframe.hoodcharlesd.me
```

Google sign-in was enabled in Firebase Console after the backend deployment.
The Admin API verification confirmed that the `google.com` provider is enabled
and has both its OAuth client ID and client secret configured. The secret remains
managed by Firebase and was not copied into the repository.

No OAuth client secret should be copied into this repository.

## Cloudflare website deployment

The Next.js owner portal is deployed as a Cloudflare Worker through Vinext. The
production request path is:

```text
browser -> everframe.hoodcharlesd.me -> Cloudflare Worker everframe
        -> Vinext/Next App Router
        -> Firebase client SDK (Google Auth, Firestore, Storage)
        -> claimDevice HTTPS function for activation
```

After sign-in, the dashboard header stays inside the authenticated application:
`Add Photos` opens the import flow, `My Device` opens the saved-photo gallery,
and the Everframe wordmark also opens the gallery. Only `Sign Out` calls
Firebase Auth `signOut()` and replaces the current route with `/`, the login
page. This prevents ordinary dashboard navigation from appearing to log the
owner out.

Photo import follows a separate deterministic path:

```text
iPhone/Android JPEG, PNG, HEIC, or HEIF (75 MB maximum)
  -> browser validates and decodes (HEIC conversion stays client-side)
  -> full image is proportionally downsampled to a <=4096px working preview
  -> owner adjusts a 480:800 / 3:5 portrait crop for every selected image
  -> browser encodes a 960x1600 JPEG source master
  -> Firebase Storage users/{uid}/images/*.jpg + Firestore images/{id}
  -> processor tone-compresses into the measured panel range and applies
     serpentine Atkinson dithering in portrait raster order
  -> processor rotates the finished palette indexes clockwise into the exact
     800x480 controller payload
```

The first authenticated production upload was verified on 2026-10-17: the
portal stored a 960x1600 RGB JPEG under the owner's
`users/{uid}/images/` prefix and wrote the matching owner-scoped Firestore
record with `crop.aspect = 3:5`. A targeted live Pub/Sub run then created an
800×480 six-color artifact of exactly 192,000 bytes plus a manifest and preview.
The preview is built by decoding the exact packed frame instead of showing the
pre-quantization source. On 2026-10-18, processor revision
`everframeprocessor-00006-nik` was deployed after option 4 won the physical
panel comparison. A targeted production refresh generated a 192,000-byte frame
with SHA-256
`776e37d0078282aebfa24588cf3eb5271662cc77f2882de3d97a6d07ff68b46f`;
the downloaded object matched that manifest hash exactly, used only controller
codes `0, 1, 2, 3, 5, 6`, and produced a 480x800 JPEG preview. The processor
also retained the real Pub/Sub message ID for retry idempotence.

The physical ESP32 end-to-end delivery test was completed after activation. The
device authenticated its poll, downloaded the 192,000-byte signed artifact,
verified its SHA-256 hash, completed the e-paper refresh, stored the current
hash, and entered its configured 43,200-second deep sleep. The test exposed and
fixed ESP-IDF's too-small default request buffer for long V4 signed URLs; the
image downloader now uses a 4 KiB transmit buffer.

Import deliberately does not invoke Gemini. AI generation is an optional
processor stage controlled by `GENERATE_ART`; with the deployed value `false`,
the selected crop goes directly to deterministic e-paper conversion.

The checked-in deployment files are:

```text
website/vite.config.ts        Vinext + Cloudflare Vite plugins
website/cloudflare.config.ts  Worker name, runtime flags, assets, custom domain
website/package.json          Vinext build/deploy scripts and React 19 runtime
```

Public Firebase build variables and the claim endpoint were supplied through
the ignored `website/.env.production.local`. They are compiled into the browser
bundle; no Firebase OAuth client secret, service-account key, or device secret
is present there.

Cloudflare was authenticated with its OAuth device flow. The repeatable build
and deployment commands are:

```bash
cd website
npm ci
npm run typecheck
npm run build
npm run build:vinext
npx cf auth login
npm run deploy:vinext
```

`cloudflare.config.ts` declares `domains: ["everframe.hoodcharlesd.me"]`, so
the Worker deployment also owns the custom-domain trigger. The previous
`output: "standalone"` setting was removed from `next.config.ts`: standalone is
a Node/Docker output and conflicts with Cloudflare's Worker Build Output.

Vinext's RSC scanner also required explicit Firebase barrel exports instead of
`export *` across TSX client modules. This is a build-tool compatibility change;
the application's Firebase API is unchanged.

The initial dependency audit found a critical advisory in Next.js 15.3.8. The
site was upgraded to Next.js 15.5.28, rebuilt, and redeployed before handoff.
The production audit now reports zero critical findings; unresolved transitive
findings remain in Firebase/Firestore, Genkit/OpenTelemetry, Vinext build tools,
and Tailwind's tooling graph where npm offers no clean compatible fix. Do not
use `npm audit fix --force`: its proposed Vinext and `patch-package` downgrades
would replace the selected runtime with older incompatible releases.

## Verification performed

Local verification before deployment:

```text
cloud unit tests: 8 passed
cloud/deploy.sh syntax: passed
git diff --check: passed
```

Live endpoint probes:

```text
registerDevice malformed GET: 400
devicePoll without Bearer secret: 401
claimDevice with wrong method: 405
```

`cloud/smoke_test.sh` then exercised a disposable end-to-end backend path:

```text
registration:                 200
pending authenticated poll:  200, claimed=false
wrong secret:                401
same ID/different hash:      409
claimed device/image poll:   200 with signed URL
signed download:             exactly 192,000 bytes, SHA-256 matched
current-hash poll:           200, upToDate=true
Cloudflare CORS preflight:   204 with the expected origin
```

The script removes its Firestore document and Storage objects on exit. Cleanup
was independently verified with Firestore 404 responses and empty test prefixes.

A targeted Pub/Sub message initially revealed that the custom Eventarc identity
lacked `run.routes.invoke`. The processor-scoped `roles/run.invoker` binding was
added and the delivery retried successfully with HTTP 200.

The first signed-image smoke test also revealed that Firestore
`DocumentSnapshot.get("lastImageHash")` raises when a newly registered device has
no optional hash. `device_auth.py` now reads that optional value from
`to_dict()`, `devicePoll` was redeployed, and the full smoke test passed.

Repeat the disposable test at any time with:

```bash
./cloud/smoke_test.sh
```

Website verification after Cloudflare deployment:

```text
https://everframe.hoodcharlesd.me/                 200, HTML, Cloudflare edge
/activate?code=ABC123                              200, hydrated with code shown
/dashboard                                         200, HTML
/everframe/everframe-logo.svg                      200, image/svg+xml
DNS                                                Cloudflare IPv4 + IPv6
TLS                                                valid HTTPS
```

The rendered activation page showed the Google sign-in action and the supplied
activation code. An account was deliberately not selected during deployment
verification, so no test application session or durable owner data was created.

The original animated marketing homepage was replaced with a deliberately
minimal sign-in page after initial review: logo, product name, one sentence,
and one Google sign-in button. Activation and dashboard routes remain intact.

Hardware verification later the same day:

```text
firmware build + flash:              passed
saved device identity and Wi-Fi:     preserved
new frame download and refresh:      passed
EXT0 short-button wake:              passed
authenticated poll after short wake: HTTP 200
unchanged-frame path:                skipped refresh and returned to deep sleep
```

The short-button test generated a distinct `devicePoll` request from the ESP32
at 2026-10-18 01:41:20 UTC (2026-10-17 18:41:20 PDT). The firmware no longer
treats every button wake as a provisioning request: a released/short press uses
saved Wi-Fi and checks the current hash, while a continuous 3-second hold enters
provisioning.

Frame integrity was then verified at both sides of the firmware's display
boundary during a real refresh. The HTTPS downloader hashed the complete buffer
after receiving exactly 192,000 bytes, and the SPI layer independently hashed
each successfully completed DMA transaction. Both produced:

```text
db354633e8389af3980ef2f10bb6ede611a0a31abbf9c814489cb6f74ee58741
```

The display refresh completed, the hash was committed to NVS, and the frame
returned to deep sleep. This proves the cloud artifact, PSRAM framebuffer, and
bytes submitted to the ESP-IDF SPI driver are identical. The panel has no
framebuffer readback, so verifying electrical MOSI signaling beyond the SPI
driver would require an external logic analyzer.

Schedule rollout and physical verification on 2026-11-01:

```text
Cloud Scheduler:                        00:00 and 12:00 UTC, enabled
Manual only -> ESP acknowledgment:      passed
12 hours -> ESP acknowledgment:         passed
Daily -> ESP acknowledgment:            passed
Weekly -> ESP acknowledgment:           passed
Cloud desired/applied version match:    passed
Unchanged image during schedule edits:  no panel refresh
Weekly computed sleep:                  593,215 seconds
Weekly target:                          next Sunday at 12:15 UTC
```

The button wake kept the frame online for five minutes and polled every 15
seconds. Each preset was applied in the dashboard order without shifting the
absolute cadence. On expiration the ESP used fresh cloud time to calculate the
remaining weekly interval, then entered deep sleep. The cloud-generated art
dispatcher runs 15 minutes before the corresponding frame wake target.

## Remaining work

1. Run a real owner sign-in → claim → upload → processor → device-poll test.
2. Wire the deployed register/poll URLs into the ESP32 runtime owner.
3. Physically verify the 3-second provisioning hold, 10-second factory reset,
   and bad-Wi-Fi recovery paths. Short-button EXT0 wake is verified.
4. Track upstream fixes for the remaining npm audit findings without using
   forced breaking upgrades; the critical Next.js finding is already fixed.
5. Only after the non-AI path is stable, redeploy with `GENERATE_ART=true` and
   run one controlled Vertex image-generation test.
