import base64
import io
import json
import sys
import unittest
from types import ModuleType
from types import SimpleNamespace
from unittest.mock import call, patch

from PIL import Image

import main


class EventParsingTests(unittest.TestCase):
    def test_empty_scheduler_message_targets_all_users(self):
        event = SimpleNamespace(data={"message": {}})
        self.assertEqual(main._event_payload(event), {})

    def test_decodes_json_payload(self):
        encoded = base64.b64encode(json.dumps({"userId": "owner-1"}).encode()).decode()
        event = SimpleNamespace(data={"message": {"data": encoded}})
        self.assertEqual(main._event_payload(event), {"userId": "owner-1"})

    def test_rejects_malformed_payload(self):
        event = SimpleNamespace(data={"message": {"data": "%%%"}})
        with self.assertRaisesRegex(ValueError, "base64-encoded JSON"):
            main._event_payload(event)

    def test_sanitizes_event_id_for_object_metadata(self):
        event = SimpleNamespace(id="../event/123")
        self.assertEqual(main._event_id(event), "event123")

    def test_reads_mapping_style_cloudevent_id(self):
        self.assertEqual(main._event_id({"id": "pubsub-message-123"}), "pubsub-message-123")

    def test_scheduler_presets_are_due_only_in_their_slots(self):
        midnight = main.dt.datetime(2026, 11, 1, 0, 0, tzinfo=main.dt.timezone.utc)
        sunday_noon = main.dt.datetime(2026, 11, 1, 12, 0, tzinfo=main.dt.timezone.utc)
        monday_noon = main.dt.datetime(2026, 11, 2, 12, 0, tzinfo=main.dt.timezone.utc)
        self.assertFalse(main._schedule_is_due("manual", sunday_noon))
        self.assertTrue(main._schedule_is_due("12h", midnight))
        self.assertTrue(main._schedule_is_due("12h", sunday_noon))
        self.assertFalse(main._schedule_is_due("daily", midnight))
        self.assertTrue(main._schedule_is_due("daily", sunday_noon))
        self.assertTrue(main._schedule_is_due("weekly", sunday_noon))
        self.assertFalse(main._schedule_is_due("weekly", monday_noon))


class FakeBlob:
    def __init__(self, path, uploads, failing_paths=()):
        self.path = path
        self.uploads = uploads
        self.failing_paths = failing_paths
        self.content_type = "image/png"

    def download_as_bytes(self):
        output = io.BytesIO()
        Image.new("RGB", (16, 16), "red").save(output, "PNG")
        return output.getvalue()

    def upload_from_string(self, data, **kwargs):
        if self.path in self.failing_paths:
            raise OSError(f"simulated upload failure for {self.path}")
        self.uploads.append((self.path, data, kwargs))


class FakeBucket:
    def __init__(self, failing_paths=()):
        self.uploads = []
        self.failing_paths = failing_paths

    def blob(self, path):
        return FakeBlob(path, self.uploads, self.failing_paths)


class FakeStorage:
    def __init__(self, bucket):
        self._bucket = bucket

    def bucket(self, _name):
        return self._bucket


class FakeSnapshot:
    def __init__(self, record):
        self.record = record

    def to_dict(self):
        return self.record


class FakeStyleQuery:
    def __init__(self, records):
        self.records = records
        self.limit_size = None

    def limit(self, size):
        self.limit_size = size
        return self

    def stream(self):
        return iter(FakeSnapshot(record) for record in self.records[: self.limit_size])


class FakeDocument:
    def __init__(self, records, profile=None):
        self.records = records
        self.profile = profile or {}

    def collection(self, name):
        assert name == "generationStyles"
        return FakeStyleQuery(self.records)

    def get(self):
        return FakeSnapshot(self.profile)


class FakeUsersCollection:
    def __init__(self, records, profile=None):
        self.records = records
        self.profile = profile

    def document(self, user_id):
        assert user_id == "owner"
        return FakeDocument(self.records, self.profile)


class FakeDb:
    def __init__(self, records, profile=None):
        self.records = records
        self.profile = profile

    def collection(self, name):
        assert name == "users"
        return FakeUsersCollection(self.records, self.profile)


