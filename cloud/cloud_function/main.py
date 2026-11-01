"""Scheduled per-user EverFrame image pipeline."""

from __future__ import annotations

import base64
import datetime as dt
import hashlib
import json
import logging
import mimetypes
import os
import random
from dataclasses import dataclass
from typing import Any

import functions_framework
from google.cloud import firestore, storage
from epaper import FRAME_BYTES, frame_preview_jpeg, image_to_frame
# Google Cloud Functions loads entry points from main.py. Re-export the HTTP
# handlers kept in their own module so all HTTP deployments share one source.
from device_auth import (  # noqa: F401
    claim_device,
    device_poll,
    device_status,
    remove_device,
    refresh_device,
    register_device,
    update_device_schedule,
)

PROJECT_ID = os.environ.get("PROJECT_ID", "everframe-479214")
REGION = os.environ.get("REGION", "us-central1")
BUCKET_NAME = os.environ.get("BUCKET_NAME", "everframe-photo-bucket")
# Gemini 3.1 Flash Image is the current image-to-image model. Keep
# generation opt-in: an accidentally omitted environment variable must never
# turn a normal photo refresh into a paid model call.
DEFAULT_IMAGE_MODEL = "gemini-3.1-flash-image"
IMAGE_MODEL = os.environ.get("IMAGE_MODEL", DEFAULT_IMAGE_MODEL)
IMAGE_LOCATION = os.environ.get("IMAGE_LOCATION", "global")
GENERATE_ART = os.environ.get("GENERATE_ART", "false").lower() in {"1", "true", "yes"}

MAX_GENERATION_STYLES = 200
PROMPTS = (
    "luminous watercolor painting on textured cotton paper",
    "expressive black and white charcoal drawing",
    "oil painting with bold layered impasto brushwork",
    "traditional ukiyo-e woodblock print",
    "soft pastel drawing with blended color",
    "architectural blueprint drawing with crisp white linework",
    "detailed pen-and-ink illustration with crosshatching",
    "layered mixed-media collage",
    "deep blue cyanotype photographic print",
    "layered hand-cut paper collage",
    "matte gouache illustration with opaque color",
    "fine egg tempera painting on a smooth panel",
    "aged fresco mural with mineral pigments",
    "textured encaustic wax painting",
    "minimal sumi-e ink wash painting",
    "bold hand-carved linocut print",
    "intricate black-and-white wood engraving",
    "antique copperplate etching with fine hatching",
    "hand-drawn stone lithograph print",
    "layered screen print with flat saturated inks",
    "luminous stained-glass window composition",
    "colorful glass and stone mosaic",
    "hand-painted ceramic tile with glossy glaze",
    "delicate blue-and-white porcelain painting",
    "jewel-toned cloisonne enamel artwork",
    "ornate woven tapestry",
    "detailed hand-embroidered textile art",
    "geometric quilted fabric artwork",
    "organic woven-fiber wall art",
    "soft needle-felted wool illustration",
    "ornamental Art Nouveau poster with flowing botanical lines",
    "elegant Art Deco poster with geometric ornament",
    "Bauhaus graphic composition with simple geometry",
    "dynamic constructivist poster composition",
    "De Stijl composition with primary-color geometry",
    "suprematist abstract geometric composition",
    "faceted cubist painting with multiple viewpoints",
    "energetic futurist painting emphasizing motion",
    "emotive expressionist painting with forceful color",
    "vivid fauvist painting with non-naturalistic color",
    "light-filled impressionist painting with broken brushwork",
    "pointillist painting built from tiny color dots",
    "dreamlike symbolist painting",
    "dramatic romantic-era landscape painting",
    "balanced neoclassical painting with sculptural clarity",
    "Renaissance panel painting with gentle chiaroscuro",
    "dramatic baroque painting with theatrical light",
    "ornate rococo painting in delicate pastel colors",
    "illuminated Gothic manuscript miniature",
    "gold-ground Byzantine icon mosaic",
    "minimal vector illustration with clean shapes",
    "contemporary flat editorial illustration",
    "bold geometric poster design",
    "precise isometric illustration",
    "faceted low-poly 3D artwork",
    "detailed retro pixel art",
    "colorful voxel-built 3D scene",
    "handmade claymation scene with soft studio lighting",
    "tactile stop-motion miniature set",
    "layered papercraft diorama",
    "faded vintage instant-film photograph",
    "silver-toned nineteenth-century daguerreotype",
    "weathered vintage tintype portrait",
    "high-contrast black-and-white film noir still",
    "surreal infrared landscape photograph",
    "graphic duotone photographic print",
    "two-color risograph print with visible ink texture",
    "vintage halftone comic-book illustration",
    "dramatic mid-century pulp magazine cover",
    "retro screen-printed travel poster",
    "dreamy vaporwave digital art in pastel neon",
    "cinematic synthwave scene with neon gradients",
    "optimistic solarpunk illustration rich with greenery",
    "rainy cyberpunk scene with luminous signage",
    "ornate steampunk illustration with brass machinery",
    "gritty dieselpunk poster with industrial forms",
    "optimistic 1960s space-age illustration",
    "warm cottagecore storybook illustration",
    "moody dark-academia oil illustration",
    "mid-century modern editorial illustration",
    "scientific botanical plate illustration",
    "antique natural-history field illustration",
    "layered topographic map artwork",
    "hand-painted cel animation frame",
    "painterly animated-film background",
    "cinematic graphic-novel panel with inked shadows",
    "gentle children's storybook illustration",
    "whimsical storybook engraving",
    "hand-drawn chalkboard illustration",
    "high-contrast scratchboard engraving",
    "glowing neon-sign artwork against a dark background",
    "iridescent holographic foil artwork",
    "refracted prismatic-glass composition",
    "layered colored-sand illustration",
    "delicate pressed-flower collage",
    "organic botanical leaf-print artwork",
    "swirling hand-marbled paper design",
    "ceramic-inspired kintsugi artwork with golden seams",
    "intricate folded-paper origami composition",
    "layered paper shadow-box scene with depth",
)

