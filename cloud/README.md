# ☁️ Everframe Cloud Pipeline (GCP)

This directory documents the Google Cloud pipeline that keeps Everframe devices refreshed with newly styled artwork. A scheduled processor selects a random photo, analyzes it with **Gemini**, generates a new artistic version using **Imagen (Vertex AI)**, stores the output, and broadcasts an update via **Pub/Sub**.

---

## 🧩 High-Level Flow

```
📆 Cloud Scheduler (every 12h)
        ↓
⚙️ Cloud Function (Python)
    ├── Pick random image from GCS /photos/
    ├── 🧠 Analyze with Gemini 1.5 Flash
    ├── 🎨 Generate new art with Imagen 3
    ├── Upload processed image to GCS /processed/
    └── Publish to Pub/Sub topic
        ↓
📡 MQTT Bridge / IoT Core
        ↓
📱 ESP32 Everframe downloads + refreshes display
```

---

## 🔷 GCP Component Mapping

| Service | Purpose |
|---------|---------|
| **Cloud Storage** | Store original (`photos/`) and processed (`processed/`) images |
| **Cloud Function** | Run the photo-processing logic (Python) |
| **Vertex AI** | Generative AI: Gemini (Analysis) + Imagen (Creation) |
| **Secret Manager** | Securely manage configuration (optional) |
| **Pub/Sub** | Message bus for device notifications |
| **Cloud Scheduler** | Trigger the job every 12 hours |

---

## 🧱 Detailed Pipeline

### 1️⃣ Storage & Secrets

**Buckets:**
```
everframe-images/
  photos/
  processed/
```

**Permissions:**
Grant the Cloud Function service account:
- `roles/storage.objectUser`
- `roles/aiplatform.user` (for Vertex AI)
- `roles/pubsub.publisher`
- `roles/secretmanager.secretAccessor` (if using secrets)

### 2️⃣ Scheduler

Schedule the processor every 12 hours:

```bash
gcloud scheduler jobs create http everframe-photo-job \
  --schedule="0 */12 * * *" \
  --uri="https://REGION-PROJECT.cloudfunctions.net/everframe-processor" \
  --http-method=POST
```

### 3️⃣ Cloud Function (Python)

See `everframe-cloud.py` for the implementation using:
- `vertexai.generative_models` (Gemini)
- `vertexai.preview.vision_models` (Imagen)
- `google.cloud.pubsub_v1`
- `google.cloud.secretmanager`

**Deploy:**

```bash
gcloud functions deploy everframe-processor \
  --runtime python311 \
  --trigger-http \
  --allow-unauthenticated \
  --set-env-vars BUCKET_NAME=everframe-images,TOPIC_ID=everframe-update,REGION=us-central1
```

### 4️⃣ Device Notification (Pub/Sub → MQTT)

The function publishes to a Pub/Sub topic (`everframe-update`). To reach the ESP32:

1.  **Pub/Sub to MQTT Bridge:** Run a small service (e.g., in Cloud Run) that subscribes to Pub/Sub and forwards messages to your MQTT broker.
2.  **Direct MQTT (Alternative):** If you prefer, modify the script to publish directly to an MQTT broker (see previous versions).

### 5️⃣ ESP32 Device

- Waits for `everframe/update` message.
- Payload: `{ "image_url": "...", "style": "...", "original_desc": "..." }`
- Downloads image and refreshes.

---

## 💰 Monthly Cost (Approx.)

| Component | Cost |
|-----------|------|
| Cloud Storage (1 GB) | ~$0.02 |
| Cloud Function (60 runs) | ~$0.01 |
| Vertex AI (Gemini + Imagen) | ~$1–2 (depending on model usage) |
| Pub/Sub | Free tier usually sufficient |
| Secret Manager | ~$0.06 per active secret |
| **Total** | **~$2–4 / month** |
