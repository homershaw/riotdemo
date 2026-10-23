"""Loopback-only RIOT introduction with local JSON state; no field-device client."""
import argparse
import copy
import csv
import io
import json
import mimetypes
import math
import os
import threading
import time
import webbrowser
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit
from tag_prototype import PrototypeError, TagPrototype

HERE = Path(__file__).resolve().parent
PAGES = {"/", "/flow", "/rules", "/alarms", "/history", "/setup", "/device-library",
         "/devices", "/device", "/tag-manager", "/datalog", "/dashboard-layout", "/api-reference", "/maintenance"}
PROTOTYPE_PAGES = {"/prototype/tags", "/prototype/tag", "/prototype/devices",
                   "/prototype/device", "/prototype/library", "/prototype/replacement"}


class InvalidDraft(ValueError):
    pass


def integer(value, low, high):
    text = str(value)
    if not text.isascii() or not text.isdigit() or not low <= int(text) <= high:
        raise InvalidDraft(f"Expected integer {low}..{high}")
    return int(text)


def text(value, limit):
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > limit or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise InvalidDraft("Invalid display name")
    return value


def decode_rows(payload, strings, numbers):
    """Decode the current Nano configuration action's length-delimited transport."""
    rows, pos = [], 0
    while pos < len(payload):
        row = []
        for _ in range(strings):
            colon = payload.find(":", pos)
            if colon < 0:
                raise InvalidDraft("Invalid encoded field")
            size = integer(payload[pos:colon], 0, 4096)
            pos = colon + 1
            if size > len(payload) - pos:
                raise InvalidDraft("Incomplete encoded field")
            row.append(unquote(payload[pos:pos + size], errors="strict"))
            pos += size
        semi = payload.find(";", pos)
        if semi < 0:
            raise InvalidDraft("Incomplete row")
        fields = payload[pos:semi].split(",")
        if len(fields) != numbers:
            raise InvalidDraft("Invalid row fields")
        row.extend(integer(field, 0, 255) for field in fields)
        rows.append(row)
        pos = semi + 1
    return rows


