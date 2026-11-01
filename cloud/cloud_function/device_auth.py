# EverFrame device-side cloud functions
# Two HTTP endpoints for the ESP32 Bearer-secret flow:
#   POST /registerDevice  { deviceId, tokenHash, activationCode } → pending
#   GET  /devicePoll      Authorization: Bearer <secret> → claimed? + signed URL

from __future__ import annotations

import hashlib
import hmac
import json
import datetime
import logging
import os
import uuid

import functions_framework
import firebase_admin
from firebase_admin import auth as firebase_auth
import google.auth
from google.auth.transport.requests import Request as AuthRequest
from google.cloud import firestore
from google.cloud import pubsub_v1
from google.cloud import storage

PROJECT_ID = os.environ.get("PROJECT_ID", "everframe-479214")
BUCKET_NAME = os.environ.get("BUCKET_NAME", "everframe-photo-bucket")
TOPIC_ID = os.environ.get("TOPIC_ID", "scheduler-event")
ALLOWED_ORIGINS = {
    origin.strip()
    for origin in os.environ.get(
        "ALLOWED_ORIGINS",
        "https://everframe-479214.web.app,https://everframe-479214.firebaseapp.com,http://localhost:3000",
    ).split(",")
    if origin.strip()
}
DEVICE_ONLINE_SECONDS = 45
BUTTON_WAKE_MINUTES = 5
DEFAULT_WAKE_SCHEDULE = "12h"
DEFAULT_SCHEDULE_VERSION = "default-12h-v1"
WAKE_SCHEDULES = {"manual", "12h", "daily", "weekly"}
SCHEDULE_WAKE_HOUR_UTC = 12
SCHEDULE_WAKE_MINUTE_UTC = 15

logger = logging.getLogger(__name__)

_db: firestore.Client | None = None
_gcs: storage.Client | None = None
_publisher: pubsub_v1.PublisherClient | None = None


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


def _publisher_client() -> pubsub_v1.PublisherClient:
    global _publisher
    if _publisher is None:
        _publisher = pubsub_v1.PublisherClient()
    return _publisher


def _queue_refresh(
    user_id: str,
    image_path: str | None = None,
    request_id: str | None = None,
) -> None:
    payload = {"userId": user_id}
    if image_path:
        payload["imagePath"] = image_path
    if request_id:
        payload["requestId"] = request_id
    topic = _publisher_client().topic_path(PROJECT_ID, TOPIC_ID)
    _publisher_client().publish(
        topic, json.dumps(payload, separators=(",", ":")).encode("utf-8")
    ).result(timeout=10)


def _json(data: dict, status: int = 200):
    return (
        json.dumps(data),
        status,
        {"Content-Type": "application/json", "Cache-Control": "no-store"},
    )


def _claim_json(request, data: dict, status: int = 200):  # type: ignore[no-untyped-def]
    origin = request.headers.get("Origin", "")
    headers = {"Content-Type": "application/json", "Cache-Control": "no-store", "Vary": "Origin"}
    if origin in ALLOWED_ORIGINS:
        headers["Access-Control-Allow-Origin"] = origin
        headers["Access-Control-Allow-Headers"] = "Authorization, Content-Type"
        headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return (json.dumps(data), status, headers)


def _verify_firebase_user(request):  # type: ignore[no-untyped-def]
    header = request.headers.get("Authorization", "")
    if not header.startswith("Bearer "):
        raise ValueError("missing Firebase ID token")
    if not firebase_admin._apps:
        firebase_admin.initialize_app(options={"projectId": PROJECT_ID})
    return firebase_auth.verify_id_token(header[len("Bearer ") :].strip(), check_revoked=True)


def _timestamp_json(value):  # type: ignore[no-untyped-def]
    return value.isoformat() if isinstance(value, datetime.datetime) else None


