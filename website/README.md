# EverFrame owner portal

The owner portal is a Next.js App Router application. It uses Firebase Google
Authentication, owner-scoped Firestore/Storage access, and authenticated
device claim, status, refresh, and removal Cloud Functions. Production runs on
Cloudflare Workers through Vinext at
`https://everframe.hoodcharlesd.me`.

## Photo import flow

The dashboard accepts JPEG, PNG, HEIC, and HEIF files up to 75 MB. It does not
call an AI model during import. HEIC/HEIF photos are decoded in the browser,
then every selected photo is presented in a crop editor before any upload.

The editor uses the frame's physical 480:800 (3:5) portrait aspect ratio.
Before cropping, the browser proportionally downsamples the complete decoded
photo to at most 4096 pixels on its longest edge. This is a full-image working
preview, not a crop, so even a 50 MP source retains its complete composition.
After the owner chooses the crop, the browser saves it as a 960×1600 JPEG under
`users/{uid}/images/` and calls the authenticated `refreshDevice` function for
the last uploaded image. The cloud processor downsamples and dithers that
already-framed source, then rotates it clockwise into the panel
controller's native 800×480 six-color, 4-bpp payload. Keeping those two stages
separate gives the user final control over composition while leaving
hardware-specific encoding in the backend.

## Device management

The dashboard lists every frame owned by the signed-in account. **Add another
frame** opens the activation flow. **Remove** confirms the action, calls
`removeDevice`, and returns that cloud device to `pending` without deleting its
secret or the owner's photos. On its next authenticated poll, the frame sees
`claimed: false`, shows its existing activation code, and can be paired again.
A physical factory reset creates a new identity, so its old offline cloud entry
must still be removed from the dashboard.

The ESP does not store the gallery or receive originals. It pulls only the
current 192 KB panel artifact from a short-lived signed URL. The cloud gallery
has no application-level photo-count limit yet; Storage quota/cost is the
effective limit.

## AI style library and deletion lifecycle

The dashboard's **Art Styles** panel stores private, owner-editable style
clauses as individual Firestore documents at
`users/{uid}/generationStyles/{styleId}`. A style has a display `label` and a
short `prompt` (maximum 160 characters); the model's base instruction, source
photo, and frame composition remain controlled by the processor. The UI
pre-populates 100 editable EverFrame styles and allows up to 200 total. Every
entry can be edited or deleted, and **Clear all** requires confirmation. Rules
validate every document's owner, shape, and field limits. The processor
consumes this library only when its AI stage is enabled;
with `GENERATE_ART=false`, saved styles do not change live device rendering.
When AI generation is enabled, a targeted job's explicit style wins. Otherwise,
the processor selects from the owner's saved library. An intentionally cleared
library disables AI restyling until a style is added or the EverFrame set is
restored.

## Frame update schedule

Each device card offers **Manual only**, **12 hours**, **Daily**, and **Weekly**
in that order. A save is marked pending until the authenticated frame reports
the same schedule version. Pressing the frame button applies a pending change
immediately; otherwise it applies at the next existing timer wake. The UI shows
the next absolute check after the version is applied.

Today the processor does **not** persist a Gemini output as an image child of a
gallery record. It makes an ephemeral model output, then writes only a rendered
daily panel `.bin`, its preview `.jpg`, and the current manifest under
`users/{uid}/daily/`. Deleting a gallery photo therefore deletes its source
object and `/images/{id}` record, but it cannot delete a historical daily panel
artifact from the browser—those paths are intentionally Admin-SDK-only. The
current screen remains available until the next successful processor run
publishes a replacement. Before enabling persistent AI generations, add an
Admin-SDK source-deletion workflow that removes generated records and all
source-keyed artifacts atomically (or queues a retryable cleanup job).

## Local development

Create an ignored `.env.local` with the `NEXT_PUBLIC_FIREBASE_*` values listed
in [`docs/cloud-auth-setup.md`](../docs/cloud-auth-setup.md), then run:

```bash
npm ci
npm run typecheck
npm run dev
```

## Production build and deployment

Production reads the same public values from the ignored
`.env.production.local` file.

```bash
npm run build
npm run build:vinext
npx cf auth login
npm run deploy:vinext
```

`cloudflare.config.ts` is the source of truth for the Worker name and custom
domain. `vite.config.ts` configures Vinext and the Cloudflare runtime. Do not
commit service-account keys, OAuth client secrets, or device secrets.

The detailed deployment record and verification evidence are in
[`docs/gcp-deployment-2026-10-17.md`](../docs/gcp-deployment-2026-10-17.md).
