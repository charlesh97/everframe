# EverFrame cloud pipeline

The production path is owner-scoped and pull-based:

```text
Cloud Scheduler -> Pub/Sub -> everframeProcessor
  -> claimed users -> enabled /images documents -> GCS source image
  -> optional Gemini 3.1 Flash Image image-to-image generation
  -> fit + serpentine Atkinson dither in 480x800 portrait raster order
     using measured 7.3-inch Spectra 6 ink colors and tone compression
  -> rotate palette indexes to native 800x480 -> 192,000-byte 4-bpp frame
  -> users/{uid}/daily/{date}-{hash}.bin
  -> users/{uid}/manifest.json (published last)

ESP32 -> devicePoll + Bearer secret -> 15-minute signed frame URL

Dashboard upload/claim -> authenticated refreshDevice/claimDevice -> Pub/Sub
Dashboard remove -> authenticated removeDevice -> device returns to pending
```

There is no MQTT bridge. Devices poll after wake, compare the returned SHA-256
with their locally persisted hash, and download only when it changed.

## Functions

| Deployment | Entry point | Trigger | Purpose |
|---|---|---|---|
| `everframeProcessor` | `hello_pubsub` | Pub/Sub `scheduler-event` | Build and publish frames |
| `registerDevice` | `register_device` | HTTP | Register device ID + secret hash |
| `devicePoll` | `device_poll` | HTTP | Authenticate a frame and return its current artifact |
| `claimDevice` | `claim_device` | HTTP | Verify Firebase ID token and atomically claim an activation code |
| `removeDevice` | `remove_device` | HTTP | Verify ownership and return a frame to pending activation |
| `refreshDevice` | `refresh_device` | HTTP | Verify Firebase ID token and queue a targeted uploaded image |
| `deviceStatus` | `device_status` | HTTP | Return owner-safe device presence and refresh progress |

All entry points are exported from `cloud_function/main.py`, as required by the
Python Cloud Functions runtime. `device_auth.py` contains the HTTP implementation
and `epaper.py` contains the pure panel conversion.

## Processor messages

The scheduled message body is `{}` and processes each distinct owner of a
claimed device. A targeted run can publish:

```json
{
  "userId": "firebase-uid",
  "imagePath": "users/firebase-uid/images/photo.png",
  "style": "watercolor painting"
}
```

`imagePath` and `style` are optional. Without an image path the processor picks
a random `/images` document belonging to that user whose `isDisplayed` field is
not false. Pub/Sub event IDs are written to manifests; retries skip users already
completed for that event.

Dashboard-targeted runs also carry a server-generated `requestId`. That ID is
recorded before Pub/Sub publication. The processor runs at concurrency one and
checks that the ID is still current before work and before manifest publication;
`devicePoll` also refuses a manifest whose `requestId` has been replaced. A
second send therefore supersedes the first deterministically. The older job may
consume generation work if it was already running, but it cannot become
deliverable or overwrite the newer dashboard state.

Set `GENERATE_ART=false` to bypass Gemini and only prepare existing gallery art
for the panel. This is useful for local/integration tests and is also a safe
operational fallback if the image model is unavailable. The runtime also defaults
to false, so an incomplete deployment cannot accidentally make paid requests.

When enabled, `IMAGE_MODEL` defaults to `gemini-3.1-flash-image` (Gemini 3.1
Flash Image) on Vertex AI's `global` endpoint. Gemini 3.1 Flash-Lite is a
multimodal model, but `gemini-3.1-flash-lite-image` is not a callable Vertex
image-generation model ID. The image model's nearest portrait ratio is 2:3,
so the request keeps the subject inside a narrower 3:5 safe area and the panel
encoder performs the final deterministic crop and downsample described below.
Override the model or endpoint only deliberately, for example:

```bash
GENERATE_ART=true IMAGE_MODEL=gemini-3.1-flash-image IMAGE_LOCATION=global ./cloud/deploy.sh
```

### Per-owner styles

The processor first honors an explicit targeted-message `style` override. For a
normal scheduled run it reads at most 200 valid documents from
`users/{uid}/generationStyles`. A new owner's library starts with 100 editable
EverFrame styles. A document belongs to that same `uid` and has a `prompt`
style clause (one to 160 characters), such as `watercolor painting`. Empty,
invalid, mismatched-owner, or explicitly disabled records are ignored.
After the library is initialized, clearing it intentionally disables AI
restyling; the source photo is prepared for the frame without a hidden fallback.
The processor supplies the fixed subject-preservation and panel-composition
instruction itself, so a dashboard prompt is never executed as a free-form
full model request.

### Device schedules

