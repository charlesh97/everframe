import datetime
import hashlib
import json
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import device_auth


class FakeRequest:
    def __init__(self, method="GET", headers=None, body=None, args=None):
        self.method = method
        self.headers = headers or {}
        self._body = body or {}
        self.args = args or {}

    def get_json(self, silent=False):
        return self._body


class FakeDeviceSnapshot:
    def __init__(self, device_id, record, reference=None):
        self.id = device_id
        self._record = record
        self.reference = reference
        self.exists = True

    def get(self, name):
        return self._record[name]

    def to_dict(self):
        return self._record


class FakeReference:
    def __init__(self, snapshot):
        self.snapshot = snapshot
        self.updates = []

    def get(self, transaction=None):
        return self.snapshot

    def update(self, values):
        self.updates.append(values)


class InMemoryReference:
    def __init__(self, collection, device_id):
        self.collection = collection
        self.id = device_id

    def get(self, transaction=None):
        record = self.collection.records.get(self.id)
        if record is None:
            snapshot = FakeDeviceSnapshot(self.id, {}, self)
            snapshot.exists = False
            return snapshot
        return FakeDeviceSnapshot(self.id, record, self)

    def set(self, values):
        self.collection.records[self.id] = dict(values)

    def update(self, values):
        record = self.collection.records[self.id]
        for key, value in values.items():
            if value is device_auth.firestore.DELETE_FIELD:
                record.pop(key, None)
            else:
                record[key] = value


class InMemoryQuery:
    def __init__(self, collection, filters=None, maximum=None):
        self.collection = collection
        self.filters = filters or []
        self.maximum = maximum

    def where(self, *, filter):
        return InMemoryQuery(self.collection, self.filters + [filter], self.maximum)

    def limit(self, maximum):
        return InMemoryQuery(self.collection, self.filters, maximum)

    def stream(self):
        matches = []
        for device_id, record in self.collection.records.items():
            if all(record.get(item.field_path) == item.value for item in self.filters):
                matches.append(FakeDeviceSnapshot(
                    device_id, record, InMemoryReference(self.collection, device_id)
                ))
        return iter(matches[:self.maximum] if self.maximum is not None else matches)


class InMemoryCollection:
    def __init__(self):
        self.records = {}

    def document(self, device_id):
        return InMemoryReference(self, device_id)

    def where(self, *, filter):
        return InMemoryQuery(self, [filter])


class InMemoryTransaction:
    def update(self, reference, values):
        reference.update(values)


class InMemoryDatabase:
    def __init__(self):
        self.devices = InMemoryCollection()

    def collection(self, name):
        if name != "devices":
            raise AssertionError(f"unexpected collection {name}")
        return self.devices

    def transaction(self):
        return InMemoryTransaction()


class FakeBlob:
    def __init__(self, exists=False, data=b""):
        self._exists = exists
        self._data = data

    def exists(self):
        return self._exists

    def download_as_string(self):
        return self._data


class FakeBucket:
    def __init__(self, blobs):
        self.blobs = blobs

    def blob(self, path):
        return self.blobs.get(path, FakeBlob())


class FakeStorage:
    def __init__(self, bucket):
        self._bucket = bucket

    def bucket(self, _name):
        return self._bucket


