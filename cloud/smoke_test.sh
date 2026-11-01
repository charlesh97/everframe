#!/usr/bin/env bash
set -euo pipefail

# Disposable production smoke test for the deployed device delivery path.
# It never prints the generated device secret or signed URL and removes the
# temporary Firestore document and Cloud Storage objects on exit.

PROJECT_ID="${PROJECT_ID:-everframe-479214}"
REGION="${REGION:-us-central1}"
BUCKET_NAME="${BUCKET_NAME:-everframe-photo-bucket}"
REGISTER_URL="${REGISTER_URL:-https://$REGION-$PROJECT_ID.cloudfunctions.net/registerDevice}"
POLL_URL="${POLL_URL:-https://$REGION-$PROJECT_ID.cloudfunctions.net/devicePoll}"
CLAIM_URL="${CLAIM_URL:-https://$REGION-$PROJECT_ID.cloudfunctions.net/claimDevice}"
WEB_ORIGIN="${WEB_ORIGIN:-https://everframe.hoodcharlesd.me}"

test_dir="$(mktemp -d)"
device_id="$(openssl rand -hex 16)"
secret="$(openssl rand -hex 32)"
wrong_secret="$(openssl rand -hex 32)"
token_hash="$(printf '%s' "$secret" | xxd -r -p | shasum -a 256 | awk '{print $1}')"
conflict_hash="$(printf '%s' "$wrong_secret" | xxd -r -p | shasum -a 256 | awk '{print $1}')"
activation_code="$(printf '%s' "$device_id" | cut -c1-6 | tr '[:lower:]' '[:upper:]')"
user_id="everframe-cloud-smoke"
frame_path="users/$user_id/daily/smoke-$token_hash.bin"
manifest_path="users/$user_id/manifest.json"
document_url="https://firestore.googleapis.com/v1/projects/$PROJECT_ID/databases/(default)/documents/devices/$device_id"

cleanup() {
  gcloud storage rm "gs://$BUCKET_NAME/$frame_path" \
    "gs://$BUCKET_NAME/$manifest_path" --quiet >/dev/null 2>&1 || true
  local cleanup_token
  cleanup_token="$(gcloud auth print-access-token 2>/dev/null || true)"
  if [[ -n "$cleanup_token" ]]; then
    curl -sS -X DELETE -H "Authorization: Bearer $cleanup_token" \
      -H "x-goog-user-project: $PROJECT_ID" "$document_url" >/dev/null || true
  fi
  find "$test_dir" -type f -delete
  rmdir "$test_dir"
}
trap cleanup EXIT

request() {
  local output="$1"
  shift
  curl -sS -o "$output" -w '%{http_code}' "$@"
}

register_code="$(request "$test_dir/register.json" -X POST "$REGISTER_URL" \
  -H 'Content-Type: application/json' \
  -d "{\"deviceId\":\"$device_id\",\"tokenHash\":\"$token_hash\",\"activationCode\":\"$activation_code\"}")"
echo "register: HTTP $register_code $(jq -c . "$test_dir/register.json")"
[[ "$register_code" == 200 ]]

pending_code="$(request "$test_dir/pending.json" "$POLL_URL" \
  -H "Authorization: Bearer $secret" -H "X-Device-Id: $device_id")"
echo "pending poll: HTTP $pending_code $(jq -c . "$test_dir/pending.json")"
[[ "$pending_code" == 200 && "$(jq -r .claimed "$test_dir/pending.json")" == false ]]

wrong_code="$(request "$test_dir/wrong.json" "$POLL_URL" \
  -H "Authorization: Bearer $wrong_secret" -H "X-Device-Id: $device_id")"
echo "wrong-secret poll: HTTP $wrong_code $(jq -c . "$test_dir/wrong.json")"
[[ "$wrong_code" == 401 ]]

conflict_code="$(request "$test_dir/conflict.json" -X POST "$REGISTER_URL" \
  -H 'Content-Type: application/json' \
  -d "{\"deviceId\":\"$device_id\",\"tokenHash\":\"$conflict_hash\",\"activationCode\":\"$activation_code\"}")"
echo "identity conflict: HTTP $conflict_code $(jq -c . "$test_dir/conflict.json")"
[[ "$conflict_code" == 409 ]]