Each claimed device has one owner-controlled preset: `manual`, `12h`, `daily`,
or `weekly`. `updateDeviceSchedule` records a versioned desired value. Every
successful authenticated `devicePoll` returns Unix `serverTime`, an absolute
`nextWakeAt` (or `null` for manual), and the version. The firmware sets its
online clock, persists the setting, reports the applied version, and computes a
fresh remaining deep-sleep duration so RTC drift does not accumulate. The
twice-daily dispatcher runs at 00:00 and 12:00 UTC and generates only for due
owners; device wake targets are 15 minutes later. Weekly runs use Sunday.

## Artifact contract

`manifest.json` is replaced only after the frame upload succeeds:

```json
{
  "version": 1,
  "image": "users/uid/daily/2026-10-17-012345abcdef.bin",
  "hash": "64-character SHA-256",
  "bytes": 192000,
  "width": 800,
  "height": 480,
  "format": "everframe-epd7in3e-4bpp"
}
```

Each byte holds the left pixel in the high nibble and right pixel in the low
nibble. Controller codes match `embedded/everframe-esp32/main/EPD_7in3e.h`:
black `0`, white `1`, yellow `2`, red `3`, blue `5`, green `6`.
The JPEG referenced by `preview` is decoded from this packed frame using the
measured ink colors, so it approximates the physical panel and shows the same
crop, rotation, six-color quantization, and dither sent to the panel.

### Crop and downsample contract

Every upload is already an owner-selected 3:5 portrait source. For an AI run,
Gemini returns a 2:3 1K portrait because that is the closest ratio the selected
model supports. `epaper.image_to_frame` applies EXIF orientation, converts to
RGB, then uses Pillow `ImageOps.fit(..., (480, 800), LANCZOS)`: it proportionally
downsamples (or upscales) and center-crops only the excess width to exactly 3:5.
For a 2:3 generated image that removes about 10% of its width overall; the
prompt's safe-area wording keeps the important subject intact. The 480×800
result is tone-compressed, serpentine-Atkinson dithered into six inks, rotated,
and packed. Thus the panel frame is always 192,000 bytes regardless of source or
model output resolution.

### Failure behavior

The processor never replaces a user's manifest until the packed frame upload
succeeds. A Gemini request that errors or returns no image, a corrupt source,
frame-encoding failure, frame upload failure, or manifest upload failure causes
the Pub/Sub invocation to fail and be retried; the prior manifest remains live.
A preview upload is non-critical: it is logged and the new manifest omits its
preview URL, while the verified frame remains deliverable. A failed manifest may
leave an unreachable frame object; a retry safely overwrites that deterministic
object path and publishes it only after the manifest succeeds.

## Deploy

The first production deployment was completed and smoke tested on 2026-10-17.
See [`docs/gcp-deployment-2026-10-17.md`](../docs/gcp-deployment-2026-10-17.md)
for the live inventory, IAM bindings, ruleset rollback IDs, and test evidence.

Prerequisites:

- Application Default Credentials for local tests.
- Cloud Functions runtime service account with Storage Object Admin, Cloud
  Datastore User, Vertex AI User, Firebase Authentication Viewer, Pub/Sub
  Publisher on `scheduler-event`, and Service Account Token Creator. Firebase
  Authentication Viewer supports revoked-token checks without granting user
  mutation access. Service Account Token Creator is required for V4 URL signing
  when the runtime uses metadata credentials.
- Cloud Scheduler, Pub/Sub, Vertex AI, IAM Credentials, Firestore, and Cloud
  Functions APIs enabled.
- Google sign-in enabled in Firebase Authentication.

Then:

```bash
chmod +x cloud/deploy.sh
./cloud/deploy.sh
firebase deploy --config website/firebase.json --only firestore:rules,storage
```

If a custom website domain calls `claimDevice`, `removeDevice`, `refreshDevice`,
or `deviceStatus`, add it to
`ALLOWED_ORIGINS` (comma-separated, no trailing slash). Set the corresponding
`NEXT_PUBLIC_*_URL` website variable only if an endpoint differs from production.

## Verify

```bash
python3 -m venv .venv-cloud
. .venv-cloud/bin/activate
pip install -r cloud/cloud_function/requirements.txt
PYTHONPATH=cloud/cloud_function python -m unittest discover \
  -s cloud/cloud_function/tests -v
```

To run a targeted production smoke test without waiting for Scheduler:

```bash
gcloud pubsub topics publish scheduler-event \
  --project everframe-479214 \
  --message '{"userId":"FIREBASE_UID"}'
```

Inspect the function logs, then verify that the user's `manifest.json` and its
referenced `.bin` both exist and that the frame object is exactly 192,000 bytes.