logger = logging.getLogger(__name__)
_db: firestore.Client | None = None
_storage: storage.Client | None = None


@dataclass(frozen=True)
class SourceImage:
    document_id: str
    path: str
    name: str


class NoSourceImage(Exception):
    """A claimed owner has not selected/uploaded art yet."""


class ImageGenerationError(RuntimeError):
    """Gemini did not return a usable image; the event should be retried."""


class FrameEncodingError(RuntimeError):
    """The source or generated bytes could not be made into a panel frame."""


class ArtifactPublicationError(RuntimeError):
    """A required GCS artifact could not be published safely."""


def _db_client() -> firestore.Client:
    global _db
    if _db is None:
        _db = firestore.Client(project=PROJECT_ID)
    return _db


def _storage_client() -> storage.Client:
    global _storage
    if _storage is None:
        _storage = storage.Client(project=PROJECT_ID)
    return _storage


def _set_device_state(user_id: str, state: str, **fields: Any) -> None:
    """Best-effort progress for the owner dashboard, never delivery-critical."""
    updates = {
        "refreshState": state,
        "refreshUpdatedAt": firestore.SERVER_TIMESTAMP,
        **fields,
    }
    try:
        query = _db_client().collection("devices").where(
            filter=firestore.FieldFilter("userId", "==", user_id)
        )
        for snapshot in query.stream():
            snapshot.reference.update(updates)
    except Exception:
        logger.exception("Could not record %s device state for user %s", state, user_id)


def _request_is_current(user_id: str, request_id: str | None) -> bool:
    """Return whether a targeted request is still the owner's latest request."""
    if not request_id:
        return True
    query = _db_client().collection("devices").where(
        filter=firestore.FieldFilter("userId", "==", user_id)
    )
    return any(
        (snapshot.to_dict() or {}).get("refreshRequestId") == request_id
        for snapshot in query.stream()
    )


def _set_request_state(
    user_id: str,
    request_id: str | None,
    state: str,
    **fields: Any,
) -> None:
    """Update progress only while this job remains authoritative."""
    if not request_id:
        _set_device_state(user_id, state, **fields)
        return
    try:
        query = _db_client().collection("devices").where(
            filter=firestore.FieldFilter("userId", "==", user_id)
        )
        updates = {
            "refreshState": state,
            "refreshUpdatedAt": firestore.SERVER_TIMESTAMP,
            **fields,
        }
        for snapshot in query.stream():
            if (snapshot.to_dict() or {}).get("refreshRequestId") == request_id:
                snapshot.reference.update(updates)
    except Exception:
        logger.exception("Could not record %s request state for user %s", state, user_id)