class DeviceAuthTests(unittest.TestCase):
    def test_schedule_targets_are_absolute_and_weekly_fits(self):
        saturday = datetime.datetime(2026, 10, 31, 14, 0, tzinfo=datetime.timezone.utc)
        twelve_hour = device_auth._schedule_values({"wakeSchedule": "12h"}, saturday)
        daily = device_auth._schedule_values({"wakeSchedule": "daily"}, saturday)
        weekly = device_auth._schedule_values({"wakeSchedule": "weekly"}, saturday)
        manual = device_auth._schedule_values({"wakeSchedule": "manual"}, saturday)

        self.assertEqual(
            datetime.datetime.fromtimestamp(twelve_hour["nextWakeAt"], datetime.timezone.utc),
            datetime.datetime(2026, 11, 1, 0, 15, tzinfo=datetime.timezone.utc),
        )
        self.assertEqual(
            datetime.datetime.fromtimestamp(daily["nextWakeAt"], datetime.timezone.utc),
            datetime.datetime(2026, 11, 1, 12, 15, tzinfo=datetime.timezone.utc),
        )
        self.assertEqual(
            datetime.datetime.fromtimestamp(weekly["nextWakeAt"], datetime.timezone.utc),
            datetime.datetime(2026, 11, 1, 12, 15, tzinfo=datetime.timezone.utc),
        )
        self.assertIsNone(manual["nextWakeAt"])

    def test_owner_can_update_schedule_and_other_owner_cannot(self):
        database = InMemoryDatabase()
        device_id = "22" * 16
        database.devices.records[device_id] = {
            "userId": "owner",
            "status": "claimed",
        }
        request = FakeRequest(method="POST", body={
            "deviceId": device_id,
            "schedule": "weekly",
        })
        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner"}), \
             patch.object(device_auth.firestore, "transactional", side_effect=lambda fn: fn):
            body, status, _ = device_auth.update_device_schedule(request)
        payload = json.loads(body)
        self.assertEqual(status, 200)
        self.assertEqual(payload["schedule"], "weekly")
        self.assertTrue(payload["pending"])
        self.assertEqual(database.devices.records[device_id]["wakeSchedule"], "weekly")
        self.assertEqual(len(database.devices.records[device_id]["scheduleVersion"]), 32)

        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "other"}), \
             patch.object(device_auth.firestore, "transactional", side_effect=lambda fn: fn):
            _, status, _ = device_auth.update_device_schedule(request)
        self.assertEqual(status, 404)

    def test_register_claim_remove_and_reclaim_workflow(self):
        database = InMemoryDatabase()
        device_id = "22" * 16
        secret = "11" * 32
        token_hash = hashlib.sha256(bytes.fromhex(secret)).hexdigest()
        register = FakeRequest(
            method="POST",
            headers={
                "X-Firmware-Version": "0.1.0",
                "X-Firmware-Build": "ab" * 32,
                "X-Firmware-Idf": "v6.0.2",
            },
            body={
                "deviceId": device_id,
                "tokenHash": token_hash,
                "activationCode": "ABC123",
            },
        )

        with patch.object(device_auth, "_db_client", return_value=database):
            body, status, _ = device_auth.register_device(register)
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["ok"])
        self.assertEqual(database.devices.records[device_id]["status"], "pending")
        self.assertEqual(database.devices.records[device_id]["firmwareVersion"], "0.1.0")
        self.assertEqual(database.devices.records[device_id]["firmwareBuild"], "ab" * 32)

        claim = FakeRequest(method="POST", body={"activationCode": "ABC123"})
        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner-a"}), \
             patch.object(device_auth, "_queue_refresh"), \
             patch.object(device_auth.firestore, "transactional", side_effect=lambda fn: fn):
            body, status, _ = device_auth.claim_device(claim)
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["refreshQueued"])
        self.assertEqual(database.devices.records[device_id]["userId"], "owner-a")
        self.assertEqual(database.devices.records[device_id]["status"], "claimed")
        database.devices.records[device_id]["lastImageHash"] = "ab" * 32

        remove = FakeRequest(method="POST", body={"deviceId": device_id})
        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner-b"}), \
             patch.object(device_auth.firestore, "transactional", side_effect=lambda fn: fn):
            body, status, _ = device_auth.remove_device(remove)
        self.assertEqual(status, 404)
        self.assertEqual(database.devices.records[device_id]["userId"], "owner-a")

        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner-a"}), \
             patch.object(device_auth.firestore, "transactional", side_effect=lambda fn: fn):
            body, status, _ = device_auth.remove_device(remove)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["status"], "pending")
        self.assertNotIn("userId", database.devices.records[device_id])
        self.assertNotIn("lastImageHash", database.devices.records[device_id])
        self.assertEqual(database.devices.records[device_id]["tokenHash"], token_hash)

        poll = FakeRequest(headers={
            "Authorization": f"Bearer {secret}",
            "X-Device-Id": device_id,
        })
        with patch.object(device_auth, "_db_client", return_value=database):
            body, status, _ = device_auth.device_poll(poll)
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["claimed"])

        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner-b"}), \
             patch.object(device_auth, "_queue_refresh"), \
             patch.object(device_auth.firestore, "transactional", side_effect=lambda fn: fn):
            body, status, _ = device_auth.claim_device(claim)
        self.assertEqual(status, 200)
        self.assertEqual(database.devices.records[device_id]["userId"], "owner-b")
        self.assertEqual(database.devices.records[device_id]["status"], "claimed")

    def test_refresh_rejects_queue_failure_without_claiming_success(self):
        request = FakeRequest(
            method="POST",
            headers={"Authorization": "Bearer user-token"},
            body={"imagePath": "users/owner/images/photo.jpg"},
        )
        with patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner"}), \
             patch.object(device_auth, "_begin_refresh_request", return_value=False), \
             patch.object(device_auth, "_update_owner_devices"), \
             patch.object(device_auth, "_queue_refresh", side_effect=OSError("pubsub down")):
            body, status, _ = device_auth.refresh_device(request)
        self.assertEqual(status, 503)
        self.assertIn("could not queue", json.loads(body)["error"])

    def test_refresh_rejects_when_ordering_state_cannot_be_recorded(self):
        request = FakeRequest(
            method="POST",
            headers={"Authorization": "Bearer user-token"},
            body={"imagePath": "users/owner/images/photo.jpg"},
        )
        with patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner"}), \
             patch.object(device_auth, "_begin_refresh_request", side_effect=OSError("firestore down")), \
             patch.object(device_auth, "_queue_refresh") as queue:
            body, status, _ = device_auth.refresh_device(request)
        self.assertEqual(status, 503)
        queue.assert_not_called()

    def test_refresh_reports_replacement_and_passes_request_id_to_worker(self):
        request = FakeRequest(
            method="POST",
            headers={"Authorization": "Bearer user-token"},
            body={"imagePath": "users/owner/images/new-photo.jpg"},
        )
        with patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner"}), \
             patch.object(device_auth, "_begin_refresh_request", return_value=True) as begin, \
             patch.object(device_auth, "_queue_refresh") as queue:
            body, status, _ = device_auth.refresh_device(request)

        payload = json.loads(body)
        self.assertEqual(status, 202)
        self.assertTrue(payload["replaced"])
        self.assertEqual(len(payload["requestId"]), 32)
        begin.assert_called_once_with("owner", "users/owner/images/new-photo.jpg", payload["requestId"])
        queue.assert_called_once_with("owner", "users/owner/images/new-photo.jpg", payload["requestId"])

    def test_poll_records_button_presence_and_display_ack(self):
        secret = "11" * 32
        frame_hash = "ab" * 32
        record = {
            "tokenHash": hashlib.sha256(bytes.fromhex(secret)).hexdigest(),
            "status": "claimed",
            "userId": "owner",
        }
        snapshot = FakeDeviceSnapshot("22" * 16, record)
        reference = FakeReference(snapshot)
        snapshot.reference = reference

        collection = SimpleNamespace(document=lambda _device_id: reference)
        database = SimpleNamespace(collection=lambda name: collection)
        manifest = json.dumps({
            "hash": frame_hash,
            "image": "users/owner/daily/frame.bin",
        }).encode()
        storage = FakeStorage(FakeBucket({
            "users/owner/manifest.json": FakeBlob(True, manifest),
        }))
        request = FakeRequest(headers={
            "Authorization": f"Bearer {secret}",
            "X-Device-Id": "22" * 16,
            "X-Current-Image-Hash": frame_hash,
            "X-Wake-Window": "button",
            "X-Schedule-Version": "schedule-v1",
            "X-Firmware-Version": "0.1.0",
            "X-Firmware-Build": "cd" * 32,
            "X-Firmware-Idf": "v6.0.2",
        })
        record["wakeSchedule"] = "daily"
        record["scheduleVersion"] = "schedule-v1"

        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_gcs_client", return_value=storage):
            body, status, _ = device_auth.device_poll(request)

        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["upToDate"])
        self.assertEqual(json.loads(body)["schedule"], "daily")
        self.assertIsInstance(json.loads(body)["serverTime"], int)
        self.assertIsInstance(json.loads(body)["nextWakeAt"], int)
        self.assertIn("lastSeen", reference.updates[0])
        self.assertIn("awakeUntil", reference.updates[0])
        self.assertEqual(reference.updates[0]["appliedScheduleVersion"], "schedule-v1")
        self.assertEqual(reference.updates[0]["firmwareVersion"], "0.1.0")
        self.assertEqual(reference.updates[0]["firmwareBuild"], "cd" * 32)
        self.assertEqual(reference.updates[-1]["refreshState"], "displayed")
        window = reference.updates[0]["awakeUntil"] - reference.updates[0]["lastSeen"]
        self.assertGreaterEqual(window, datetime.timedelta(minutes=4, seconds=59))

    def test_poll_never_delivers_a_manifest_from_a_replaced_request(self):
        secret = "11" * 32
        record = {
            "tokenHash": hashlib.sha256(bytes.fromhex(secret)).hexdigest(),
            "status": "claimed",
            "userId": "owner",
            "refreshRequestId": "new-request",
        }
        snapshot = FakeDeviceSnapshot("22" * 16, record)
        reference = FakeReference(snapshot)
        snapshot.reference = reference
        collection = SimpleNamespace(document=lambda _device_id: reference)
        database = SimpleNamespace(collection=lambda _name: collection)
        manifest = json.dumps({
            "hash": "ab" * 32,
            "image": "users/owner/daily/old.bin",
            "requestId": "old-request",
        }).encode()
        storage = FakeStorage(FakeBucket({
            "users/owner/manifest.json": FakeBlob(True, manifest),
        }))
        request = FakeRequest(headers={
            "Authorization": f"Bearer {secret}",
            "X-Device-Id": "22" * 16,
        })

        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_gcs_client", return_value=storage):
            body, status, _ = device_auth.device_poll(request)

        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["processing"])
        self.assertNotIn("url", body)

    def test_repeated_button_polls_do_not_extend_the_awake_deadline(self):
        secret = "11" * 32
        frame_hash = "ab" * 32
        existing_deadline = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(minutes=4)
        record = {
            "tokenHash": hashlib.sha256(bytes.fromhex(secret)).hexdigest(),
            "status": "claimed",
            "userId": "owner",
            "awakeUntil": existing_deadline,
        }
        snapshot = FakeDeviceSnapshot("22" * 16, record)
        reference = FakeReference(snapshot)
        snapshot.reference = reference
        collection = SimpleNamespace(document=lambda _device_id: reference)
        database = SimpleNamespace(collection=lambda _name: collection)
        manifest = json.dumps({
            "hash": frame_hash,
            "image": "users/owner/daily/frame.bin",
        }).encode()
        storage = FakeStorage(FakeBucket({
            "users/owner/manifest.json": FakeBlob(True, manifest),
        }))
        request = FakeRequest(headers={
            "Authorization": f"Bearer {secret}",
            "X-Device-Id": "22" * 16,
            "X-Current-Image-Hash": frame_hash,
            "X-Wake-Window": "button",
        })

        with patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_gcs_client", return_value=storage):
            _, status, _ = device_auth.device_poll(request)

        self.assertEqual(status, 200)
        self.assertNotIn("awakeUntil", reference.updates[0])

    def test_device_status_does_not_expose_device_credentials(self):
        now = datetime.datetime.now(datetime.timezone.utc)
        record = {
            "tokenHash": "secret-hash",
            "activationCode": "ABC123",
            "userId": "owner",
            "refreshState": "queued",
            "lastSeen": now,
            "awakeUntil": now + datetime.timedelta(minutes=5),
            "firmwareVersion": "0.1.0",
            "firmwareBuild": "ef" * 32,
            "firmwareIdfVersion": "v6.0.2",
        }
        snapshot = FakeDeviceSnapshot("device-1", record)
        query = SimpleNamespace(stream=lambda: iter([snapshot]))
        collection = SimpleNamespace(where=lambda **_kwargs: query)
        database = SimpleNamespace(collection=lambda _name: collection)
        storage = FakeStorage(FakeBucket({}))
        request = FakeRequest(headers={"Authorization": "Bearer user-token"})

        with patch.object(device_auth, "_verify_firebase_user", return_value={"uid": "owner"}), \
             patch.object(device_auth, "_db_client", return_value=database), \
             patch.object(device_auth, "_gcs_client", return_value=storage):
            body, status, _ = device_auth.device_status(request)

        payload = json.loads(body)
        self.assertEqual(status, 200)
        self.assertTrue(payload["devices"][0]["awake"])
        self.assertEqual(payload["devices"][0]["firmwareVersion"], "0.1.0")
        self.assertEqual(payload["devices"][0]["firmwareBuild"], "ef" * 32)
        self.assertNotIn("tokenHash", body)
        self.assertNotIn("activationCode", body)


if __name__ == "__main__":
    unittest.main()
