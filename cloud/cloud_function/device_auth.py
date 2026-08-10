# EverFrame device-side cloud functions
# Two HTTP endpoints for the ESP32 Bearer-secret flow:
#   POST /registerDevice  { deviceId, tokenHash, activationCode } → pending
#   GET  /devicePoll      Authorization: Bearer <secret> → claimed? + signed URL

import base64
import hashlib
import hmac
import json
import datetime
import os

import functions_framework
from google.cloud import firestore
from google.cloud import storage

PROJECT_ID = os.environ.get("PROJECT_ID", "everframe-479214")
BUCKET_NAME = os.environ.get("BUCKET_NAME", "everframe-photo-bucket")

_db: firestore.Client | None = None
_gcs: storage.Client | None = None


def _db_client() -> firestore.Client:
    global _db
    if _db is None:
        _db = firestore.Client(project=PROJECT_ID)
    return _db


def _gcs_client() -> storage.Client:
    global _gcs
    if _gcs is None:
        _gcs = storage.Client(project=PROJECT_ID)
    return _gcs


def _json(data: dict, status: int = 200):
    return (json.dumps(data), status, {"Content-Type": "application/json"})


# ---------------------------------------------------------------------------
# POST /registerDevice
# ESP32 calls once after Wi-Fi connects, before showing the activation screen.
# No Bearer header — hash hasn't been verified yet. Idempotent.
# ---------------------------------------------------------------------------
@functions_framework.http
def register_device(request):  # type: ignore[no-untyped-def]
    body = request.get_json(silent=True) or {}
    device_id = (body.get("deviceId") or "").strip().lower()
    token_hash = (body.get("tokenHash") or "").strip().lower()
    code = (body.get("activationCode") or "").strip().upper()

    if not device_id or len(device_id) != 32:
        return _json({"error": "invalid deviceId (want 32 hex chars)"}, 400)
    if not token_hash or len(token_hash) != 64:
        return _json({"error": "invalid tokenHash (want 64 hex SHA256)"}, 400)
    if not code or len(code) != 6:
        return _json({"error": "invalid activationCode (want 6 chars)"}, 400)
    # basic hex sanity
    try:
        bytes.fromhex(device_id)
        bytes.fromhex(token_hash)
    except ValueError:
        return _json({"error": "deviceId/tokenHash must be hex"}, 400)

    ref = _db_client().collection("devices").document(device_id)
    snap = ref.get()
    if snap.exists:
        existing_hash = (snap.get("tokenHash") or "").lower()
        if existing_hash and existing_hash != token_hash:
            return _json({"error": "tokenHash mismatch — factory reset?"}, 409)
        return _json({"ok": True, "deviceId": device_id, "code": snap.get("activationCode") or code}, 200)

    ref.set({
        "activationCode": code,
        "tokenHash": token_hash,
        "status": "pending",
        "createdAt": firestore.SERVER_TIMESTAMP,
    })
    return _json({"ok": True, "deviceId": device_id, "code": code}, 200)


# ---------------------------------------------------------------------------
# GET /devicePoll
# Every poll carries Authorization: Bearer <64-hex secret> + X-Device-Id.
# Server verifies SHA256(secret) == tokenHash (constant-time) before
# returning anything. Guessing deviceId without secret → 401. No enumeration.
# ---------------------------------------------------------------------------
def _unauth(msg: str = "unauthorized"):
    return _json({"error": msg}, 401)


@functions_framework.http
def device_poll(request):  # type: ignore[no-untyped-def]
    # --- extract Bearer ---
    auth = request.headers.get("Authorization", "")
    if not auth.startswith("Bearer "):
        return _unauth("missing Bearer token")
    secret = auth[len("Bearer ") :].strip().lower()
    if len(secret) != 64:
        return _unauth("invalid token length (want 64 hex)")
    try:
        secret_bytes = bytes.fromhex(secret)
    except ValueError:
        return _unauth("token must be hex")

    # --- extract deviceId (header preferred) ---
    device_id = (
        request.headers.get("X-Device-Id", "")
        or request.args.get("deviceId", "")
        or (request.get_json(silent=True) or {}).get("deviceId", "")
    )
    device_id = device_id.strip().lower()
    if not device_id or len(device_id) != 32:
        return _unauth("missing deviceId")
    try:
        bytes.fromhex(device_id)
    except ValueError:
        return _unauth("deviceId must be hex")

    # --- load device, verify hash ---
    doc = _db_client().collection("devices").document(device_id).get()
    if not doc.exists or not doc.get("tokenHash"):
        return _unauth("unknown device — register first")

    expected = doc.get("tokenHash").lower()
    computed = hashlib.sha256(secret_bytes).hexdigest()
    if not hmac.compare_digest(computed, expected):
        return _unauth("token mismatch")

    # --- pending → not claimed yet ---
    if doc.get("status") != "claimed" or not doc.get("userId"):
        return _json({"claimed": False}, 200)

    user_id = doc.get("userId")

    # --- manifest lookup, per-user ---
    manifest_blob = _gcs_client().bucket(BUCKET_NAME).blob(f"users/{user_id}/manifest.json")
    if not manifest_blob.exists():
        return _json({"claimed": True, "hasImage": False}, 200)

    manifest = json.loads(manifest_blob.download_as_string())
    # expected: {"image": "users/{uid}/daily/2026-08-10.bin", "hash": "sha256hex"}
    manifest_hash = manifest.get("hash") or manifest.get("sha256") or ""
    manifest_image = manifest.get("image") or ""

    last_hash = doc.get("lastImageHash") or ""
    if last_hash and manifest_hash and last_hash == manifest_hash:
        return _json({"claimed": True, "upToDate": True, "hash": manifest_hash}, 200)

    if not manifest_image:
        return _json({"claimed": True, "hasImage": False, "hash": manifest_hash}, 200)

    # --- signed URL (15 min), v4 ---
    blob = _gcs_client().bucket(BUCKET_NAME).blob(manifest_image)
    url = blob.generate_signed_url(
        version="v4",
        expiration=datetime.timedelta(minutes=15),
        method="GET",
    )
    return _json({"claimed": True, "hash": manifest_hash, "url": url}, 200)