class Simulator:
    def __init__(self, state_path=None):
        self.state_path = Path(state_path) if state_path is not None else None
        self.lock = threading.RLock()
        self.scenarios = {}
        for path in sorted((HERE / "scenarios").glob("*.json")):
            data = json.loads(path.read_text(encoding="utf-8"))
            if data["id"] in self.scenarios:
                raise ValueError("Duplicate scenario ID")
            self.scenarios[data["id"]] = data
        self.reset("jc_first_start")
        if self.state_path is not None:
            if self.state_path.exists():
                try:
                    self.restore(json.loads(self.state_path.read_text(encoding="utf-8")))
                except (ValueError, KeyError, TypeError, UnicodeError) as exc:
                    raise ValueError(f"Saved simulator state is invalid; file retained at {self.state_path}: {exc}") from exc
            else:
                self.persist()

    def saved_state(self):
        return {"version": 1, "scenario": self.scenario,
                "devices": {d["id"]: d["online"] for d in self.data["devices"]},
                "values": {t["ref"]: t["value"] for t in self.data["tags"]},
                "dashboard": copy.deepcopy(self.data["dashboard"]),
                "events": self.events[-30:], "walkthrough": copy.deepcopy(self.walkthrough)}

    def persist(self):
        if self.state_path is None:
            return
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.state_path.with_suffix(".tmp")
        try:
            with temporary.open("w", encoding="utf-8") as out:
                json.dump(self.saved_state(), out, indent=2, allow_nan=False)
                out.write("\n")
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, self.state_path)
        finally:
            if temporary.exists():
                temporary.unlink()

    def restore(self, saved):
        if not isinstance(saved, dict) or saved.get("version") != 1:
            raise InvalidDraft("Unsupported simulator state version")
        self.reset(saved["scenario"])
        walkthrough = saved.get("walkthrough", self.walkthrough)
        if not isinstance(walkthrough, dict) or set(walkthrough) != set(self.walkthrough):
            raise InvalidDraft("Invalid JC walkthrough state")
        if any(type(walkthrough[k]) is not bool for k in ("installed", "logging")):
            raise InvalidDraft("Invalid JC walkthrough flags")
        integer(walkthrough["interval"], 1, 300)
        if not isinstance(walkthrough["logged"], list) or len(set(walkthrough["logged"])) != len(walkthrough["logged"]):
            raise InvalidDraft("Invalid JC Logger selections")
        self.walkthrough = copy.deepcopy(walkthrough)
        if self.scenario == "jc_first_start" and walkthrough["installed"]:
            self.install_m31()
        if any(ref not in {t["ref"] for t in self.data["tags"]} for ref in walkthrough["logged"]):
            raise InvalidDraft("Unknown JC Logger selection")
        devices, values = saved["devices"], saved["values"]
        if not isinstance(devices, dict) or set(devices) != {d["id"] for d in self.data["devices"]}:
            raise InvalidDraft("Saved Device set does not match this introduction")
        if not isinstance(values, dict) or set(values) != {t["ref"] for t in self.data["tags"]}:
            raise InvalidDraft("Saved Tag set does not match this introduction")
        for device in self.data["devices"]:
            if type(devices[device["id"]]) is not bool:
                raise InvalidDraft("Invalid saved Device state")
            device["online"] = devices[device["id"]]
        for tag in self.data["tags"]:
            value = values[tag["ref"]]
            if type(value) is not type(tag["value"]) or (isinstance(value, float) and not math.isfinite(value)):
                raise InvalidDraft("Invalid saved Tag value")
            tag["value"] = value
        layout = saved["dashboard"]
        if not isinstance(layout, dict) or not isinstance(layout.get("groups"), list) or not isinstance(layout.get("items"), list):
            raise InvalidDraft("Invalid saved Dashboard layout")
        if len(layout["groups"]) > 12 or len(layout["items"]) > 64:
            raise InvalidDraft("Saved Dashboard exceeds limits")
        group_ids, refs = set(), set()
        for group in layout["groups"]:
            ident = integer(group["id"], 1, 255)
            integer(group["order"], 1, 255)
            text(group["name"], 24)
            if ident in group_ids or type(group["visible"]) is not bool or type(group["collapsed"]) is not bool:
                raise InvalidDraft("Invalid saved Dashboard group")
            group_ids.add(ident)
        available = {s["ref"] for s in self.sources()}
        for item in layout["items"]:
            integer(item["order"], 1, 255)
            text(item["label"], 35)
            if item["ref"] in refs or item["ref"] not in available or item["groupId"] not in group_ids or type(item["visible"]) is not bool:
                raise InvalidDraft("Invalid saved Dashboard item")
            refs.add(item["ref"])
        integer(layout["statusMask"], 0, 15)
        if layout["primaryDevice"] and layout["primaryDevice"] not in {"DEV." + d["id"] for d in self.data["devices"]}:
            raise InvalidDraft("Invalid saved Primary I/O Device")
        events = saved.get("events", [])
        if not isinstance(events, list) or len(events) > 30:
            raise InvalidDraft("Invalid saved event history")
        for event in events:
            text(event["time"], 40)
            text(event["message"], 200)
        self.data["dashboard"] = copy.deepcopy(layout)
        self.events = copy.deepcopy(events)

    def reset(self, scenario):
        if scenario not in self.scenarios:
            raise InvalidDraft("Unknown scenario")
        self.data = copy.deepcopy(self.scenarios["normal_site"])
        self.scenario = scenario
        self.walkthrough = {"installed": False, "logging": False, "interval": 60, "logged": []}
        self.samples = []
        self.next_sample = 0
        overlay = self.scenarios[scenario]
        for device in self.data["devices"]:
            if device["id"] in overlay.get("offline", []):
                device.update(online=False, bad=8)
        self.data["dashboard"]["items"] = []
        for index, tag in enumerate(self.data["tags"], 1):
            tag["ref"] = f'LIB.{tag["device"]}.{tag["id"]}'
            tag["selected"] = True
            if tag["ref"] in overlay.get("values", {}):
                tag["value"] = overlay["values"][tag["ref"]]
            self.data["dashboard"]["items"].append({
                "ref": tag["ref"], "label": tag["name"], "groupId": tag["group"],
                "order": index, "visible": True,
            })
        self.started = time.monotonic()
        if scenario == "jc_first_start":
            self.data.update(site={"name": "JC-Riot first start", "location": "Local simulation"},
                             profiles=[], devices=[], tags=[],
                             dashboard={"statusMask": 7, "primaryDevice": "", "groups": [], "items": []})
        self.events = [{"time": self.clock(), "message": f"Scenario loaded: {overlay['name']}"}]

    def clock(self):
        if self.scenario == "jc_first_start":
            elapsed = int((time.monotonic() - self.started) * 10)
            return (datetime(2026, 10, 15, 12, tzinfo=timezone.utc) + timedelta(seconds=elapsed)).strftime("%Y-%m-%d %H:%M:%S")
        return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

    def install_m31(self):
        if self.data["devices"]:
            return
        fixture = self.scenarios["normal_site"]
        self.data["profiles"] = [copy.deepcopy(p) for p in fixture["profiles"] if p["id"] == "riot_m31"]
        device = copy.deepcopy(fixture["devices"][0])
        device.update(id="LD02", address="Documentation address 192.0.2.31:502 / Unit 1 (never contacted)")
        self.data["devices"] = [device]
        self.data["tags"] = [dict(copy.deepcopy(t), device="LD02", ref=f'LIB.LD02.{t["id"]}',
                                 selected=True, identity=256 + i)
                             for i, t in enumerate(fixture["tags"]) if t["device"] == "LD01"]
        for tag in self.data["tags"]:
            if tag["id"].startswith("do"):
                tag["value"] = False

    def snapshot(self):
        data = copy.deepcopy(self.data)
        devices = {d["id"]: d for d in data["devices"]}
        for tag in data["tags"]:
            tag["quality"] = "GOOD" if devices[tag["device"]]["online"] else "BAD"
            if self.scenario == "jc_first_start" and tag["id"].startswith("ai") and tag["quality"] == "GOOD":
                phase = (time.monotonic() - self.started) / 4 + int(tag["id"][-1])
                tag["value"] = round(tag["value"] + math.sin(phase) * 1.2, 2)
            elif self.scenario == "jc_first_start" and tag["id"].startswith("di") and tag["quality"] == "GOOD":
                tag["value"] = bool((int((time.monotonic() - self.started) / 5) + int(tag["id"][-1])) % 2)
        data.update(scenario=self.scenario, scenarioName=self.scenarios[self.scenario]["name"],
                    description=self.scenarios[self.scenario]["description"],
                    simulated=True, time=self.clock(), events=self.events[-30:],
                    walkthrough=copy.deepcopy(self.walkthrough))
        temp = next((t for t in data["tags"] if t["id"] == "temperature"), None)
        data["alarms"] = ([{"name": "High Process Temperature", "value": temp["value"],
                           "threshold": 90, "state": "ACTIVE"}]
                          if temp and temp["quality"] == "GOOD" and temp["value"] > 90 else [])
        elapsed = int((time.monotonic() - self.started) * 10)
        if self.scenario == "jc_first_start" and self.walkthrough["logging"] and elapsed >= self.next_sample:
            for tag in data["tags"]:
                if tag["ref"] in self.walkthrough["logged"]:
                    self.samples.append([data["time"], tag["identity"], tag["ref"], tag["name"],
                                         self.value(tag["value"]) if tag["quality"] == "GOOD" else "",
                                         tag["units"], tag["quality"], "SIMULATED"])
            self.samples = self.samples[-240:]
            self.next_sample = elapsed + self.walkthrough["interval"]
        data["sampleCount"] = len(self.samples)
        return data

    def sample_csv(self):
        self.snapshot()
        output = io.StringIO(newline="")
        writer = csv.writer(output)
        writer.writerow(["UtcTime", "Identity", "Ref", "Name", "Value", "Units", "Quality", "DataSource"])
        writer.writerows(self.samples)
        return output.getvalue()

    def sources(self):
        data = self.snapshot()
        result = [{
            "ref": t["ref"], "label": t["name"], "group": t["pool"], "value": self.value(t["value"]),
            "units": t["units"], "quality": t["quality"], "available": t["selected"],
        } for t in data["tags"] if t["selected"]]
        result.extend({
            "ref": "DEV." + d["id"], "label": d["name"], "group": "Devices",
            "value": "ONLINE" if d["online"] else "OFFLINE", "units": "",
            "quality": "GOOD" if d["online"] else "BAD", "available": True,
        } for d in data["devices"])
        return result

    @staticmethod
    def value(value):
        if isinstance(value, bool):
            return "ON" if value else "OFF"
        return str(value)

    def layout(self, view=False):
        layout = copy.deepcopy(self.data["dashboard"])
        sources = self.sources()
        by_ref = {s["ref"]: s for s in sources}
        items = [{**item, **{k: v for k, v in by_ref.get(item["ref"], {}).items() if k != "label"},
                  "available": item["ref"] in by_ref} for item in layout["items"]]
        groups = sorted(layout["groups"], key=lambda g: g["order"])
        for group in groups:
            group["items"] = sorted((i for i in items if i["groupId"] == group["id"] and i["visible"] and i["available"]), key=lambda i: i["order"])
        primary = copy.deepcopy(by_ref.get(layout["primaryDevice"]))
        if primary:
            device = next(d for d in self.data["devices"] if "DEV." + d["id"] == primary["ref"])
            primary.update(hasCounters=True, good=device["good"], bad=device["bad"])
        return {**layout, "ok": True, "custom": True, "store": "SIMULATED_MEMORY", "primary": primary,
                "groups": [g for g in groups if not view or (g["visible"] and g["items"])],
                "items": items, "sources": sources}

    def save_layout(self, params):
        old = self.data["dashboard"]
        action = params.get("action")
        if action == "factory-reset":
            if params.get("confirm") != "RESTORE FACTORY LAYOUT":
                raise InvalidDraft("Confirmation required")
            # Reset layout only, not outputs, scenario, or installed sources.
            fresh = Simulator()
            fresh.reset(self.scenario)
            self.data["dashboard"] = copy.deepcopy(fresh.data["dashboard"])
            return
        if action in ("group-add", "item-add"):
            draft = copy.deepcopy(old)
            if action == "group-add":
                if len(draft["groups"]) >= 12:
                    raise InvalidDraft("Group limit reached")
                ident = max((g["id"] for g in draft["groups"]), default=0) + 1
                integer(ident, 1, 255)
                draft["groups"].append({"id": ident, "name": "New Group", "order": len(draft["groups"]) + 1, "visible": True, "collapsed": False})
            else:
                ref = params.get("ref")
                source = next((s for s in self.sources() if s["ref"] == ref), None)
                gid = integer(params.get("groupId"), 1, 255)
                if not source or not any(g["id"] == gid for g in draft["groups"]) or any(i["ref"] == ref for i in draft["items"]) or len(draft["items"]) >= 64:
                    raise InvalidDraft("Source/group invalid, already assigned, or item limit reached")
                draft["items"].append({"ref": ref, "label": source["label"], "groupId": gid, "order": len(draft["items"]) + 1, "visible": True})
            self.data["dashboard"] = draft
            return
        if action != "layout-save":
            raise InvalidDraft("Unsupported simulated layout action")
        groups, items, seen_groups, seen_items = [], [], set(), set()
        for name, ident, order, visible, collapsed, remove in decode_rows(params.get("groupsPayload", ""), 1, 5):
            if ident in seen_groups or not any(g["id"] == ident for g in old["groups"]):
                raise InvalidDraft("Unknown or duplicate group")
            seen_groups.add(ident)
            text(name, 24)
            integer(order, 1, 255)
            if any(n not in (0, 1) for n in (visible, collapsed, remove)):
                raise InvalidDraft("Invalid group flags")
            if not remove:
                groups.append({"id": ident, "name": name, "order": order, "visible": bool(visible), "collapsed": bool(collapsed)})
        for ref, label, gid, order, visible, remove in decode_rows(params.get("payload", ""), 2, 4):
            if ref in seen_items or not any(i["ref"] == ref for i in old["items"]):
                raise InvalidDraft("Unknown or duplicate item")
            seen_items.add(ref)
            text(label, 35)
            integer(order, 1, 255)
            if visible not in (0, 1) or remove not in (0, 1):
                raise InvalidDraft("Invalid item flags")
            if not remove:
                if not any(g["id"] == gid for g in groups):
                    raise InvalidDraft("Group still contains mapped items; reassign or unassign first")
                items.append({"ref": ref, "label": label, "groupId": gid, "order": order, "visible": bool(visible)})
        if len(seen_groups) != len(old["groups"]) or len(seen_items) != len(old["items"]):
            raise InvalidDraft("Draft must include every group and mapped item")
        primary = params.get("primaryDevice", "")
        if primary and not any(s["ref"] == primary and primary.startswith("DEV.") for s in self.sources()):
            raise InvalidDraft("Invalid Primary I/O Device")
        mask = sum(1 << i for i, name in enumerate(("time", "ethernet", "wifi", "tagHealth")) if params.get(name) == "1")
        self.data["dashboard"] = {"groups": groups, "items": items, "primaryDevice": primary, "statusMask": mask}

    def read(self, path, query):
        data = self.snapshot()
        if path == "/api/sim/state":
            return {"ok": True, **data}
        if path == "/api/sim/scenarios":
            return {"ok": True, "scenarios": [{"id": s["id"], "name": s["name"]} for s in self.scenarios.values()]}
        if path == "/api/dashboard-layout":
            return self.layout(query.get("view") == ["1"])
        if path == "/api/hmi":
            return {"firmware": "JC-Riot SIMULATED" if self.scenario == "jc_first_start" else "WEB1 SIMULATED",
                    "m31Online": any(d["online"] and d["driver"] == "m31" for d in data["devices"]),
                    "ip": "127.0.0.1", "site": data["site"]}
        if path == "/status":
            return {"firmware": "JC-Riot SIMULATED" if self.scenario == "jc_first_start" else "WEB1 SIMULATED",
                    "uptimeSec": int(time.monotonic() - self.started),
                    "ethernetLink": True, "ethernetGotIp": True, "ip": "SIMULATED", "simulated": True}
        if path == "/api/time":
            return {"valid": True, "local": self.clock(), "utc": self.clock(), "timezoneLabel": "UTC", "simulated": True}
        if path == "/api/walkup":
            return {"active": False, "simulated": True}
        if path in ("/tags", "/api/library-signals"):
            return {"ok": True, "simulated": True, "tags" if path == "/tags" else "signals": [
                {**t, "name": t["name"], "source": "SITE_SIGNAL", "id": t["ref"],
                 "deviceName": next(d["name"] for d in data["devices"] if d["id"] == t["device"]),
                 "pool": t["pool"], "reason": "OK" if t["quality"] == "GOOD" else "DEVICE_OFFLINE"}
                for t in data["tags"] if t["selected"]
            ]}
        if path in ("/api/modbus", "/api/library-devices"):
            return {"ok": True, "simulated": True, "devices": data["devices"]}
        return None

    def write(self, path, params):
        before = copy.deepcopy((self.data, self.scenario, self.events, self.started,
                                self.walkthrough, self.samples, self.next_sample))
        try:
            result = self.apply_write(path, params)
            self.persist()
            return result
        except (OSError, ValueError):
            (self.data, self.scenario, self.events, self.started,
             self.walkthrough, self.samples, self.next_sample) = before
            raise

    def apply_write(self, path, params):
        if path == "/sim/scenario":
            self.reset(params.get("scenario", ""))
        elif path == "/sim/reset":
            self.reset("jc_first_start")
        elif path == "/sim/jc":
            if self.scenario != "jc_first_start":
                raise InvalidDraft("Open the JC first-start scenario first")
            action = params.get("action")
            if action == "add-m31":
                if self.walkthrough["installed"]:
                    raise InvalidDraft("M31 already installed in this demo")
                self.install_m31()
                self.walkthrough["installed"] = True
            elif action == "tags-save":
                refs = {t["ref"] for t in self.data["tags"]}
                dashboard = params.get("dashboard", "").split(",") if params.get("dashboard") else []
                logged = params.get("logged", "").split(",") if params.get("logged") else []
                if not refs or any(ref not in refs for ref in dashboard + logged):
                    raise InvalidDraft("Install the simulated M31 and select known Tags")
                if len(set(dashboard)) != len(dashboard) or len(set(logged)) != len(logged):
                    raise InvalidDraft("Duplicate Tag selection")
                self.data["dashboard"] = {
                    "statusMask": 7, "primaryDevice": "",
                    "groups": [{"id": 1, "name": "M31", "order": 1, "visible": True, "collapsed": False}] if dashboard else [],
                    "items": [{"ref": ref, "label": next(t["name"] for t in self.data["tags"] if t["ref"] == ref),
                               "groupId": 1, "order": i + 1, "visible": True} for i, ref in enumerate(dashboard)],
                }
                self.walkthrough["logged"] = logged
            elif action == "logger-save":
                interval = integer(params.get("interval"), 1, 300)
                enabled = bool(integer(params.get("enabled"), 0, 1))
                if enabled and not self.walkthrough["logged"]:
                    raise InvalidDraft("Select Log on at least one Tag first")
                self.walkthrough.update(logging=enabled, interval=interval)
                self.next_sample = 0
            else:
                raise InvalidDraft("Unknown JC walkthrough action")
            self.events.append({"time": self.clock(), "message": "SIMULATED JC: " + action})
        elif path == "/m31/output/do":
            channel = integer(params.get("ch"), 1, 4)
            value = integer(params.get("value"), 0, 1)
            device = next((d for d in self.data["devices"] if d["driver"] == "m31"), None)
            if not device or not device["online"]:
                raise InvalidDraft("Simulated Primary I/O is offline; output unchanged")
            tag = next(t for t in self.data["tags"] if t["device"] == device["id"] and t["id"] == f"do{channel}")
            tag["value"] = bool(value)
            self.events.append({"time": self.clock(), "message": f"Simulated DO{channel} set {'ON' if value else 'OFF'}"})
        elif path == "/sim/device":
            device = next((d for d in self.data["devices"] if d["id"] == params.get("id")), None)
            if not device:
                raise InvalidDraft("Unknown simulated Device")
            device["online"] = bool(integer(params.get("online"), 0, 1))
        elif path == "/sim/tag-dashboard":
            tag = next((t for t in self.data["tags"] if t["ref"] == params.get("ref")), None)
            if not tag:
                raise InvalidDraft("Unknown Device Tag")
            visible = bool(integer(params.get("visible"), 0, 1))
            item = next((i for i in self.data["dashboard"]["items"] if i["ref"] == tag["ref"]), None)
            if item:
                item["visible"] = visible
            elif visible:
                raise InvalidDraft("Assign this Tag in Dashboard Layout first")
        elif path == "/api/dashboard-layout/action":
            self.save_layout(params)
        else:
            raise InvalidDraft("Unsupported action; no real equipment writes are available")
        return {"ok": True, "simulated": True}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, port=8080, state_path=HERE / "state" / "current_state.json",
                 prototype_state_path=HERE / "state" / "tag_prototype_v1.json"):
        original_paths = {(HERE / "state" / "current_state.json").resolve()}
        if state_path is not None:
            original_paths.add(Path(state_path).resolve())
        if prototype_state_path is not None and Path(prototype_state_path).resolve() in original_paths:
            raise ValueError("Prototype state must not overwrite current_state.json or the original session")
        self.sim = Simulator(state_path)
        self.prototype = None
        self.prototype_state_path = prototype_state_path
        self.prototype_lock = threading.RLock()
        super().__init__(("127.0.0.1", port), Handler)

    def get_prototype(self):
        if self.prototype is None:
            self.prototype = TagPrototype(self.prototype_state_path)
        return self.prototype


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body, content_type="application/json", download=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=True).encode()
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type + ("; charset=utf-8" if content_type.startswith("text/") else ""))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        if download:
            self.send_header("Content-Disposition", f'attachment; filename="{download}"')
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; form-action 'self'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(body)

    def trusted(self):
        host = self.headers.get("Host")
        expected = f"127.0.0.1:{self.server.server_port}"
        if host != expected or self.headers.get("Origin", f"http://{expected}") != f"http://{expected}":
            self.reply(403, {"ok": False, "error": "Use the printed loopback URL; cross-origin requests are refused"})
            return False
        return True

    def do_GET(self):
        if not self.trusted():
            return
        parsed = urlsplit(self.path)
        if parsed.path == "/sim/sample.csv":
            with self.server.sim.lock:
                body = self.server.sim.sample_csv()
            self.reply(200, body, "text/csv", "JC-Riot-SIMULATED.csv")
            return
        if parsed.path in PROTOTYPE_PAGES:
            shell = (HERE / "web" / "shell.html").read_text(encoding="utf-8")
            shell = shell.replace("/web/introduction.js", "/web/tag-prototype.js")
            shell = shell.replace("</head>", '<link rel="stylesheet" href="/web/tag-prototype.css"></head>')
            start = shell.index('<nav class="nav"')
            end = shell.index("</nav>", start) + len("</nav>")
            shell = shell[:start] + '<nav class="nav" aria-label="Prototype navigation">' + "".join(
                f'<a href="{path}">{label}</a>' for path, label in [
                    ("/prototype/tags", "Tags"), ("/prototype/library", "Device Library"),
                    ("/prototype/devices", "Devices"), ("/prototype/replacement", "Replace IMV25"),
                    ("/", "Original Introduction")]) + "</nav>" + shell[end:]
            start = shell.index('<div class="demoToolbar">')
            end = shell.index("{{CONTENT}}", start)
            shell = shell[:start] + '<div class="demoToolbar"><b>P0 - configuration prototype</b><span>Flow, History, Alarms and assignments are simulated. No engines or field connections. Isolated state; no proven Nano capacity.</span></div>' + shell[end:]
            content = "<main class='wrap introCards prototype' id='prototypePage'><p role='status'>Loading isolated prototype...</p></main>"
            self.reply(200, shell.replace("{{CONTENT}}", content), "text/html")
            return
        if parsed.path in ("/api/prototype/state", "/api/prototype/replacement"):
            try:
                with self.server.prototype_lock:
                    prototype = self.server.get_prototype()
                    result = prototype.snapshot() if parsed.path.endswith("/state") else prototype.replacement_preview(
                        parse_qs(parsed.query).get("target", [""])[0])
                self.reply(200, result)
            except PrototypeError as exc:
                self.reply(400, {"ok": False, "error": str(exc)})
            except OSError as exc:
                print(f"Prototype state read failed: {exc}", flush=True)
                self.reply(500, {"ok": False, "error": "Cannot read isolated prototype state; file retained."})
            return
        with self.server.sim.lock:
            result = self.server.sim.read(parsed.path, parse_qs(parsed.query))
        if result is not None:
            self.reply(200, result)
        elif parsed.path in PAGES:
            shell = (HERE / "web" / "shell.html").read_text(encoding="utf-8")
            name = {"/": "dashboard.html", "/dashboard-layout": "dashboard-layout.html"}.get(parsed.path)
            content = (HERE / "web" / name).read_text(encoding="utf-8") if name else "<main class='wrap introCards' id='introPage'><p>Loading simulated controller...</p></main>"
            if parsed.path == "/":
                content = "<div class='wrap introCards' id='introPage'></div>" + content
            self.reply(200, shell.replace("{{CONTENT}}", content), "text/html")
        elif parsed.path in ("/web/riot.css", "/web/introduction.css", "/web/introduction.js",
                             "/web/tag-prototype.css", "/web/tag-prototype.js", "/web/OAI-LOGO-SMALL.PNG"):
            asset = HERE / "web" / parsed.path.rsplit("/", 1)[1]
            self.reply(200, asset.read_bytes(), mimetypes.guess_type(asset.name)[0] or "text/plain")
        else:
            self.reply(404, {"ok": False, "error": "Introduction route not found"})

    def do_POST(self):
        if not self.trusted():
            return
        try:
            length = integer(self.headers.get("Content-Length", ""), 0, 65536)
            if self.headers.get_content_type() != "application/x-www-form-urlencoded":
                raise InvalidDraft("Expected URL-encoded simulated action")
            body = self.rfile.read(length).decode("utf-8")
            values = parse_qs(body, keep_blank_values=True, max_num_fields=100)
            if any(len(v) != 1 for v in values.values()):
                raise InvalidDraft("Duplicate action fields")
            params = {k: v[0] for k, v in values.items()}
            path = urlsplit(self.path).path
            if path == "/api/prototype/save":
                with self.server.prototype_lock:
                    result = self.server.get_prototype().write(json.loads(params.get("draft", "")))
            else:
                with self.server.sim.lock:
                    result = self.server.sim.write(path, params)
        except (InvalidDraft, UnicodeError, ValueError, KeyError, TypeError) as exc:
            self.reply(400, {"ok": False, "error": str(exc)})
            return
        except OSError as exc:
            print(f"Simulator state save failed: {exc}", flush=True)
            self.reply(500, {"ok": False, "error": "Local state save failed; prior state retained. Check folder permissions and free space."})
            return
        if path == "/api/dashboard-layout/action" and params.get("ajax") != "1":
            self.send_response(303)
            self.send_header("Location", "/dashboard-layout")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self.reply(200, result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--open", action="store_true", help="Open the default browser")
    parser.add_argument("--no-browser", action="store_true", help="Override --open")
    parser.add_argument("--prototype", action="store_true", help="Open P0; original Introduction state is not loaded or saved")
    parser.add_argument("--prototype-state", type=Path, default=HERE / "state" / "tag_prototype_v1.json",
                        help="Isolated versioned P0 state file (never use current_state.json)")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("Port must be 1..65535")
    if args.prototype_state.resolve() == (HERE / "state" / "current_state.json").resolve():
        parser.error("Prototype state must not overwrite current_state.json")
    try:
        server = Server(args.port, state_path=None if args.prototype else HERE / "state" / "current_state.json",
                        prototype_state_path=args.prototype_state)
    except (OSError, ValueError, KeyError) as exc:
        print(f"Cannot start RIOT Introduction: {exc}\nIf the port is occupied, use start.bat --port 8081.")
        return 1
    url = f"http://127.0.0.1:{server.server_port}" + ("/prototype/tags" if args.prototype else "")
    print(f"RIOT Controller Introduction\nSimulated Controller - No field equipment connected\n{url}\nPress Ctrl+C to stop.", flush=True)
    if args.open and not args.no_browser:
        if not webbrowser.open(url):
            print("Browser could not be opened automatically. Open the URL above manually.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nIntroduction stopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