access_token="$(gcloud auth print-access-token)"
patch_payload="$(jq -n --arg user_id "$user_id" \
  '{fields:{status:{stringValue:"claimed"},userId:{stringValue:$user_id}}}')"
curl -fsS -X PATCH \
  -H "Authorization: Bearer $access_token" \
  -H "x-goog-user-project: $PROJECT_ID" \
  -H 'Content-Type: application/json' \
  "$document_url?updateMask.fieldPaths=status&updateMask.fieldPaths=userId" \
  -d "$patch_payload" >/dev/null

dd if=/dev/zero of="$test_dir/frame.bin" bs=192000 count=1 status=none
frame_hash="$(shasum -a 256 "$test_dir/frame.bin" | awk '{print $1}')"
gcloud storage cp "$test_dir/frame.bin" "gs://$BUCKET_NAME/$frame_path" \
  --content-type=application/octet-stream --quiet
jq -n --arg image "$frame_path" --arg hash "$frame_hash" \
  '{version:1,image:$image,hash:$hash,bytes:192000,width:800,height:480,format:"everframe-epd7in3e-4bpp"}' \
  > "$test_dir/manifest.json"
gcloud storage cp "$test_dir/manifest.json" "gs://$BUCKET_NAME/$manifest_path" \
  --content-type=application/json --cache-control=no-store --quiet

image_code="$(request "$test_dir/image.json" "$POLL_URL" \
  -H "Authorization: Bearer $secret" -H "X-Device-Id: $device_id")"
echo "image-ready poll: HTTP $image_code"
if [[ "$image_code" != 200 ]]; then
  jq -c . "$test_dir/image.json" 2>/dev/null || head -c 500 "$test_dir/image.json"
  exit 1
fi
[[ "$(jq -r .hash "$test_dir/image.json")" == "$frame_hash" ]]
signed_url="$(jq -er .url "$test_dir/image.json")"
curl -fsS "$signed_url" -o "$test_dir/download.bin"
downloaded_bytes="$(wc -c < "$test_dir/download.bin" | tr -d ' ')"
downloaded_hash="$(shasum -a 256 "$test_dir/download.bin" | awk '{print $1}')"
echo "signed download: bytes=$downloaded_bytes hash_match=$([[ "$downloaded_hash" == "$frame_hash" ]] && echo true || echo false)"
[[ "$downloaded_bytes" == 192000 && "$downloaded_hash" == "$frame_hash" ]]

current_code="$(request "$test_dir/current.json" "$POLL_URL" \
  -H "Authorization: Bearer $secret" -H "X-Device-Id: $device_id" \
  -H "X-Current-Image-Hash: $frame_hash")"
echo "up-to-date poll: HTTP $current_code $(jq -c . "$test_dir/current.json")"
[[ "$current_code" == 200 && "$(jq -r .upToDate "$test_dir/current.json")" == true ]]

# A missing device hash means the panel state is unknown (for example, after
# showing an activation code).  The server must not substitute its historical
# Firestore hash and incorrectly suppress the frame download.
unknown_panel_code="$(request "$test_dir/unknown-panel.json" "$POLL_URL" \
  -H "Authorization: Bearer $secret" -H "X-Device-Id: $device_id")"
echo "unknown-panel poll: HTTP $unknown_panel_code"
[[ "$unknown_panel_code" == 200 ]]
[[ "$(jq -r .hash "$test_dir/unknown-panel.json")" == "$frame_hash" ]]
jq -e '.url | type == "string" and length > 0' "$test_dir/unknown-panel.json" >/dev/null

cors_code="$(curl -sS -D "$test_dir/cors.headers" -o /dev/null -w '%{http_code}' \
  -X OPTIONS "$CLAIM_URL" -H "Origin: $WEB_ORIGIN" \
  -H 'Access-Control-Request-Method: POST' \
  -H 'Access-Control-Request-Headers: Authorization, Content-Type')"
cors_origin="$(awk 'BEGIN{IGNORECASE=1} /^access-control-allow-origin:/ {gsub(/\r/,""); sub(/^[^:]+:[[:space:]]*/,""); print}' "$test_dir/cors.headers")"
echo "claim CORS preflight: HTTP $cors_code allow-origin=$cors_origin"
[[ "$cors_code" == 204 && "$cors_origin" == "$WEB_ORIGIN" ]]

echo "backend smoke test: PASS"