def _schedule_values(record: dict, now: datetime.datetime) -> dict:
    """Return the authenticated device timing contract for one poll."""
    preset = record.get("wakeSchedule")
    if preset not in WAKE_SCHEDULES:
        preset = DEFAULT_WAKE_SCHEDULE
    version = record.get("scheduleVersion")
    if not isinstance(version, str) or not version:
        version = DEFAULT_SCHEDULE_VERSION

    next_wake = None
    if preset != "manual":
        candidate = now.replace(
            hour=SCHEDULE_WAKE_HOUR_UTC,
            minute=SCHEDULE_WAKE_MINUTE_UTC,
            second=0,
            microsecond=0,
        )
        if preset == "12h":
            morning = candidate.replace(hour=0)
            evening = candidate
            if now < morning:
                candidate = morning
            elif now < evening:
                candidate = evening
            else:
                candidate = morning + datetime.timedelta(days=1)
        elif preset == "daily":
            if candidate <= now:
                candidate += datetime.timedelta(days=1)
        else:  # weekly, Sunday at 12:15 UTC
            days_until_sunday = (6 - candidate.weekday()) % 7
            candidate += datetime.timedelta(days=days_until_sunday)
            if candidate <= now:
                candidate += datetime.timedelta(days=7)
        next_wake = int(candidate.timestamp())

    return {
        "schedule": preset,
        "scheduleVersion": version,
        "serverTime": int(now.timestamp()),
        "nextWakeAt": next_wake,
    }


def _poll_json(record: dict, now: datetime.datetime, data: dict, status: int = 200):
    payload = dict(data)
    payload.update(_schedule_values(record, now))
    return _json(payload, status)


def _update_owner_devices(user_id: str, updates: dict) -> None:
    """Best-effort status metadata; delivery must not depend on dashboard state."""
    query = _db_client().collection("devices").where(
        filter=firestore.FieldFilter("userId", "==", user_id)
    )
    for snapshot in query.stream():
        snapshot.reference.update(updates)


def _begin_refresh_request(user_id: str, image_path: str, request_id: str) -> bool:
    """Make this request authoritative before its Pub/Sub job can start."""
    query = _db_client().collection("devices").where(
        filter=firestore.FieldFilter("userId", "==", user_id)
    )
    snapshots = list(query.stream())
    replaced = any(
        (snapshot.to_dict() or {}).get("refreshState") in {"queued", "processing", "ready"}
        for snapshot in snapshots
    )
    updates = {
        "refreshState": "queued",
        "refreshRequestId": request_id,
        "requestedImagePath": image_path,
        "refreshRequestedAt": firestore.SERVER_TIMESTAMP,
        "lastError": firestore.DELETE_FIELD,
    }
    for snapshot in snapshots:
        snapshot.reference.update(updates)
    return replaced


# ---------------------------------------------------------------------------
# POST /claimDevice
# Authenticated owner-portal endpoint. Keeping the activation lookup server-side
# means Firestore device listing can remain completely disabled for clients.
# ---------------------------------------------------------------------------
@functions_framework.http
def claim_device(request):  # type: ignore[no-untyped-def]
    if request.method == "OPTIONS":
        return _claim_json(request, {}, 204)
    if request.method != "POST":
        return _claim_json(request, {"error": "method not allowed"}, 405)
    if request.headers.get("Origin") and request.headers.get("Origin") not in ALLOWED_ORIGINS:
        return _claim_json(request, {"error": "origin not allowed"}, 403)

    try:
        token = _verify_firebase_user(request)
    except Exception:
        return _claim_json(request, {"error": "invalid or expired sign-in"}, 401)

    code = ((request.get_json(silent=True) or {}).get("activationCode") or "").strip().upper()
    if len(code) != 6 or not code.isalnum():
        return _claim_json(request, {"error": "activationCode must be 6 letters or digits"}, 400)

    query = _db_client().collection("devices").where(
        filter=firestore.FieldFilter("activationCode", "==", code)
    ).where(filter=firestore.FieldFilter("status", "==", "pending")).limit(2)
    matches = list(query.stream())
    if not matches:
        return _claim_json(request, {"error": "no pending device found for that code"}, 404)
    if len(matches) > 1:
        return _claim_json(request, {"error": "activation code collision; register the frame again"}, 409)

    ref = matches[0].reference
    transaction = _db_client().transaction()

    @firestore.transactional
    def claim(txn):  # type: ignore[no-untyped-def]
        current = ref.get(transaction=txn)
        if not current.exists or current.get("status") != "pending":
            raise ValueError("device is no longer pending")
        txn.update(ref, {
            "userId": token["uid"],
            "status": "claimed",
            "claimedAt": firestore.SERVER_TIMESTAMP,
            "refreshState": "linked",
            "wakeSchedule": DEFAULT_WAKE_SCHEDULE,
            "scheduleVersion": uuid.uuid4().hex,
            "scheduleUpdatedAt": firestore.SERVER_TIMESTAMP,
            "appliedScheduleVersion": firestore.DELETE_FIELD,
        })

    try:
        claim(transaction)
    except ValueError as exc:
        return _claim_json(request, {"error": str(exc)}, 409)
    refresh_queued = True
    try:
        _queue_refresh(token["uid"])
    except Exception:
        # Ownership is already committed. The daily scheduler remains a
        # fallback, so report the queue state without turning a valid claim
        # into an apparent failure.
        refresh_queued = False
    return _claim_json(request, {
        "ok": True,
        "deviceId": matches[0].id,
        "code": code,
        "refreshQueued": refresh_queued,
    })