class ProcessingTests(unittest.TestCase):
    def test_uploads_frame_before_manifest(self):
        bucket = FakeBucket()
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        with patch.object(main, "GENERATE_ART", False), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "image_to_frame", return_value=b"\x11" * main.FRAME_BYTES), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)):
            result = main.process_user("owner", "event-1", {})

        self.assertEqual(result["status"], "processed")
        self.assertEqual(len(bucket.uploads[0][1]), 192000)
        self.assertTrue(bucket.uploads[0][0].endswith(".bin"))
        self.assertEqual(bucket.uploads[-1][0], "users/owner/manifest.json")
        manifest = json.loads(bucket.uploads[-1][1])
        self.assertEqual(manifest["hash"], result["hash"])
        self.assertEqual(manifest["bytes"], 192000)
        self.assertEqual(manifest["eventId"], "event-1")

    def test_generation_failure_never_publishes_a_frame_or_manifest(self):
        bucket = FakeBucket()
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        with patch.object(main, "GENERATE_ART", True), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "_choose_style", return_value="watercolor painting"), \
             patch.object(main, "_generate_art", side_effect=main.ImageGenerationError("unavailable")), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)):
            with self.assertRaisesRegex(main.ImageGenerationError, "unavailable"):
                main.process_user("owner", "event-1", {})

        self.assertEqual(bucket.uploads, [])

    def test_frame_upload_failure_never_publishes_manifest(self):
        bucket = FakeBucket()
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        # The deterministic hash path is not important: make every artifact
        # write fail by replacing its upload method after source download.
        with patch.object(main, "GENERATE_ART", False), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "image_to_frame", return_value=b"\x11" * main.FRAME_BYTES), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)), \
             patch.object(FakeBlob, "upload_from_string", side_effect=OSError("GCS down")):
            with self.assertRaisesRegex(main.ArtifactPublicationError, "frame artifact"):
                main.process_user("owner", "event-1", {})

        self.assertEqual(bucket.uploads, [])

    def test_preview_failure_still_publishes_deliverable_manifest(self):
        bucket = FakeBucket()
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        with patch.object(main, "GENERATE_ART", False), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "image_to_frame", return_value=b"\x11" * main.FRAME_BYTES), \
             patch.object(main, "frame_preview_jpeg", side_effect=OSError("preview down")), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)):
            main.process_user("owner", "event-1", {})

        manifest = json.loads(bucket.uploads[-1][1])
        self.assertIsNone(manifest["preview"])

    def test_manifest_failure_leaves_previous_manifest_unpublished(self):
        bucket = FakeBucket(failing_paths=("users/owner/manifest.json",))
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        with patch.object(main, "GENERATE_ART", False), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "image_to_frame", return_value=b"\x11" * main.FRAME_BYTES), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)):
            with self.assertRaisesRegex(main.ArtifactPublicationError, "manifest"):
                main.process_user("owner", "event-1", {})

        self.assertEqual(len(bucket.uploads), 2)
        self.assertFalse(any(path.endswith("manifest.json") for path, *_ in bucket.uploads))

    def test_replaced_request_is_discarded_before_artifact_upload(self):
        bucket = FakeBucket()
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        payload = {"requestId": "a" * 32}
        with patch.object(main, "GENERATE_ART", False), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_request_is_current", side_effect=[True, False]), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "image_to_frame", return_value=b"\x11" * main.FRAME_BYTES), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)):
            result = main.process_user("owner", "event-1", payload)

        self.assertEqual(result["status"], "superseded")
        self.assertEqual(bucket.uploads, [])

    def test_replacement_during_publication_cannot_overwrite_manifest(self):
        bucket = FakeBucket()
        source = main.SourceImage("image-1", "users/owner/images/source.png", "source")
        payload = {"requestId": "b" * 32}
        with patch.object(main, "GENERATE_ART", False), \
             patch.object(main, "_already_processed", return_value=False), \
             patch.object(main, "_request_is_current", side_effect=[True, True, False]), \
             patch.object(main, "_choose_source", return_value=source), \
             patch.object(main, "image_to_frame", return_value=b"\x11" * main.FRAME_BYTES), \
             patch.object(main, "_storage_client", return_value=FakeStorage(bucket)):
            result = main.process_user("owner", "event-1", payload)

        self.assertEqual(result["status"], "superseded")
        self.assertFalse(any(path.endswith("manifest.json") for path, *_ in bucket.uploads))