def _event_payload(cloud_event: Any) -> dict[str, Any]:
    """Decode a Pub/Sub CloudEvent. Empty scheduler messages are valid."""
    data = getattr(cloud_event, "data", None) or {}
    encoded = (data.get("message") or {}).get("data")
    if not encoded:
        return {}
    try:
        payload = json.loads(base64.b64decode(encoded, validate=True))
    except (ValueError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError("Pub/Sub data must be base64-encoded JSON") from exc
    if not isinstance(payload, dict):
        raise ValueError("Pub/Sub JSON payload must be an object")
    return payload


def _event_id(cloud_event: Any) -> str:
    event_id = getattr(cloud_event, "id", "")
    if not event_id:
        try:
            event_id = cloud_event["id"]
        except (KeyError, TypeError):
            event_id = "manual"
    event_id = str(event_id)
    return "".join(c for c in event_id if c.isalnum() or c in "-_")[:80] or "manual"


def _schedule_is_due(preset: Any, now: dt.datetime) -> bool:
    """Return whether the twice-daily dispatcher should generate this slot."""
    if preset not in {"manual", "12h", "daily", "weekly"}:
        preset = "12h"
    if preset == "manual":
        return False
    if preset == "12h":
        return True
    if preset == "daily":
        return now.hour == 12
    return now.hour == 12 and now.weekday() == 6


def _target_users(payload: dict[str, Any], now: dt.datetime | None = None) -> list[str]:
    requested = payload.get("userId")
    if requested:
        if not isinstance(requested, str) or "/" in requested or len(requested) > 128:
            raise ValueError("invalid userId")
        return [requested]

    now = now or dt.datetime.now(dt.timezone.utc)
    users: set[str] = set()
    query = _db_client().collection("devices").where(
        filter=firestore.FieldFilter("status", "==", "claimed")
    )
    for snapshot in query.stream():
        record = snapshot.to_dict() or {}
        user_id = record.get("userId")
        if isinstance(user_id, str) and user_id and _schedule_is_due(
            record.get("wakeSchedule"), now
        ):
            users.add(user_id)
    return sorted(users)


def _choose_source(user_id: str, requested_path: str | None = None) -> SourceImage:
    prefix = f"users/{user_id}/images/"
    if requested_path:
        if not requested_path.startswith(prefix) or ".." in requested_path:
            raise ValueError("imagePath must be inside the user's images prefix")
        return SourceImage("event", requested_path, requested_path.rsplit("/", 1)[-1])

    candidates: list[SourceImage] = []
    query = _db_client().collection("images").where(
        filter=firestore.FieldFilter("userId", "==", user_id)
    )
    for snapshot in query.stream():
        record = snapshot.to_dict() or {}
        path = record.get("storagePath")
        if record.get("isDisplayed", True) and isinstance(path, str) and path.startswith(prefix):
            candidates.append(SourceImage(snapshot.id, path, str(record.get("name") or snapshot.id)))
    if not candidates:
        raise NoSourceImage(f"no enabled images found for user {user_id}")
    return random.choice(candidates)


def _valid_style(value: Any) -> str | None:
    """Return a safe generation instruction or ``None`` when it is unusable."""
    if not isinstance(value, str):
        return None
    style = value.strip()
    return style if style and len(style) <= 160 else None


def _choose_style(user_id: str, requested_style: Any = None) -> str | None:
    """Choose an explicit style from the owner's editable style library.

    Owner styles are deliberately read from a subcollection rather than a
    top-level collection so one owner can never influence another owner's art.
    New accounts temporarily use the 100 built-ins until the dashboard copies
    them into the owner's editable library. After initialization, an empty
    library is intentional and disables AI restyling rather than silently
    restoring hidden defaults. Up to 200 valid records are considered.
    ``style`` in a targeted Pub/Sub message remains an explicit operator
    override.
    """
    if requested_style is not None:
        style = _valid_style(requested_style)
        if style is None:
            raise ValueError("style must be a non-empty string up to 160 characters")
        return style

    styles: list[str] = []
    library_read_succeeded = False
    user_ref = None
    try:
        user_ref = _db_client().collection("users").document(user_id)
        query = user_ref.collection("generationStyles")
        for snapshot in query.limit(MAX_GENERATION_STYLES).stream():
            record = snapshot.to_dict() or {}
            if record.get("userId") != user_id or record.get("enabled") is False:
                continue
            # ``prompt`` is the canonical dashboard field. ``style`` preserves
            # compatibility with early manually-created records.
            style = _valid_style(record.get("prompt", record.get("style")))
            if style:
                styles.append(style)
        library_read_succeeded = True
    except Exception:
        # A transient Firestore read problem must not stop today's frame.
        # The built-ins are the documented safe fallback for this condition.
        logger.exception("Could not load generation styles for user %s", user_id)
    if styles:
        return random.choice(styles)
    if library_read_succeeded and user_ref is not None:
        try:
            profile = user_ref.get().to_dict() or {}
            if profile.get("generationStylesInitialized") is True:
                return None
        except Exception:
            logger.exception("Could not read style library state for user %s", user_id)
    return random.choice(PROMPTS)


def _generate_art(source: bytes, mime_type: str, style: str) -> bytes:
    """Run image-to-image generation and return encoded image bytes."""
    try:
        from google import genai
        from google.genai import types

        # Gemini image models are served from Vertex AI's global endpoint even
        # though the surrounding Cloud Function and storage stay regional.
        client = genai.Client(vertexai=True, project=PROJECT_ID, location=IMAGE_LOCATION)
        response = client.models.generate_content(
            model=IMAGE_MODEL,
            contents=[
                types.Part.from_bytes(data=source, mime_type=mime_type),
                f"Recreate this photo as a {style}. Preserve the main subject and composition. "
                "Use a portrait 2:3 composition; keep the subject safely centered for a "
                "slightly narrower 3:5 frame. No text or border.",
            ],
            config=types.GenerateContentConfig(
                response_modalities=["IMAGE"],
                # Flash Image supports 2:3 (not 3:5). ``image_to_frame``
                # subsequently center-fits it to the panel's exact 3:5 raster.
                image_config=types.ImageConfig(aspect_ratio="2:3"),
            ),
        )
    except Exception as exc:
        raise ImageGenerationError(
            f"Gemini request failed for model {IMAGE_MODEL}: {type(exc).__name__}"
        ) from exc
    try:
        for part in response.parts or []:
            inline = getattr(part, "inline_data", None)
            if inline and inline.data and str(inline.mime_type or "").startswith("image/"):
                return bytes(inline.data)
    except Exception as exc:
        raise ImageGenerationError(
            f"Gemini model {IMAGE_MODEL} returned an unreadable response"
        ) from exc
    raise ImageGenerationError(f"Gemini model {IMAGE_MODEL} returned no image")


def _already_processed(user_id: str, event_id: str) -> bool:
    blob = _storage_client().bucket(BUCKET_NAME).blob(f"users/{user_id}/manifest.json")
    if not blob.exists():
        return False
    try:
        manifest = json.loads(blob.download_as_bytes())
    except (ValueError, json.JSONDecodeError):
        return False
    return manifest.get("eventId") == event_id


def process_user(user_id: str, event_id: str, payload: dict[str, Any]) -> dict[str, Any]:
    """Process and atomically publish the next frame for one owner."""
    if _already_processed(user_id, event_id):
        return {"userId": user_id, "status": "skipped", "reason": "event already processed"}

    request_id = payload.get("requestId")
    if request_id is not None and (
        not isinstance(request_id, str) or len(request_id) != 32
    ):
        raise ValueError("requestId must be a 32-character string")
    if request_id and not _request_is_current(user_id, request_id):
        return {"userId": user_id, "status": "superseded"}

    requested_path = payload.get("imagePath")
    if requested_path is not None and not isinstance(requested_path, str):
        raise ValueError("imagePath must be a string")
    source = _choose_source(user_id, requested_path)
    source_blob = _storage_client().bucket(BUCKET_NAME).blob(source.path)
    try:
        source_bytes = source_blob.download_as_bytes()
    except Exception as exc:
        raise FrameEncodingError(f"could not download source image {source.path}") from exc
    if not source_bytes:
        raise FrameEncodingError(f"source image {source.path} was empty")
    mime_type = source_blob.content_type or mimetypes.guess_type(source.path)[0] or "image/jpeg"

    # Do not pay for an extra Firestore read when AI generation is disabled.
    style = _choose_style(user_id, payload.get("style")) if GENERATE_ART else None
    generated = _generate_art(source_bytes, mime_type, style) if style else source_bytes
    try:
        frame = image_to_frame(generated)
    except Exception as exc:
        raise FrameEncodingError(f"could not encode panel frame for {source.path}") from exc
    if len(frame) != FRAME_BYTES:
        raise RuntimeError(f"invalid frame size: {len(frame)}")

    if request_id and not _request_is_current(user_id, request_id):
        return {"userId": user_id, "status": "superseded"}

    digest = hashlib.sha256(frame).hexdigest()
    date = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d")
    artifact_path = f"users/{user_id}/daily/{date}-{digest[:12]}.bin"
    preview_path = f"users/{user_id}/daily/{date}-{digest[:12]}.jpg"
    bucket = _storage_client().bucket(BUCKET_NAME)
    try:
        bucket.blob(artifact_path).upload_from_string(
            frame, content_type="application/octet-stream"
        )
    except Exception as exc:
        raise ArtifactPublicationError("could not upload frame artifact") from exc
    try:
        bucket.blob(preview_path).upload_from_string(
            frame_preview_jpeg(frame), content_type="image/jpeg"
        )
    except Exception:  # Preview failure must not block frame delivery.
        logger.exception("Could not upload preview for user %s", user_id)
        preview_path = ""

    manifest = {
        "version": 1, "image": artifact_path, "hash": digest,
        "bytes": FRAME_BYTES, "width": 800, "height": 480,
        "format": "everframe-epd7in3e-4bpp",
        "sourceImageId": source.document_id, "sourcePath": source.path,
        "style": style if GENERATE_ART else None, "preview": preview_path or None,
        "eventId": event_id, "createdAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "requestId": request_id,
    }
    # Object replacement is atomic. Publishing this last prevents a manifest
    # from pointing at a partially uploaded frame.
    manifest_blob = bucket.blob(f"users/{user_id}/manifest.json")
    manifest_blob.cache_control = "no-store"
    try:
        if request_id and not _request_is_current(user_id, request_id):
            return {"userId": user_id, "status": "superseded"}
        manifest_blob.upload_from_string(
            json.dumps(manifest, separators=(",", ":")),
            content_type="application/json",
        )
    except Exception as exc:
        # The frame is intentionally left in place as an unreachable orphan.
        # A Pub/Sub retry can safely overwrite it, but no device sees it until
        # the manifest upload succeeds.
        raise ArtifactPublicationError("could not publish image manifest") from exc
    return {"userId": user_id, "status": "processed", "hash": digest, "image": artifact_path}


@functions_framework.cloud_event
def hello_pubsub(cloud_event: Any) -> None:
    payload = _event_payload(cloud_event)
    event_id = _event_id(cloud_event)
    users = _target_users(payload)
    if not users:
        logger.info("No claimed users to process")
        return

    failures: list[str] = []
    for user_id in users:
        request_id = payload.get("requestId")
        if request_id and not _request_is_current(user_id, request_id):
            logger.info("Ignoring superseded refresh %s for user %s", request_id, user_id)
            continue
        _set_request_state(
            user_id, request_id, "processing", lastError=firestore.DELETE_FIELD
        )
        try:
            result = process_user(user_id, event_id, payload)
            logger.info("EverFrame pipeline result: %s", json.dumps(result))
            if result.get("status") != "superseded":
                _set_request_state(
                    user_id, request_id, "ready", readyHash=result.get("hash")
                )
        except NoSourceImage as exc:
            # This is a normal account state, not a retryable pipeline failure.
            logger.info("Skipping user %s: %s", user_id, exc)
            _set_request_state(user_id, request_id, "waiting_for_photo")
        except Exception as exc:
            if request_id and not _request_is_current(user_id, request_id):
                logger.info("Ignoring failure from superseded refresh %s", request_id)
                continue
            logger.exception("Pipeline failed for user %s", user_id)
            _set_request_state(
                user_id,
                request_id,
                "error",
                lastError="Image preparation failed; retrying.",
            )
            failures.append(f"{user_id}: {exc}")
    if failures:
        # Pub/Sub retries; users completed in this event are skipped next time.
        raise RuntimeError("; ".join(failures))
