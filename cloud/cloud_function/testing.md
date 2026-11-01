# Local cloud testing

From the repository root:

```bash
python3 -m venv .venv-cloud
. .venv-cloud/bin/activate
pip install -r cloud/cloud_function/requirements.txt
PYTHONPATH=cloud/cloud_function python -m unittest discover \
  -s cloud/cloud_function/tests -v
```

The unit tests do not call GCP. They verify the exact 192,000-byte frame layout,
all six controller color codes, deterministic crop/downsample behavior, invalid
image handling, Pub/Sub parsing, Lite-model request configuration, per-owner
style selection, and publication ordering/failure behavior.

For a local processor integration run, authenticate Application Default
Credentials and disable paid image generation first:

```bash
gcloud auth application-default login
export PROJECT_ID=everframe-479214
export BUCKET_NAME=everframe-photo-bucket
export GENERATE_ART=false
cd cloud/cloud_function
functions-framework --target hello_pubsub --signature-type cloudevent
```

In another terminal, send a CloudEvent containing a known Firebase user ID:

```bash
curl -X POST http://localhost:8080 \
  -H 'Content-Type: application/cloudevents+json' \
  -H 'ce-id: local-test-1' \
  -H 'ce-source: local' \
  -H 'ce-specversion: 1.0' \
  -H 'ce-type: google.cloud.pubsub.topic.v1.messagePublished' \
  -d '{"message":{"data":"eyJ1c2VySWQiOiJGSFJFQkFTRV9VSUQifQ=="}}'
```

Replace the encoded example with:

```bash
printf '%s' '{"userId":"YOUR_UID"}' | base64
```

The user needs at least one Firestore `/images` record with a valid
`users/{uid}/images/...` `storagePath`. The processor uploads the frame and
preview before replacing `users/{uid}/manifest.json`.

To exercise paid image generation in an isolated development project, opt in
explicitly; production defaults remain off:

```bash
export GENERATE_ART=true
export IMAGE_MODEL=gemini-3.1-flash-image
export IMAGE_LOCATION=global
```
