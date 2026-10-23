import copy
import csv
import io
import http.client
import json
import sys
import threading
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from urllib.parse import quote, urlencode

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import InvalidDraft, PAGES, Server, Simulator


def encoded(value):
    value = quote(value, safe="")
    return f"{len(value)}:{value}"


def draft(sim):
    layout = sim.data["dashboard"]
    return {
        "action": "layout-save", "ajax": "1",
        "groupsPayload": "".join(encoded(g["name"]) + f'{g["id"]},{g["order"]},{int(g["visible"])},{int(g["collapsed"])},0;' for g in layout["groups"]),
        "payload": "".join(encoded(i["ref"]) + encoded(i["label"]) + f'{i["groupId"]},{i["order"]},{int(i["visible"])},0;' for i in layout["items"]),
        "primaryDevice": layout["primaryDevice"],
        "time": "1", "ethernet": "1", "wifi": "1", "tagHealth": "1",
    }


class StateTests(unittest.TestCase):
    def setUp(self):
        self.sim = Simulator()
        self.sim.reset("normal_site")

    def test_scenarios_and_output(self):
        self.sim.write("/m31/output/do", {"ch": "1", "value": "1"})
        item = next(i for i in self.sim.layout()["items"] if i["ref"] == "LIB.LD01.do1")
        self.assertEqual(item["value"], "ON")
        self.sim.reset("m31_communication_failure")
        before = copy.deepcopy(self.sim.data)
        with self.assertRaises(InvalidDraft):
            self.sim.write("/m31/output/do", {"ch": "1", "value": "1"})
        self.assertEqual(self.sim.data, before)
        self.assertEqual(self.sim.layout()["primary"]["quality"], "BAD")
        self.sim.reset("burner_offline")
        self.assertTrue(all(t["quality"] == "BAD" for t in self.sim.snapshot()["tags"] if t["device"] == "LD02"))
        self.sim.reset("high_process_alarm")
        self.assertEqual(len(self.sim.snapshot()["alarms"]), 1)
        self.sim.reset("normal_site")
        self.assertEqual(self.sim.snapshot()["alarms"], [])
        self.sim.reset("ngc_offline")
        self.assertEqual(len([t for t in self.sim.snapshot()["tags"] if t["device"] == "LD04" and t["quality"] == "BAD"]), 4)
        self.sim.write("/sim/device", {"id": "LD04", "online": "1"})
        self.sim.write("/sim/reset", {})
        self.assertEqual(self.sim.scenario, "jc_first_start")
        self.assertEqual(self.sim.data["devices"], [])

    def test_shared_save_and_atomic_rejection(self):
        params = draft(self.sim)
        params["groupsPayload"] = params["groupsPayload"].replace(encoded("Production"), encoded("Sales & Production"))
        params.pop("tagHealth")
        self.sim.save_layout(params)
        self.assertEqual(self.sim.data["dashboard"]["groups"][0]["name"], "Sales & Production")
        self.assertEqual(self.sim.data["dashboard"]["statusMask"], 7)
        before = copy.deepcopy(self.sim.data)
        params["groupsPayload"] = params["groupsPayload"].replace("1,1,1,0,0;", "1,1,1,0,1;")
        with self.assertRaisesRegex(InvalidDraft, "still contains"):
            self.sim.save_layout(params)
        self.assertEqual(self.sim.data, before)
        # Explicitly reassign all Production items before deleting its group.
        params["payload"] = params["payload"].replace("1,16,1,0;", "3,16,1,0;")
        self.sim.save_layout(params)
        self.assertFalse(any(g["id"] == 1 for g in self.sim.data["dashboard"]["groups"]))

    def test_unassign_preserves_source_and_unavailable_mapping(self):
        params = draft(self.sim)
        params["payload"] = params["payload"].replace("4,1,1,0;", "4,1,1,1;")
        self.sim.save_layout(params)
        self.assertEqual(len(self.sim.data["tags"]), 22)
        self.assertEqual(len(self.sim.data["dashboard"]["items"]), 21)
        self.sim.data["tags"][1]["selected"] = False
        unavailable = next(i for i in self.sim.layout()["items"] if i["ref"] == "LIB.LD01.ai2")
        self.assertFalse(unavailable["available"])
        self.sim.save_layout(draft(self.sim))
        self.sim.data["tags"][1]["selected"] = True
        self.assertTrue(next(i for i in self.sim.layout()["items"] if i["ref"] == "LIB.LD01.ai2")["available"])

    def test_bad_drafts_and_real_writes_refused(self):
        for change in ({"payload": ""}, {"groupsPayload": "broken"}, {"primaryDevice": "192.0.2.31"}, {"payload": "99999:x"}):
            before = copy.deepcopy(self.sim.data)
            params = draft(self.sim)
            params.update(change)
            with self.assertRaises(ValueError):
                self.sim.save_layout(params)
            self.assertEqual(self.sim.data, before)
        for path in ("/firmware", "/format", "/modbus/write"):
            with self.assertRaises(InvalidDraft):
                self.sim.write(path, {})

    def test_persistence_restart_reset_and_write_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state" / "current_state.json"
            sim = Simulator(path)
            sim.write("/sim/scenario", {"scenario": "ngc_offline"})
            sim.write("/sim/device", {"id": "LD04", "online": "1"})
            sim.write("/m31/output/do", {"ch": "1", "value": "1"})
            params = draft(sim)
            params["groupsPayload"] = params["groupsPayload"].replace(encoded("Gas Quality"), encoded("Chromatograph"))
            sim.write("/api/dashboard-layout/action", params)
            restored = Simulator(path)
            self.assertEqual(restored.saved_state(), sim.saved_state())
            before = restored.saved_state()
            disk = path.read_bytes()
            with patch("server.os.replace", side_effect=OSError("simulated disk failure")):
                with self.assertRaises(OSError):
                    restored.write("/sim/reset", {})
            self.assertEqual(restored.saved_state(), before)
            self.assertEqual(path.read_bytes(), disk)
            self.assertFalse(path.with_suffix(".tmp").exists())
            restored.write("/sim/reset", {})
            restarted = Simulator(path)
            self.assertEqual(restarted.scenario, "jc_first_start")
            self.assertEqual(restarted.data["devices"], [])
            self.assertEqual(restarted.data["dashboard"]["groups"], [])

    def test_corrupt_state_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "current_state.json"
            for value in ("invalid json", '{"version":99}', '{"version":1,"scenario":"ngc_offline","devices":{}}'):
                path.write_text(value, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "file retained"):
                    Simulator(path)
                self.assertEqual(path.read_text(encoding="utf-8"), value)


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server(0, state_path=None)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, path, params=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        body = urlencode(params) if params is not None else None
        h = {"Content-Type": "application/x-www-form-urlencoded"} if body is not None else {}
        h.update(headers or {})
        conn.request("POST" if params is not None else "GET", path, body, h)
        response = conn.getresponse()
        result = response.status, response.read(), dict(response.getheaders())
        conn.close()
        return result

    def test_navigation_and_assets(self):
        for path in PAGES:
            status, body, _ = self.request(path)
            self.assertEqual(status, 200, path)
            self.assertIn(b"Simulated Controller", body)
        for path in ("/web/riot.css", "/web/introduction.css", "/web/introduction.js"):
            self.assertEqual(self.request(path)[0], 200)
        status, body, headers = self.request("/web/OAI-LOGO-SMALL.PNG")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "image/png")
        self.assertTrue(body.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(self.request("/web/../server.py")[0], 404)

    def test_api_and_post(self):
        self.assertEqual(self.request("/sim/scenario", {"scenario": "normal_site"})[0], 200)
        for path in ("/api/hmi", "/status", "/api/time", "/tags", "/api/modbus", "/api/library-signals", "/api/dashboard-layout?view=1"):
            status, body, _ = self.request(path)
            self.assertEqual(status, 200, path)
            self.assertIsInstance(json.loads(body), dict)
        self.assertEqual(self.request("/m31/output/do", {"ch": "4", "value": "1"})[0], 200)
        self.assertEqual(self.request("/m31/output/do", {"ch": "9", "value": "1"})[0], 400)
        self.assertEqual(self.request("/sim/scenario", {"scenario": "missing"})[0], 400)
        status, body, _ = self.request("/api/dashboard-layout/action", draft(self.server.sim))
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["ok"])

    def test_cross_origin_refused(self):
        self.assertEqual(self.request("/status", headers={"Host": "example.invalid"})[0], 403)
        self.assertEqual(self.request("/sim/reset", {}, {"Origin": "https://example.invalid"})[0], 403)

    def test_jc_http_walkthrough_and_download(self):
        self.assertEqual(self.request("/sim/reset", {})[0], 200)
        state = json.loads(self.request("/api/sim/state")[1])
        self.assertFalse(state["tags"])
        self.assertEqual(self.request("/sim/jc", {"action": "add-m31"})[0], 200)
        self.assertEqual(self.request("/sim/jc", {"action": "tags-save", "dashboard": "LIB.LD02.ai1",
                                                 "logged": "LIB.LD02.ai1"})[0], 200)
        self.assertEqual(self.request("/sim/jc", {"action": "logger-save", "enabled": "1", "interval": "60"})[0], 200)
        status, body, headers = self.request("/sim/sample.csv")
        self.assertEqual(status, 200)
        self.assertIn("attachment", headers["Content-Disposition"])
        self.assertIn(b"SIMULATED", body)


