import copy
import http.client
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlencode

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import Server, PROTOTYPE_PAGES
from tag_prototype import TagPrototype, PrototypeError


def tag_draft(sim, ident="P000002"):
    return {"revision": sim.data["revision"], "action": "tag",
            "tag": copy.deepcopy(sim.tag(sim.data, ident))}


def select_rosemount(sim):
    d = sim.device(sim.data, "D000006")
    return sim.write({"revision": sim.data["revision"], "action": "device", "device_id": d["id"],
                      "name": d["name"], "state": "online",
                      "selected": ["dp", "pressure", "temperature"]})


def set_device(sim, ident, state, confirm=""):
    d = sim.device(sim.data, ident)
    return sim.write({"revision": sim.data["revision"], "action": "device", "device_id": ident,
                      "name": d["name"], "state": state,
                      "selected": [p["id"] for p in d["points"] if p["selected"]], "confirm": confirm})


class PrototypeModelTests(unittest.TestCase):
    def setUp(self):
        self.sim = TagPrototype()

    def test_original_devices_and_ngc_preserved(self):
        data = self.sim.snapshot()
        self.assertEqual(len(data["devices"]), 6)
        ngc = self.sim.device(data, "D000004")
        self.assertEqual(ngc["profile"], "abb_ngc8206")
        self.assertEqual(len(ngc["points"]), 4)
        self.assertEqual(len(self.sim.device(data, "D000001")["points"]), 12)
        self.assertEqual({t["live"]["quality"] for t in data["tags"]}, {"GOOD"})

    def test_rename_history_alarm_and_usage_preserve_identity(self):
        payload = tag_draft(self.sim)
        payload["tag"]["name"] = "Sales Pressure Renamed"
        payload["tag"]["history"]["sample_sec"] = 5
        payload["tag"]["alarms"]["levels"]["H"] = {"enabled": True, "threshold": 800}
        payload["tag"]["usage"].update(mqtt=True, rule=True)
        before_binding = copy.deepcopy(payload["tag"]["binding"])
        result = self.sim.write(payload)
        tag = self.sim.tag(result, "P000002")
        self.assertEqual(tag["id"], "P000002")
        self.assertEqual(tag["binding"], before_binding)
        self.assertEqual(tag["binding_revision"], 1)
        self.assertTrue(tag["usage"]["mqtt"])
        self.assertEqual(tag["usage"]["flow_role"], "static")
        self.assertEqual(tag["history"]["sample_sec"], 5)
        self.assertEqual(result["events"][-1]["kind"], "rename")

    def test_save_restart_and_original_state_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            original = Path(tmp) / "current_state.json"
            original.write_text("existing session stays untouched")
            path = Path(tmp) / "tag_prototype_v1.json"
            sim = TagPrototype(path)
            payload = tag_draft(sim)
            payload["tag"]["name"] = "Saved Pressure"
            sim.write(payload)
            restored = TagPrototype(path)
            self.assertEqual(restored.data, sim.data)
            self.assertEqual(original.read_text(), "existing session stays untouched")
            self.assertEqual(restored.data["version"], 1)

    def test_replacement_continuity_and_audit(self):
        select_rosemount(self.sim)
        before = copy.deepcopy(self.sim.data["tags"])
        preview = self.sim.replacement_preview("D000006")
        self.assertTrue(preview["compatible"])
        self.assertEqual(len(preview["rows"]), 3)
        result = self.sim.write({"action": "replace", "revision": preview["revision"],
                                 "target_id": "D000006", "confirm": "REPLACE"})
        for previous in before[:3]:
            new = self.sim.tag(result, previous["id"])
            for field in ("id", "name", "history", "alarms", "usage"):
                self.assertEqual(new[field], previous[field])
            self.assertEqual(new["binding"]["device_id"], "D000006")
            self.assertEqual(new["binding_revision"], 2)
        events = [e for e in result["events"] if e["kind"] == "binding"]
        self.assertEqual(len(events), 3)
        self.assertEqual(events[0]["old"]["device_id"], "D000005")
        self.assertEqual(events[0]["new"]["device_id"], "D000006")

    def test_replacement_save_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p0.json"
            sim = TagPrototype(path)
            select_rosemount(sim)
            sim.write({"action": "replace", "revision": sim.data["revision"],
                       "target_id": "D000006", "confirm": "REPLACE"})
            self.assertEqual(TagPrototype(path).data, sim.data)

    def test_incompatible_replacement_is_atomic(self):
        preview = self.sim.replacement_preview("D000006")
        self.assertFalse(preview["compatible"])
        before = copy.deepcopy(self.sim.data)
        with self.assertRaisesRegex(PrototypeError, "incompatible"):
            self.sim.write({"action": "replace", "revision": preview["revision"],
                            "target_id": "D000006", "confirm": "REPLACE"})
        self.assertEqual(before, self.sim.data)
        select_rosemount(self.sim)
        self.sim.device(self.sim.data, "D000006")["points"][1]["reference"] = "absolute"
        self.assertFalse(self.sim.replacement_preview("D000006")["compatible"])

    def test_incompatible_binding_rejected_and_prior_retained(self):
        payload = tag_draft(self.sim)
        payload["tag"]["binding"] = {"device_id": "D000005", "point_id": "temperature", "conversion": None}
        before = copy.deepcopy(self.sim.data)
        with self.assertRaisesRegex(PrototypeError, "Binding rejected"):
            self.sim.write(payload)
        self.assertEqual(self.sim.data, before)

    def test_device_slot_reuse_cannot_rebind(self):
        self.sim.snapshot()
        set_device(self.sim, "D000005", "missing", "DELETE")
        before_ids = [t["id"] for t in self.sim.data["tags"]]
        self.sim.write({"action": "device-create", "revision": self.sim.data["revision"],
                        "profile": "p0_imv25", "slot": "LD05", "name": "New IMV25 in reused slot"})
        new = self.sim.data["devices"][-1]
        self.assertEqual(new["id"], "D000007")
        self.assertEqual(new["slot"], "LD05")
        for t in self.sim.snapshot()["tags"][:3]:
            self.assertEqual(t["binding"]["device_id"], "D000005")
            self.assertEqual(t["live"]["quality"], "UNAVAILABLE")
        self.assertEqual(before_ids, [t["id"] for t in self.sim.data["tags"]])
        with self.assertRaisesRegex(PrototypeError, "Recreate"):
            set_device(self.sim, "D000005", "online")

    def test_disconnected_disabled_missing_incompatible(self):
        self.sim.snapshot()
        for status, quality in [("disconnected", "BAD"), ("disabled", "DISABLED"), ("incompatible", "INCOMPATIBLE")]:
            result = set_device(self.sim, "D000005", status)
            tag = self.sim.tag(result, "P000002")
            self.assertEqual(tag["live"]["quality"], quality)
            self.assertEqual(tag["live"]["value"], 620.0)
        result = set_device(self.sim, "D000005", "missing", "DELETE")
        self.assertEqual(self.sim.tag(result, "P000002")["live"]["quality"], "UNAVAILABLE")

    def test_selection_prevents_deselecting_bound_point(self):
        d = self.sim.device(self.sim.data, "D000005")
        with self.assertRaisesRegex(PrototypeError, "Unbind"):
            self.sim.write({"revision": self.sim.data["revision"], "action": "device",
                            "device_id": d["id"], "name": d["name"], "state": "online", "selected": []})

    def test_m31_conversion_and_tag_creation(self):
        casing = self.sim.tag(self.sim.snapshot(), "P000004")
        self.assertAlmostEqual(casing["live"]["value"], 425.0)
        payload = tag_draft(self.sim, "P000004")
        payload["action"] = "tag-create"
        payload["tag"]["name"] = "New Casing Alias"
        self.sim.write(payload)
        self.assertEqual(self.sim.data["tags"][-1]["id"], "P000009")
        self.assertEqual(self.sim.data["next_tag"], 10)

    def test_new_generation_does_not_retain_old_sample(self):
        self.sim.snapshot()
        select_rosemount(self.sim)
        set_device(self.sim, "D000006", "disconnected")
        payload = tag_draft(self.sim)
        payload["tag"]["binding"] = {"device_id": "D000006", "point_id": "pressure", "conversion": None}
        result = self.sim.write(payload)
        self.assertIsNone(self.sim.tag(result, "P000002")["live"]["value"])

    def test_stale_revision_and_flow_role_conflict(self):
        payload = tag_draft(self.sim)
        payload["revision"] -= 1
        with self.assertRaisesRegex(PrototypeError, "changed elsewhere"):
            self.sim.write(payload)
        payload = tag_draft(self.sim, "P000004")
        payload["tag"]["usage"]["flow_role"] = "static"
        with self.assertRaisesRegex(PrototypeError, "already assigned"):
            self.sim.write(payload)

    def test_bad_alarm_and_history_validation(self):
        payload = tag_draft(self.sim)
        payload["tag"]["alarms"]["levels"]["H"] = {"enabled": True, "threshold": 800}
        payload["tag"]["alarms"]["levels"]["HH"] = {"enabled": True, "threshold": 700}
        with self.assertRaisesRegex(PrototypeError, "LL <= L"):
            self.sim.write(payload)
        payload = tag_draft(self.sim)
        payload["tag"]["history"]["sample_sec"] = 0
        with self.assertRaisesRegex(PrototypeError, "interval"):
            self.sim.write(payload)

    def test_failed_save_preserves_memory_disk_and_submitted_draft(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p0.json"
            sim = TagPrototype(path)
            sim.write(tag_draft(sim))
            committed, disk = copy.deepcopy(sim.data), path.read_bytes()
            payload = tag_draft(sim)
            payload["tag"]["name"] = "Unsaved Pressure"
            submitted = copy.deepcopy(payload)
            with patch("tag_prototype.os.replace", side_effect=OSError("disk failure")):
                with self.assertRaises(OSError):
                    sim.write(payload)
            self.assertEqual(sim.data, committed)
            self.assertEqual(path.read_bytes(), disk)
            self.assertEqual(payload, submitted)
            self.assertFalse(path.with_suffix(".tmp").exists())
            payload["simulate_failure"] = True
            with self.assertRaisesRegex(PrototypeError, "Simulated save failure"):
                sim.write(payload)
            self.assertEqual(sim.data, committed)

    def test_corrupt_state_and_reused_counters_retained(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p0.json"
            for data in [{"version": 99}, {**self.sim.data, "next_device": 1}]:
                raw = json.dumps(data)
                path.write_text(raw)
                with self.assertRaisesRegex(PrototypeError, "file retained"):
                    TagPrototype(path)
                self.assertEqual(path.read_text(), raw)


class PrototypeHttpTests(unittest.TestCase):
    def setUp(self):
        self.server = Server(0, state_path=None, prototype_state_path=None)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, path, draft=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        body = urlencode({"draft": json.dumps(draft)}) if draft is not None else None
        h = {"Content-Type": "application/x-www-form-urlencoded"} if body is not None else {}
        h.update(headers or {})
        conn.request("POST" if draft is not None else "GET", path, body, h)
        response = conn.getresponse()
        result = response.status, response.read()
        conn.close()
        return result

    def test_prototype_routes_assets_and_original_untouched(self):
        before = self.server.sim.saved_state()
        for path in PROTOTYPE_PAGES:
            status, body = self.request(path)
            self.assertEqual(status, 200)
            self.assertIn(b"/web/tag-prototype.js", body)
            self.assertNotIn(b"/web/introduction.js", body)
        for asset in ("tag-prototype.js", "tag-prototype.css"):
            self.assertEqual(self.request("/web/" + asset)[0], 200)
        self.assertEqual(self.request("/api/prototype/state")[0], 200)
        sim = self.server.prototype
        payload = tag_draft(sim)
        payload["tag"]["name"] = "P0 only"
        self.assertEqual(self.request("/api/prototype/save", payload)[0], 200)
        self.assertEqual(self.server.sim.saved_state(), before)

    def test_failure_and_invalid_actions_surface_errors(self):
        self.request("/api/prototype/state")
        sim = self.server.prototype
        payload = tag_draft(sim)
        payload["simulate_failure"] = True
        status, body = self.request("/api/prototype/save", payload)
        self.assertEqual(status, 400)
        self.assertIn(b"retained", body)
        with patch.object(sim, "persist", side_effect=OSError("test disk failure")):
            payload.pop("simulate_failure")
            self.assertEqual(self.request("/api/prototype/save", payload)[0], 500)
        for draft in ({}, {"revision": sim.data["revision"], "action": "modbus/write"}, {"revision": sim.data["revision"], "action": "tag", "tag": {}}):
            self.assertEqual(self.request("/api/prototype/save", draft)[0], 400)
        self.assertEqual(self.request("/api/prototype/state", headers={"Host": "example.invalid"})[0], 403)

    def test_original_state_path_is_refused(self):
        with self.assertRaisesRegex(ValueError, "must not overwrite"):
            Server(0, state_path=None, prototype_state_path=Path(__file__).parents[1] / "state" / "current_state.json")
        with tempfile.TemporaryDirectory() as tmp:
            original = Path(tmp) / "custom_session.json"
            with self.assertRaisesRegex(ValueError, "must not overwrite"):
                Server(0, state_path=original, prototype_state_path=original)
            self.assertFalse(original.exists())


if __name__ == "__main__":
    unittest.main()