# ---------------------------------------------------------------------------
# POST /removeDevice
# Authenticated owner-portal endpoint. Unpair without deleting the hardware
# credential so the physical frame can immediately return to activation mode.
# ---------------------------------------------------------------------------
@functions_framework.http
def remove_device(request):  # type: ignore[no-untyped-def]
    if request.method == "OPTIONS":
        return _claim_json(request, {}, 204)
    if request.method != "POST":
        return _claim_json(request, {"error": "method not allowed"}, 405)
    if request.headers.get("Origin") and request.headers.get("Origin") not in ALLOWED_ORIGINS:
        return _claim_json(request, {"error": "origin not allowed"}, 403)

    try:
        token = _verify_firebase_user(request)
    except Exception:
        return _claim_json(request, {"error": "invalid or expired sign-in"}, 401)

    device_id = ((request.get_json(silent=True) or {}).get("deviceId") or "").strip().lower()
    if len(device_id) != 32:
        return _claim_json(request, {"error": "deviceId must be 32 hex characters"}, 400)
    try:
        bytes.fromhex(device_id)
    except ValueError:
        return _claim_json(request, {"error": "deviceId must be 32 hex characters"}, 400)

    ref = _db_client().collection("devices").document(device_id)
    transaction = _db_client().transaction()

    @firestore.transactional
    def unpair(txn):  # type: ignore[no-untyped-def]
        current = ref.get(transaction=txn)
        record = current.to_dict() if current.exists else {}
        if not current.exists or record.get("userId") != token["uid"]:
            raise LookupError("device not found for this account")
        txn.update(ref, {
            "status": "pending",
            "userId": firestore.DELETE_FIELD,
            "claimedAt": firestore.DELETE_FIELD,
            # The panel will show activation next, so its prior photo can no
            # longer be treated as physically displayed.
            "lastImageHash": firestore.DELETE_FIELD,
            "refreshState": firestore.DELETE_FIELD,
            "refreshRequestId": firestore.DELETE_FIELD,
            "requestedImagePath": firestore.DELETE_FIELD,
            "refreshRequestedAt": firestore.DELETE_FIELD,
            "lastError": firestore.DELETE_FIELD,
            "awakeUntil": firestore.DELETE_FIELD,
            "displayedAt": firestore.DELETE_FIELD,
            "wakeSchedule": firestore.DELETE_FIELD,
            "scheduleVersion": firestore.DELETE_FIELD,
            "scheduleUpdatedAt": firestore.DELETE_FIELD,
            "appliedScheduleVersion": firestore.DELETE_FIELD,
            "scheduleAppliedAt": firestore.DELETE_FIELD,
            "unpairedAt": firestore.SERVER_TIMESTAMP,
        })

    try:
        unpair(transaction)
    except LookupError as exc:
        # Do not reveal whether another account owns the supplied device ID.
        return _claim_json(request, {"error": str(exc)}, 404)
    except Exception:
        logger.exception("Could not unpair device %s", device_id)
        return _claim_json(request, {"error": "could not remove device"}, 503)

    return _claim_json(request, {"ok": True, "deviceId": device_id, "status": "pending"})


