#!/usr/bin/env bash
set -euo pipefail

PROJECT_ID="${PROJECT_ID:-everframe-479214}"
REGION="${REGION:-us-central1}"
BUCKET_NAME="${BUCKET_NAME:-everframe-photo-bucket}"
TOPIC_ID="${TOPIC_ID:-scheduler-event}"
FUNCTION_SERVICE_ACCOUNT="${FUNCTION_SERVICE_ACCOUNT:-}"
GENERATE_ART="${GENERATE_ART:-false}"
IMAGE_MODEL="${IMAGE_MODEL:-gemini-3.1-flash-image}"
IMAGE_LOCATION="${IMAGE_LOCATION:-global}"
ALLOWED_ORIGINS="${ALLOWED_ORIGINS:-https://everframe.hoodcharlesd.me,https://everframe-479214.web.app,https://everframe-479214.firebaseapp.com,http://localhost:3000}"
SOURCE_DIR="$(cd "$(dirname "$0")/cloud_function" && pwd)"

common=(--gen2 --runtime python311 --region "$REGION" --project "$PROJECT_ID" --source "$SOURCE_DIR")
if [[ -n "$FUNCTION_SERVICE_ACCOUNT" ]]; then
  common+=(--service-account "$FUNCTION_SERVICE_ACCOUNT")
fi

if ! gcloud pubsub topics describe "$TOPIC_ID" --project "$PROJECT_ID" >/dev/null 2>&1; then
  gcloud pubsub topics create "$TOPIC_ID" --project "$PROJECT_ID"
fi

if [[ -n "$FUNCTION_SERVICE_ACCOUNT" ]]; then
  # verify_id_token(check_revoked=True) performs a Firebase Auth user lookup.
  # Viewer is sufficient; the functions never create, update, or delete users.
  gcloud projects add-iam-policy-binding "$PROJECT_ID" \
    --member "serviceAccount:$FUNCTION_SERVICE_ACCOUNT" \
    --role roles/firebaseauth.viewer --quiet >/dev/null

  # claimDevice and refreshDevice publish targeted refresh requests after a
  # successful link or upload. Keep this grant scoped to the one topic.
  gcloud pubsub topics add-iam-policy-binding "$TOPIC_ID" \
    --project "$PROJECT_ID" \
    --member "serviceAccount:$FUNCTION_SERVICE_ACCOUNT" \
    --role roles/pubsub.publisher --quiet >/dev/null
fi

gcloud functions deploy everframeProcessor "${common[@]}" \
  --entry-point hello_pubsub --trigger-topic "$TOPIC_ID" \
  --memory 1Gi --timeout 540s --retry --max-instances 1 --concurrency 1 \
  --set-env-vars "PROJECT_ID=$PROJECT_ID,BUCKET_NAME=$BUCKET_NAME,REGION=$REGION,TOPIC_ID=$TOPIC_ID,GENERATE_ART=$GENERATE_ART,IMAGE_MODEL=$IMAGE_MODEL,IMAGE_LOCATION=$IMAGE_LOCATION"

# With a custom Eventarc trigger identity, Gen2 does not always add the
# underlying Cloud Run invoker binding. Scope it to the processor service.
if [[ -n "$FUNCTION_SERVICE_ACCOUNT" ]]; then
  gcloud run services add-iam-policy-binding everframeprocessor \
    --region "$REGION" --project "$PROJECT_ID" \
    --member "serviceAccount:$FUNCTION_SERVICE_ACCOUNT" \
    --role roles/run.invoker --quiet >/dev/null
fi

for spec in "registerDevice:register_device" "devicePoll:device_poll" "claimDevice:claim_device" "removeDevice:remove_device" "refreshDevice:refresh_device" "deviceStatus:device_status" "updateDeviceSchedule:update_device_schedule"; do
  function_name="${spec%%:*}"
  entry_point="${spec##*:}"
  gcloud functions deploy "$function_name" "${common[@]}" \
    --entry-point "$entry_point" --trigger-http --allow-unauthenticated \
    --max-instances 2 \
    --set-env-vars "^|^PROJECT_ID=$PROJECT_ID|BUCKET_NAME=$BUCKET_NAME|REGION=$REGION|TOPIC_ID=$TOPIC_ID|ALLOWED_ORIGINS=$ALLOWED_ORIGINS"
done

if ! gcloud scheduler jobs describe everframe-twice-daily \
  --location "$REGION" --project "$PROJECT_ID" >/dev/null 2>&1; then
  gcloud scheduler jobs create pubsub everframe-twice-daily \
    --location "$REGION" --project "$PROJECT_ID" \
    --schedule "0 0,12 * * *" --time-zone "Etc/UTC" \
    --topic "$TOPIC_ID" --message-body '{}'
else
  gcloud scheduler jobs update pubsub everframe-twice-daily \
    --location "$REGION" --project "$PROJECT_ID" \
    --schedule "0 0,12 * * *" --time-zone "Etc/UTC" \
    --topic "$TOPIC_ID" --message-body '{}'
fi

echo "Cloud functions and daily scheduler deployed to $PROJECT_ID ($REGION)."
echo "AI generation enabled: $GENERATE_ART"
echo "AI image model: $IMAGE_MODEL"
echo "AI image location: $IMAGE_LOCATION"