class JcWalkthroughTests(unittest.TestCase):
    def test_blank_dynamic_clock_sampling_quality_bound_and_reset(self):
        sim = Simulator()
        self.assertEqual(sim.snapshot()["tags"], [])
        self.assertEqual(sim.layout()["items"], [])
        self.assertFalse(sim.read("/api/hmi", {})["m31Online"])
        with self.assertRaises(InvalidDraft):
            sim.write("/sim/jc", {"action": "logger-save", "enabled": "1", "interval": "60"})
        with patch("server.time.monotonic", return_value=sim.started + 2):
            self.assertEqual(sim.clock(), "2026-10-15 12:00:20")
        sim.write("/sim/jc", {"action": "add-m31"})
        self.assertEqual(sim.data["devices"][0]["id"], "LD02")
        with self.assertRaises(InvalidDraft):
            sim.write("/sim/jc", {"action": "add-m31"})
        with patch("server.time.monotonic", return_value=sim.started):
            a = sim.snapshot()["tags"][0]["value"]
        with patch("server.time.monotonic", return_value=sim.started + 3):
            self.assertNotEqual(sim.snapshot()["tags"][0]["value"], a)
        params = {"action": "tags-save", "dashboard": "LIB.LD02.ai1", "logged": "LIB.LD02.ai1"}
        sim.write("/sim/jc", params)
        sim.write("/sim/jc", {"action": "logger-save", "enabled": "1", "interval": "60"})
        sim.snapshot()
        self.assertEqual(len(sim.samples), 1)
        sim.snapshot()
        self.assertEqual(len(sim.samples), 1)
        sim.write("/sim/device", {"id": "LD02", "online": "0"})
        with patch("server.time.monotonic", return_value=sim.started + 7):
            rows = list(csv.reader(io.StringIO(sim.sample_csv())))
        self.assertEqual(rows[-1][-3:], ["mA", "BAD", "SIMULATED"])
        self.assertEqual(rows[-1][4], "")
        for second in range(14, 2100, 7):
            with patch("server.time.monotonic", return_value=sim.started + second):
                sim.snapshot()
        self.assertEqual(len(sim.samples), 240)
        sim.write("/sim/jc", {"action": "tags-save", "dashboard": "", "logged": ""})
        count = len(sim.samples)
        sim.next_sample = 0
        sim.snapshot()
        self.assertEqual(len(sim.samples), count)
        sim.write("/sim/reset", {})
        self.assertEqual(sim.snapshot()["tags"], [])
        self.assertEqual(sim.samples, [])
        self.assertFalse(sim.walkthrough["logging"])
        self.assertEqual(sim.walkthrough["interval"], 60)

    def test_local_restart_and_rejected_save_preserve_state(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "local.json"
            sim = Simulator(path)
            sim.write("/sim/jc", {"action": "add-m31"})
            sim.write("/sim/jc", {"action": "tags-save", "dashboard": "LIB.LD02.ai1", "logged": "LIB.LD02.ai1"})
            restored = Simulator(path)
            self.assertEqual(restored.saved_state(), sim.saved_state())
            before = restored.saved_state()
            with self.assertRaises(InvalidDraft):
                restored.write("/sim/jc", {"action": "tags-save", "dashboard": "unknown"})
            self.assertEqual(restored.saved_state(), before)
            with patch("server.os.replace", side_effect=OSError("disk unavailable")):
                with self.assertRaises(OSError):
                    restored.write("/sim/reset", {})
            self.assertEqual(restored.saved_state(), before)


if __name__ == "__main__":
    unittest.main()