# ---------------------------------------------------------------------------
# POST /refreshDevice
# Authenticated owner-portal endpoint. Queue processing immediately after an
# upload instead of waiting for the daily rotation schedule.
# ---------------------------------------------------------------------------
@functions_framework.http
def refresh_device(request):  # type: ignore[no-untyped-def]
    if request.method == "OPTIONS":
        return _claim_json(request, {}, 204)
    if request.method != "POST":
        return _claim_json(request, {"error": "method not allowed"}, 405)
    if request.headers.get("Origin") and request.headers.get("Origin") not in ALLOWED_ORIGINS:
        return _claim_json(request, {"error": "origin not allowed"}, 403)

    try:
        token = _verify_firebase_user(request)
    except Exception:
        return _claim_json(request, {"error": "invalid or expired sign-in"}, 401)

    user_id = token["uid"]
    image_path = ((request.get_json(silent=True) or {}).get("imagePath") or "").strip()
    expected_prefix = f"users/{user_id}/images/"
    if not image_path.startswith(expected_prefix) or ".." in image_path:
        return _claim_json(request, {"error": "imagePath must belong to the signed-in user"}, 400)

    request_id = uuid.uuid4().hex
    try:
        replaced = _begin_refresh_request(user_id, image_path, request_id)
    except Exception:
        logger.exception("Could not establish refresh ordering for user %s", user_id)
        return _claim_json(request, {"error": "could not queue device refresh"}, 503)
    try:
        _queue_refresh(user_id, image_path, request_id)
    except Exception:
        try:
            _update_owner_devices(user_id, {
                "refreshState": "error",
                "lastError": "Could not queue this photo. Please try again.",
            })
        except Exception:
            logger.exception("Could not record queue failure for user %s", user_id)
        return _claim_json(request, {"error": "could not queue device refresh"}, 503)
    return _claim_json(request, {
        "ok": True,
        "queued": True,
        "replaced": replaced,
        "requestId": request_id,
    }, 202)


# ---------------------------------------------------------------------------
# GET /deviceStatus
# Authenticated owner-portal endpoint. Returns only compact, user-facing state;
# token hashes, activation codes, and signed URLs never leave the server.
# ---------------------------------------------------------------------------
@functions_framework.http
def device_status(request):  # type: ignore[no-untyped-def]
    if request.method == "OPTIONS":
        return _claim_json(request, {}, 204)
    if request.method != "GET":
        return _claim_json(request, {"error": "method not allowed"}, 405)
    if request.headers.get("Origin") and request.headers.get("Origin") not in ALLOWED_ORIGINS:
        return _claim_json(request, {"error": "origin not allowed"}, 403)

    try:
        token = _verify_firebase_user(request)
    except Exception:
        return _claim_json(request, {"error": "invalid or expired sign-in"}, 401)

    user_id = token["uid"]
    now = datetime.datetime.now(datetime.timezone.utc)
    devices = []
    query = _db_client().collection("devices").where(
        filter=firestore.FieldFilter("userId", "==", user_id)
    )
    for snapshot in query.stream():
        record = snapshot.to_dict() or {}
        last_seen = record.get("lastSeen")
        awake_until = record.get("awakeUntil")
        online = isinstance(last_seen, datetime.datetime) and (
            now - last_seen.astimezone(datetime.timezone.utc)
        ).total_seconds() <= DEVICE_ONLINE_SECONDS
        awake = isinstance(awake_until, datetime.datetime) and (
            awake_until.astimezone(datetime.timezone.utc) > now
        )
        schedule = _schedule_values(record, now)
        applied_version = record.get("appliedScheduleVersion")
        devices.append({
            "id": snapshot.id,
            "state": record.get("refreshState") or "linked",
            "online": online,
            "awake": awake,
            "lastSeen": _timestamp_json(last_seen),
            "displayedHash": record.get("lastImageHash"),
            "lastError": record.get("lastError"),
            "schedule": schedule["schedule"],
            "scheduleVersion": schedule["scheduleVersion"],
            "appliedScheduleVersion": applied_version,
            "schedulePending": applied_version != schedule["scheduleVersion"],
            "nextWakeAt": (
                datetime.datetime.fromtimestamp(
                    schedule["nextWakeAt"], datetime.timezone.utc
                ).isoformat()
                if schedule["nextWakeAt"] is not None else None
            ),
        })

    current = None
    manifest_blob = _gcs_client().bucket(BUCKET_NAME).blob(f"users/{user_id}/manifest.json")
    if manifest_blob.exists():
        try:
            manifest = json.loads(manifest_blob.download_as_string())
            current = {
                "hash": manifest.get("hash"),
                "sourcePath": manifest.get("sourcePath"),
                "createdAt": manifest.get("createdAt"),
            }
        except (TypeError, ValueError, json.JSONDecodeError):
            logger.warning("Invalid manifest while reading device status for %s", user_id)

    return _claim_json(request, {"devices": devices, "current": current})