class StyleAndGenerationTests(unittest.TestCase):
    def test_has_one_hundred_distinct_fallback_styles(self):
        self.assertEqual(main.MAX_GENERATION_STYLES, 200)
        self.assertEqual(len(main.PROMPTS), 100)
        self.assertEqual(len(set(main.PROMPTS)), 100)

    def test_owner_styles_are_the_editable_generation_pool(self):
        records = [
            {"userId": "someone-else", "prompt": "ignore me"},
            {"userId": "owner", "prompt": " "},
            {"userId": "owner", "prompt": "watercolor painting"},
        ]
        choices = []
        with patch.object(main, "_db_client", return_value=FakeDb(records)), \
             patch.object(main.random, "choice", side_effect=lambda values: choices.append(values) or values[-1]):
            self.assertEqual(main._choose_style("owner"), "watercolor painting")
        self.assertEqual(choices[0], ["watercolor painting"])

    def test_owner_style_query_matches_dashboard_limit(self):
        query = FakeStyleQuery([{"userId": "owner", "prompt": "watercolor"}])
        document = SimpleNamespace(collection=lambda _name: query)
        users = SimpleNamespace(document=lambda _user_id: document)
        database = SimpleNamespace(collection=lambda _name: users)
        with patch.object(main, "_db_client", return_value=database), \
             patch.object(main.random, "choice", side_effect=lambda values: values[-1]):
            self.assertEqual(main._choose_style("owner"), "watercolor")
        self.assertEqual(query.limit_size, 200)

    def test_cleared_initialized_library_disables_hidden_fallback(self):
        database = FakeDb([], {"generationStylesInitialized": True})
        with patch.object(main, "_db_client", return_value=database):
            self.assertIsNone(main._choose_style("owner"))

    def test_targeted_style_is_validated_without_a_firestore_read(self):
        self.assertEqual(main._choose_style("owner", "  ink drawing  "), "ink drawing")
        with self.assertRaisesRegex(ValueError, "non-empty string"):
            main._choose_style("owner", "")

    def test_style_read_failure_uses_a_builtin_fallback(self):
        with patch.object(main, "_db_client", side_effect=OSError("Firestore down")), \
             patch.object(main.random, "choice", return_value="watercolor painting"):
            self.assertEqual(main._choose_style("owner"), "watercolor painting")

    def test_flash_image_request_uses_portrait_provider_aspect_ratio(self):
        captured = {}

        class FakePart:
            @staticmethod
            def from_bytes(**kwargs):
                captured["part"] = kwargs
                return kwargs

        class FakeImageConfig:
            def __init__(self, **kwargs):
                self.kwargs = kwargs

        class FakeGenerateContentConfig:
            def __init__(self, **kwargs):
                self.kwargs = kwargs

        fake_types = SimpleNamespace(
            Part=FakePart,
            ImageConfig=FakeImageConfig,
            GenerateContentConfig=FakeGenerateContentConfig,
        )
        response = SimpleNamespace(parts=[SimpleNamespace(
            inline_data=SimpleNamespace(data=b"generated", mime_type="image/png")
        )])
        fake_client = SimpleNamespace(models=SimpleNamespace(
            generate_content=lambda **kwargs: captured.setdefault("request", kwargs) and response
        ))
        fake_genai = ModuleType("google.genai")
        fake_genai.Client = lambda **kwargs: captured.setdefault("client", kwargs) and fake_client
        fake_genai.types = fake_types
        fake_google = ModuleType("google")
        fake_google.genai = fake_genai

        with patch.dict(sys.modules, {"google": fake_google, "google.genai": fake_genai}):
            generated = main._generate_art(b"source", "image/jpeg", "watercolor painting")

        self.assertEqual(generated, b"generated")
        self.assertEqual(main.DEFAULT_IMAGE_MODEL, "gemini-3.1-flash-image")
        self.assertEqual(captured["client"]["location"], "global")
        self.assertEqual(captured["request"]["config"].kwargs["response_modalities"], ["IMAGE"])
        self.assertEqual(captured["request"]["config"].kwargs["image_config"].kwargs, {"aspect_ratio": "2:3"})

    def test_model_error_has_a_clear_retryable_pipeline_error(self):
        fake_genai = ModuleType("google.genai")
        fake_genai.Client = lambda **kwargs: (_ for _ in ()).throw(OSError("network down"))
        fake_genai.types = SimpleNamespace()
        fake_google = ModuleType("google")
        fake_google.genai = fake_genai
        with patch.dict(sys.modules, {"google": fake_google, "google.genai": fake_genai}):
            with self.assertRaisesRegex(main.ImageGenerationError, "Gemini request failed"):
                main._generate_art(b"source", "image/jpeg", "watercolor painting")


class PipelineStatusTests(unittest.TestCase):
    def test_retryable_failure_records_processing_then_error(self):
        event = SimpleNamespace(data={"message": {}}, id="event-1")
        with patch.object(main, "_target_users", return_value=["owner"]), \
             patch.object(main, "process_user", side_effect=main.ImageGenerationError("down")), \
             patch.object(main, "_set_device_state") as set_state:
            with self.assertRaises(RuntimeError):
                main.hello_pubsub(event)
        self.assertEqual(set_state.call_args_list[0], call("owner", "processing", lastError=main.firestore.DELETE_FIELD))
        self.assertEqual(set_state.call_args_list[-1], call("owner", "error", lastError="Image preparation failed; retrying."))

    def test_no_source_is_non_retryable_and_visible(self):
        event = SimpleNamespace(data={"message": {}}, id="event-1")
        with patch.object(main, "_target_users", return_value=["owner"]), \
             patch.object(main, "process_user", side_effect=main.NoSourceImage("empty")), \
             patch.object(main, "_set_device_state") as set_state:
            main.hello_pubsub(event)
        self.assertEqual(set_state.call_args_list[-1], call("owner", "waiting_for_photo"))


if __name__ == "__main__":
    unittest.main()
