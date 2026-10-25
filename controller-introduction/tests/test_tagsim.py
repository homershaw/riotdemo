import http.client
import json
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from urllib.parse import urlencode

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import Server
from tagsim import TAG_LIMIT, SimError, TagSim


class ModelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "tc.json"
        self.sim = TagSim(self.path)

    def tearDown(self):
        self.temp.cleanup()

    def named(self, name):
        return self.sim.find_name(name)

    def test_owner_creates_and_removes_its_tags(self):
        m31 = self.sim.add_owner({"kind": "m31", "ai": 8, "ao": 4, "di": 4, "do": 4})
        self.assertEqual(len(self.sim.tags_of(m31["id"])), 20)
        ai1 = self.named("M31.AI1")
        self.assertEqual((ai1["raw"], ai1["eu"], ai1["units"]), ([4.0, 20.0], [0.0, 100.0], "%"))
        self.sim.remove_owner(m31["id"])
        self.assertEqual(self.sim.cfg["tags"], [])

    def test_ids_never_reused_and_rename_keeps_identity(self):
        m31 = self.sim.add_owner({"kind": "m31", "ai": 2, "ao": 0, "di": 0, "do": 0})
        first = [t["id"] for t in self.sim.tags_of(m31["id"])]
        self.sim.remove_owner(m31["id"])
        again = self.sim.add_owner({"kind": "m31", "ai": 2, "ao": 0, "di": 0, "do": 0})
        self.assertTrue(set(first).isdisjoint(t["id"] for t in self.sim.tags_of(again["id"])))
        tag = self.sim.tags_of(again["id"])[0]
        self.sim.save_tags([{"id": tag["id"], "name": "TUBING_PRESS"}])
        self.assertEqual(self.sim.tag(tag["id"])["name"], "TUBING_PRESS")

    def test_256_limit_refuses_whole_owner(self):
        self.sim.load_example("hub")
        used = len(self.sim.cfg["tags"])
        self.assertLessEqual(used, TAG_LIMIT)
        with self.assertRaises(SimError):
            self.sim.action({"op": "addOwner", "params": {"kind": "modbus", "profile": "rtu40"}})
        self.assertEqual(len(self.sim.cfg["tags"]), used)

    def test_remove_preview_lists_uses_and_marks_them_removed(self):
        self.sim.load_example("plunger")
        m31 = next(o for o in self.sim.cfg["owners"] if o["kind"] == "m31")
        preview = self.sim.remove_preview(m31["id"])
        uses = {t["name"]: t["uses"] for t in preview["tags"]}
        self.assertIn("OPT tubing pressure", uses["TUBING_PRESS"])
        self.sim.remove_owner(m31["id"])
        opt = next(o for o in self.sim.cfg["owners"] if o["kind"] == "feature")
        self.assertEqual(opt["inputs"]["tubing"], {"removed": "TUBING_PRESS"})
        self.sim.tick()   # removed inputs must not crash the scan

    def test_shrinking_a_rack_asks_before_removing_used_tags(self):
        self.sim.load_example("plunger")
        m31 = next(o for o in self.sim.cfg["owners"] if o["kind"] == "m31")
        result = self.sim.change_owner(m31["id"], {"ai": 2})
        self.assertTrue(result["needsConfirm"])
        self.assertEqual(m31["ai"], 8)
        self.assertTrue(self.sim.change_owner(m31["id"], {"ai": 2}, confirm=True)["ok"])
        self.assertIsNone(self.named("LINE_PRESS"))

    def test_replace_device_keeps_ids_and_uses(self):
        dev = self.sim.add_owner({"kind": "modbus", "profile": "imv25", "name": "SALES"})
        flow = self.sim.add_owner({"kind": "flow"})
        ids = {t["point"]: t["id"] for t in self.sim.tags_of(dev["id"])}
        self.sim.change_owner(flow["id"], {"inputs": {"dp": ids["DP"], "sp": ids["SP"], "pt": ids["PT"]}})
        self.sim.replace_owner(dev["id"], "rosemount3095", {str(ids["DP"]): "DP", str(ids["SP"]): "SP", str(ids["PT"]): "PT"})
        self.assertEqual(dev["profile"], "rosemount3095")
        self.assertEqual(flow["inputs"], {"dp": ids["DP"], "sp": ids["SP"], "pt": ids["PT"]})
        self.assertEqual(self.sim.tag(ids["DP"])["name"], "SALES.DP")

    def test_tag_validation(self):
        self.sim.add_owner({"kind": "m31", "ai": 2, "ao": 0, "di": 1, "do": 0})
        a1, a2, d1 = self.sim.cfg["tags"]
        for rows in ([{"id": a1["id"], "name": "M31.AI2"}], [{"id": a1["id"], "eu": [5, 5]}],
                     [{"id": d1["id"], "alarms": {"h": 1}}], [{"id": a1["id"], "modbus": 40001}, {"id": a2["id"], "modbus": 40002}],
                     [{"id": a1["id"], "name": "1BAD"}]):
            with self.assertRaises(SimError):
                self.sim.action({"op": "saveTags", "rows": rows})
        self.assertEqual(self.sim.tag(a1["id"])["name"], "M31.AI1")

    def test_calculation_formula_is_arithmetic_only_and_follows_renames(self):
        self.sim.add_owner({"kind": "manual", "name": "A"})
        with self.assertRaises(SimError):
            self.sim.add_owner({"kind": "calc", "name": "X", "formula": "__import__('os')"})
        with self.assertRaises(SimError):
            self.sim.add_owner({"kind": "calc", "name": "X", "formula": "MISSING + 1"})
        calc = self.sim.add_owner({"kind": "calc", "name": "TWICE", "formula": "A * 2"})
        a = self.named("A")
        self.sim.set_value(a["id"], 21)
        self.sim.save_tags([{"id": a["id"], "name": "B"}])
        self.assertEqual(calc["formula"], "B * 2")
        self.sim.last_tick -= 0.2
        self.sim.tick()
        self.assertEqual(self.sim.live[self.named("TWICE")["id"]], 42.0)

    def test_outputs_have_one_writer(self):
        self.sim.load_example("plunger")
        valve = self.named("WELL_VALVE")
        with self.assertRaises(SimError):
            self.sim.set_value(valve["id"], 1)
        pid = self.sim.add_owner({"kind": "pid"})
        with self.assertRaises(SimError):
            self.sim.change_owner(pid["id"], {"out": self.named("CHOKE_POS")["id"]})

    def test_pid_drives_simulated_level_to_setpoint(self):
        self.sim.load_example("plunger")
        for _ in range(40):
            self.sim.last_tick -= 2.0
            self.sim.tick()
        level, _ = self.sim.eng(self.named("SEP_LEVEL"))
        self.assertAlmostEqual(level, 50.0, delta=3.0)

    def advance(self, steps, seconds=2.0):
        for _ in range(steps):
            self.sim.last_tick -= seconds
            self.sim.tick()

    def test_alarms_are_high_and_low_only(self):
        self.sim.add_owner({"kind": "m31", "ai": 1, "ao": 0, "di": 0, "do": 0})
        tag = self.sim.cfg["tags"][0]
        self.sim.save_tags([{"id": tag["id"], "alarms": {"hh": 99, "h": 80, "l": 20, "ll": 1}}])
        self.assertEqual(self.sim.tag(tag["id"])["alarms"], {"h": 80.0, "l": 20.0})
        with self.assertRaises(SimError):
            self.sim.action({"op": "saveTags", "rows": [{"id": tag["id"], "alarms": {"h": 10, "l": 20}}]})
        self.assertEqual(self.sim.alarm_state(self.sim.tag(tag["id"]), 85.0), "HIGH")
        self.assertEqual(self.sim.alarm_state(self.sim.tag(tag["id"]), 10.0), "LOW")
        self.assertEqual(self.sim.alarm_state(self.sim.tag(tag["id"]), 50.0), "")

    def test_override_takes_and_returns_control_by_low_select(self):
        self.sim.load_example("plunger")
        active, ovr_sp = self.named("PID1.OVR_ACTIVE")["id"], self.named("PID1.OVR_SP")["id"]
        self.advance(60)
        self.assertEqual(self.sim.live[active], 0)
        self.sim.set_value(ovr_sp, 4000)
        self.advance(60)
        self.assertEqual(self.sim.live[active], 1)
        self.assertAlmostEqual(self.sim.eng(self.named("LINE_PRESS"))[0], 4000, delta=150)
        self.sim.set_value(ovr_sp, 6000)
        self.advance(80)
        self.assertEqual(self.sim.live[active], 0)
        self.assertAlmostEqual(self.sim.eng(self.named("SEP_LEVEL"))[0], 50, delta=3)

    def test_manual_to_auto_is_bumpless_and_auto_refuses_manual_output(self):
        self.sim.load_example("plunger")
        auto, out = self.named("PID1.AUTO")["id"], self.named("PID1.OUT")["id"]
        self.advance(60)
        with self.assertRaises(SimError):
            self.sim.set_value(out, 20)
        self.sim.set_value(auto, 0)
        self.sim.set_value(out, 20)
        self.advance(5)
        self.sim.set_value(auto, 1)
        self.sim.last_tick -= 0.05
        self.sim.tick()
        self.assertAlmostEqual(self.sim.live[out], 20, delta=1.5)

    def test_deadband_holds_output(self):
        self.sim.load_example("plunger")
        pid = next(o for o in self.sim.cfg["owners"] if o["kind"] == "pid")
        self.advance(60)
        self.sim.change_owner(pid["id"], {"deadband": 5})
        out = self.named("PID1.OUT")["id"]
        before = self.sim.live[out]
        self.sim.set_value(self.named("PID1.SP")["id"], 52)   # error 2 % inside the 5 % deadband
        self.advance(5)
        self.assertAlmostEqual(self.sim.live[out], before, delta=0.5)

    def test_persistence_round_trip(self):
        self.sim.action({"op": "example", "name": "plunger"})
        reloaded = TagSim(self.path)
        self.assertEqual([t["name"] for t in reloaded.cfg["tags"]], [t["name"] for t in self.sim.cfg["tags"]])
        self.assertEqual(reloaded.snapshot()["measure"]["tags"], len(self.sim.cfg["tags"]))

    def test_logger_rows_use_tag_id(self):
        self.sim.load_example("plunger")
        self.sim.action({"op": "logger", "enabled": True, "interval": 10})
        self.sim.last_tick -= 1.0
        self.sim.tick()
        rows = list(self.sim.csv().splitlines())
        self.assertEqual(rows[0], "UtcTime,TagId,Name,Value,Units,Quality,DataSource")
        self.assertTrue(all(r.endswith("SIMULATED") for r in rows[1:]))
        self.assertGreater(len(rows), 5)


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        base = Path(self.temp.name)
        self.server = Server(0, state_path=None, prototype_state_path=base / "p0.json", tagsim_state_path=base / "tc.json")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.temp.cleanup()

    def request(self, method, path, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        headers = {"Host": f"127.0.0.1:{self.port}"}
        if body is not None:
            body = urlencode({"json": json.dumps(body)})
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        conn.request(method, path, body=body, headers=headers)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        return r.status, data

    def test_pages_state_and_actions(self):
        for path in ("/tc", "/tc/tags", "/tc/devices", "/tc/owner", "/tc/flow", "/tc/features", "/tc/logger", "/tc/maintenance"):
            status, body = self.request("GET", path)
            self.assertEqual(status, 200, path)
            self.assertIn(b"/web/tagsim.js", body)
        status, _ = self.request("POST", "/api/tc/action", {"op": "example", "name": "plunger"})
        self.assertEqual(status, 200)
        status, body = self.request("GET", "/api/tc/state")
        state = json.loads(body)
        self.assertEqual(state["measure"]["tagLimit"], 256)
        self.assertGreater(state["measure"]["tags"], 30)
        status, body = self.request("POST", "/api/tc/action", {"op": "addOwner", "params": {"kind": "nope"}})
        self.assertEqual(status, 400)
        status, body = self.request("GET", "/tc/log.csv")
        self.assertEqual(status, 200)


if __name__ == "__main__":
    unittest.main()