# ---------------------------------------------------------------------------
# POST /updateDeviceSchedule
# Owner selects one bounded preset. The sleeping frame applies it on its next
# authenticated poll (or immediately after a short button press).
# ---------------------------------------------------------------------------
@functions_framework.http
def update_device_schedule(request):  # type: ignore[no-untyped-def]
    if request.method == "OPTIONS":
        return _claim_json(request, {}, 204)
    if request.method != "POST":
        return _claim_json(request, {"error": "method not allowed"}, 405)
    if request.headers.get("Origin") and request.headers.get("Origin") not in ALLOWED_ORIGINS:
        return _claim_json(request, {"error": "origin not allowed"}, 403)
    try:
        token = _verify_firebase_user(request)
    except Exception:
        return _claim_json(request, {"error": "invalid or expired sign-in"}, 401)

    body = request.get_json(silent=True) or {}
    device_id = (body.get("deviceId") or "").strip().lower()
    schedule = (body.get("schedule") or "").strip().lower()
    try:
        if len(device_id) != 32:
            raise ValueError
        bytes.fromhex(device_id)
    except ValueError:
        return _claim_json(request, {"error": "deviceId must be 32 hex characters"}, 400)
    if schedule not in WAKE_SCHEDULES:
        return _claim_json(request, {"error": "unsupported schedule"}, 400)

    ref = _db_client().collection("devices").document(device_id)
    version = uuid.uuid4().hex
    transaction = _db_client().transaction()

    @firestore.transactional
    def save_schedule(txn):  # type: ignore[no-untyped-def]
        snapshot = ref.get(transaction=txn)
        record = snapshot.to_dict() if snapshot.exists else {}
        if not snapshot.exists or record.get("userId") != token["uid"]:
            raise LookupError("device not found for this account")
        txn.update(ref, {
            "wakeSchedule": schedule,
            "scheduleVersion": version,
            "scheduleUpdatedAt": firestore.SERVER_TIMESTAMP,
        })

    try:
        save_schedule(transaction)
    except LookupError as exc:
        return _claim_json(request, {"error": str(exc)}, 404)
    except Exception:
        logger.exception("Could not update schedule for device %s", device_id)
        return _claim_json(request, {"error": "could not update schedule"}, 503)
    timing = _schedule_values(
        {"wakeSchedule": schedule, "scheduleVersion": version},
        datetime.datetime.now(datetime.timezone.utc),
    )
    return _claim_json(request, {
        "ok": True,
        "deviceId": device_id,
        "schedule": schedule,
        "scheduleVersion": version,
        "nextWakeAt": timing["nextWakeAt"],
        "pending": True,
    })


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
    ref = _db_client().collection("devices").document(device_id)
    doc = ref.get()
    if not doc.exists or not doc.get("tokenHash"):
        return _unauth("unknown device — register first")

    device_record = doc.to_dict() or {}
    expected = device_record.get("tokenHash", "").lower()
    computed = hashlib.sha256(secret_bytes).hexdigest()
    if not hmac.compare_digest(computed, expected):
        return _unauth("token mismatch")

    last_hash = (
        request.headers.get("X-Current-Image-Hash", "")
        or request.args.get("lastHash", "")
        or ""
    )
    valid_last_hash = isinstance(last_hash, str) and len(last_hash) == 64
    if valid_last_hash:
        try:
            bytes.fromhex(last_hash)
        except ValueError:
            valid_last_hash = False

    now = datetime.datetime.now(datetime.timezone.utc)
    presence = {"lastSeen": now}
    if valid_last_hash:
        presence["lastImageHash"] = last_hash.lower()
    if request.headers.get("X-Wake-Window", "").lower() == "button":
        existing_awake_until = device_record.get("awakeUntil")
        if not isinstance(existing_awake_until, datetime.datetime) or (
            existing_awake_until.astimezone(datetime.timezone.utc) <= now
        ):
            presence["awakeUntil"] = now + datetime.timedelta(minutes=BUTTON_WAKE_MINUTES)
    reported_schedule_version = request.headers.get("X-Schedule-Version", "").strip()
    active_schedule_version = _schedule_values(device_record, now)["scheduleVersion"]
    if reported_schedule_version and hmac.compare_digest(
        reported_schedule_version, active_schedule_version
    ):
        presence["appliedScheduleVersion"] = active_schedule_version
        presence["scheduleAppliedAt"] = now
    try:
        ref.update(presence)
    except Exception:
        # Presence is advisory and must not prevent an authenticated frame from
        # receiving the current image.
        logger.exception("Could not update presence for device %s", device_id)

    # --- pending → not claimed yet ---
    if device_record.get("status") != "claimed" or not device_record.get("userId"):
        return _json({"claimed": False}, 200)

    user_id = device_record.get("userId")

    # --- manifest lookup, per-user ---
    manifest_blob = _gcs_client().bucket(BUCKET_NAME).blob(f"users/{user_id}/manifest.json")
    if not manifest_blob.exists():
        return _poll_json(device_record, now, {"claimed": True, "hasImage": False}, 200)

    try:
        manifest = json.loads(manifest_blob.download_as_string())
    except (TypeError, ValueError, json.JSONDecodeError):
        return _json({"error": "invalid image manifest"}, 503)
    # expected: {"image": "users/{uid}/daily/2026-08-10.bin", "hash": "sha256hex"}
    manifest_hash = manifest.get("hash") or manifest.get("sha256") or ""
    manifest_image = manifest.get("image") or ""
    active_request_id = device_record.get("refreshRequestId")
    manifest_request_id = manifest.get("requestId")

    # A replacement becomes authoritative as soon as refreshDevice records its
    # request ID. Never hand an older manifest to a frame during the interval
    # before the replacement finishes.
    if active_request_id and manifest_request_id != active_request_id:
        return _poll_json(device_record, now, {"claimed": True, "processing": True}, 200)

    expected_prefix = f"users/{user_id}/daily/"
    if not isinstance(manifest_hash, str) or len(manifest_hash) != 64:
        return _json({"error": "invalid image manifest hash"}, 503)
    try:
        bytes.fromhex(manifest_hash)
    except ValueError:
        return _json({"error": "invalid image manifest hash"}, 503)
    if not isinstance(manifest_image, str) or not manifest_image.startswith(expected_prefix):
        return _json({"error": "invalid image manifest path"}, 503)

    if valid_last_hash and manifest_hash and last_hash.lower() == manifest_hash.lower():
        try:
            ref.update({
                "refreshState": "displayed",
                "lastImageHash": manifest_hash.lower(),
                "displayedAt": firestore.SERVER_TIMESTAMP,
                "lastError": firestore.DELETE_FIELD,
                "refreshRequestId": firestore.DELETE_FIELD,
            })
        except Exception:
            logger.exception("Could not record displayed frame for device %s", device_id)
        return _poll_json(
            device_record, now,
            {"claimed": True, "upToDate": True, "hash": manifest_hash}, 200,
        )

    # --- signed URL (15 min), v4 ---
    blob = _gcs_client().bucket(BUCKET_NAME).blob(manifest_image)
    if not blob.exists():
        return _json({"error": "image artifact is not available"}, 503)
    blob.reload()
    if blob.size != 192000:
        return _json({"error": "image artifact has invalid size"}, 503)

    # Metadata-server credentials do not contain a private key. Supplying their
    # service-account identity + access token makes google-cloud-storage sign via
    # IAM Credentials signBlob (roles/iam.serviceAccountTokenCreator required).
    credentials, _ = google.auth.default()
    if not credentials.valid:
        credentials.refresh(AuthRequest())
    service_account_email = getattr(credentials, "service_account_email", None)
    if service_account_email and service_account_email != "default":
        url = blob.generate_signed_url(
            version="v4", expiration=datetime.timedelta(minutes=15), method="GET",
            service_account_email=service_account_email, access_token=credentials.token,
        )
    else:
        url = blob.generate_signed_url(
            version="v4", expiration=datetime.timedelta(minutes=15), method="GET"
        )
    return _poll_json(
        device_record, now,
        {"claimed": True, "hash": manifest_hash, "url": url}, 200,
    )
