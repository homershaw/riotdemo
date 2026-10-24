"""Tag-centric RIOT simulator model: owners create Tags, one Tag table, consumers read Tags.

Desktop simulation for design review and training. Values, profiles, Flow and Optimizer logic are
illustrative; capacities and memory figures are planning numbers, not firmware measurements.
"""
import ast
import copy
import csv
import io
import json
import math
import os
import random
import re
import threading
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
TAG_LIMIT = 256
PID_PLANNING_LIMIT = 16          # planning number until measured on a JC and a Nano
TAG_BYTES = 116                  # planning estimate: ~100 B settings + 16 B live value
SAMPLE_LIMIT = 2000
EVENT_LIMIT = 200
SPEED = 10                       # simulated seconds per real second
NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_.]{0,34}$")
OWNER_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,15}$")

# Illustrative Device Library profiles (not manufacturer register maps).
PROFILES = {
    "imv25": {"name": "Invensys IMV25 (illustrative)", "points": [
        ("DP", "Differential pressure", "analog", "kPa", 25.0),
        ("SP", "Static pressure", "analog", "kPa", 1050.0),
        ("PT", "Process temperature", "analog", "degC", 18.0)]},
    "rosemount3095": {"name": "Rosemount 3095FB (illustrative)", "points": [
        ("DP", "Differential pressure", "analog", "kPa", 24.0),
        ("SP", "Static pressure", "analog", "kPa", 1040.0),
        ("PT", "Process temperature", "analog", "degC", 19.0),
        ("SENSOR_T", "Sensor temperature", "analog", "degC", 21.0)]},
    "ngc8206": {"name": "ABB NGC8206 (illustrative)", "points": [
        ("C1", "Methane", "analog", "mol%", 91.2),
        ("C2", "Ethane", "analog", "mol%", 4.1),
        ("HV", "Heating value", "analog", "MJ/m3", 38.4),
        ("SG", "Specific gravity", "analog", "", 0.61),
        ("READY", "Analysis ready", "digital", "", 1)]},
    "rtu40": {"name": "Generic RTU, 40 points (illustrative)", "points":
        [(f"AI{i}", f"Analog {i}", "analog", "kPa", 300.0 + 10 * i) for i in range(1, 31)] +
        [(f"DI{i}", f"Status {i}", "digital", "", 0) for i in range(1, 11)]},
}
FEATURES = {
    "opt5": {"name": "Tubing Optimizer v5", "inputs": ["tubing", "casing", "line", "arrival", "flow"],
             "outputs": ["valve"],
             "tags": [("STATE", "analog", "", "state"), ("ARRIVALS", "analog", "", "in"),
                      ("SHUTIN_SP", "analog", "s", "setpoint"), ("OPEN_SP", "analog", "s", "setpoint"),
                      ("AFTERFLOW_SP", "analog", "s", "setpoint")]},
    "opt8": {"name": "Tubing Optimizer v8.00", "inputs": ["tubing", "casing", "line", "arrival", "flow", "lshh", "pshh", "esd"],
             "outputs": ["valve", "siteDo"],
             "tags": [("STATE", "analog", "", "state"), ("STEP", "analog", "", "in"),
                      ("SITE", "digital", "", "in"), ("ARRIVALS", "analog", "", "in"),
                      ("SHUTIN_SP", "analog", "s", "setpoint"), ("OPEN_SP", "analog", "s", "setpoint"),
                      ("AFTERFLOW_SP", "analog", "s", "setpoint"), ("CRIT_RATE", "analog", "E3m3/d", "in")]},
}
OPT_STATES = {0: "Stopped", 1: "Shut-in", 2: "Open", 3: "Afterflow"}
FLOW_POINTS = [("RATE", "E3m3/d"), ("HOUR", "E3m3"), ("DAY", "E3m3"), ("GRAND", "E3m3")]


class SimError(ValueError):
    pass


def _name(value, what="Tag name"):
    if not isinstance(value, str) or not NAME_RE.match(value):
        raise SimError(f"{what}: 1-35 letters, digits, _ or . and start with a letter or _")
    return value


def _num(value, what, low=-1e12, high=1e12):
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise SimError(f"{what} must be a number") from None
    if not math.isfinite(number) or not low <= number <= high:
        raise SimError(f"{what} must be between {low:g} and {high:g}")
    return number


def _int(value, what, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, str)) or not str(value).lstrip("-").isdigit():
        raise SimError(f"{what} must be a whole number")
    number = int(value)
    if not low <= number <= high:
        raise SimError(f"{what} must be {low}-{high}")
    return number


class TagSim:
    def __init__(self, state_path=None, seed=7):
        self.state_path = Path(state_path) if state_path else None
        self.lock = threading.RLock()
        self.random = random.Random(seed)
        self.blank()
        if self.state_path and self.state_path.exists():
            try:
                self.load(json.loads(self.state_path.read_text(encoding="utf-8")))
            except (ValueError, KeyError, TypeError) as exc:
                raise SimError(f"Saved tag simulator state is invalid; file kept at {self.state_path}: {exc}") from exc

    # ----- configuration -------------------------------------------------------------------
    def blank(self):
        self.cfg = {"version": 1, "nextTagId": 1, "nextOwnerId": 1, "owners": [], "tags": [],
                    "logger": {"enabled": False, "interval": 60}, "events": []}
        self.live = {}
        self.samples = []
        self.started = time.monotonic()
        self.last_tick = self.started
        self.sim_seconds = 0.0
        self.next_sample = 0.0
        self.scan_ms = []
        self.quality = {}

    def load(self, saved):
        if saved.get("version") != 1:
            raise SimError("Unsupported tag simulator state version")
        self.cfg = saved
        for tag in self.cfg["tags"]:
            self.live[tag["id"]] = self.initial_value(tag)

    def persist(self):
        if not self.state_path:
            return
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.state_path.with_suffix(".tmp")
        try:
            with temporary.open("w", encoding="utf-8") as out:
                json.dump(self.cfg, out, indent=1, allow_nan=False)
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, self.state_path)
        finally:
            if temporary.exists():
                temporary.unlink()

    def event(self, message):
        self.cfg["events"].append({"time": self.clock(), "message": message})
        del self.cfg["events"][:-EVENT_LIMIT]

    def clock(self):
        return (datetime(2026, 10, 24, 6, tzinfo=timezone.utc) + timedelta(seconds=int(self.sim_seconds))).strftime("%Y-%m-%d %H:%M:%S")

    def owner(self, owner_id):
        found = next((o for o in self.cfg["owners"] if o["id"] == owner_id), None)
        if not found:
            raise SimError("Unknown owner")
        return found

    def tag(self, tag_id):
        found = next((t for t in self.cfg["tags"] if t["id"] == tag_id), None)
        if not found:
            raise SimError("Unknown Tag")
        return found

    def tags_of(self, owner_id):
        return [t for t in self.cfg["tags"] if t["owner"] == owner_id]

    def unique_owner_name(self, base):
        names = {o["name"] for o in self.cfg["owners"]}
        if base not in names:
            return base
        n = 2
        while f"{base}_{n}" in names:
            n += 1
        return f"{base}_{n}"

    # ----- what each owner kind creates --------------------------------------------------------
    def points_for(self, owner):
        """(point key, kind, io, units, extra) for every Tag the owner should have."""
        k = owner["kind"]
        if k == "m31":
            pts = []
            for i in range(1, owner["ai"] + 1):
                pts.append((f"AI{i}", "analog", "in", "%", {"raw": [4.0, 20.0], "eu": [0.0, 100.0], "rawUnits": "mA"}))
            for i in range(1, owner["ao"] + 1):
                pts.append((f"AO{i}", "analog", "out", "%", {"raw": [4.0, 20.0], "eu": [0.0, 100.0], "rawUnits": "mA"}))
            pts += [(f"DI{i}", "digital", "in", "", {}) for i in range(1, owner["di"] + 1)]
            pts += [(f"DO{i}", "digital", "out", "", {}) for i in range(1, owner["do"] + 1)]
            return pts
        if k == "modbus":
            prof = PROFILES[owner["profile"]]
            return [(key, kind, "in", units, {"label": label, "base": base})
                    for key, label, kind, units, base in prof["points"] if key in owner["selected"]]
        if k == "mvs":
            return [("DP", "analog", "in", "kPa", {"base": 22.0}), ("SP", "analog", "in", "kPa", {"base": 980.0}),
                    ("PT", "analog", "in", "degC", {"base": 16.0})]
        if k == "flow":
            return [(key, "analog", "calc", units, {}) for key, units in FLOW_POINTS]
        if k == "calc":
            return [("VALUE", "analog", "calc", owner.get("units", ""), {})]
        if k == "manual":
            return [("VALUE", owner.get("type", "analog"), "setpoint", owner.get("units", ""), {})]
        if k == "pid":
            return [("SP", "analog", "setpoint", owner.get("units", ""), {}), ("OUT", "analog", "calc", "%", {}),
                    ("AUTO", "digital", "setpoint", "", {})]
        if k == "feature":
            return [(key, kind, io, units, {"states": OPT_STATES} if io == "state" else {})
                    for key, kind, units, io in FEATURES[owner["feature"]]["tags"]]
        raise SimError("Unknown owner kind")

    def make_tag(self, owner, point, kind, io, units, extra):
        tag = {"id": self.cfg["nextTagId"], "owner": owner["id"], "point": point,
               "name": f'{owner["name"]}.{point}' if owner["kind"] not in ("calc", "manual") else owner["name"],
               "kind": kind, "io": "state" if io == "state" else io, "units": units,
               "log": False, "dash": False, "alarms": {}, "modbus": None, "description": extra.get("label", "")}
        if "raw" in extra:
            tag.update(raw=list(extra["raw"]), eu=list(extra["eu"]), rawUnits=extra["rawUnits"])
        if "states" in extra:
            tag["states"] = {str(k): v for k, v in extra["states"].items()}
        if "base" in extra:
            tag["base"] = extra["base"]
        if self.find_name(tag["name"]) is not None:
            tag["name"] = f'{tag["name"]}_{tag["id"]}'
        self.cfg["nextTagId"] += 1
        return tag

    def find_name(self, name):
        return next((t for t in self.cfg["tags"] if t["name"].lower() == name.lower()), None)

    def initial_value(self, tag):
        if tag["io"] == "setpoint":
            return tag.get("setValue", 0.0 if tag["kind"] == "analog" else 0)
        if "raw" in tag and tag["io"] == "in":
            return self.random.uniform(7.0, 17.0)       # raw mA; scaled on read
        if "raw" in tag:
            return 4.0
        if tag["kind"] == "digital":
            return int(tag.get("base", 0))
        return float(tag.get("base", 0.0))

    # ----- capacity -------------------------------------------------------------------------
    def check_room(self, adding, removing=0):
        used = len(self.cfg["tags"]) - removing
        if used + adding > TAG_LIMIT:
            raise SimError(f"Needs {adding} Tags but only {TAG_LIMIT - used} of {TAG_LIMIT} are free; nothing was added")

    # ----- owners: add / change / remove / replace -----------------------------------------------
    def add_owner(self, params):
        kind = params.get("kind")
        owner = {"id": self.cfg["nextOwnerId"], "kind": kind, "enabled": True, "online": True}
        if kind == "m31":
            owner.update(name=self.unique_owner_name("M31"), **{k: _int(params.get(k, 0), k.upper(), 0, 16) for k in ("ai", "ao", "di", "do")},
                         host=params.get("host", "192.0.2.150"))
            if not any(owner[k] for k in ("ai", "ao", "di", "do")):
                raise SimError("At least one channel count must be above 0")
        elif kind == "modbus":
            if params.get("profile") not in PROFILES:
                raise SimError("Choose a Device Library profile")
            keys = [p[0] for p in PROFILES[params["profile"]]["points"]]
            selected = params.get("selected") or keys
            if not set(selected) <= set(keys) or not selected:
                raise SimError("Select at least one point of the profile")
            owner.update(name=self.unique_owner_name(params.get("name") or params["profile"].upper()),
                         profile=params["profile"], selected=[k for k in keys if k in selected], host=params.get("host", "192.0.2.20"))
        elif kind == "mvs":
            if sum(o["kind"] == "mvs" for o in self.cfg["owners"]) >= 6:
                raise SimError("MVS limit is 6 sensors")
            owner.update(name=self.unique_owner_name("MVS1" if not any(o["kind"] == "mvs" for o in self.cfg["owners"]) else "MVS"))
        elif kind == "flow":
            runs = {o["run"] for o in self.cfg["owners"] if o["kind"] == "flow"}
            run = next((r for r in range(1, 7) if r not in runs), None)
            if run is None:
                raise SimError("All 6 Flow runs are in use")
            owner.update(name=f"FLOW{run}", run=run, inputs={"dp": None, "sp": None, "pt": None},
                         manual={"dp": 25.0, "sp": 1000.0, "pt": 15.0})
        elif kind == "calc":
            owner.update(name=_name(params.get("name") or f"CALC{owner['id']}", "Calculation name"),
                         formula=str(params.get("formula") or "0"), units=str(params.get("units", ""))[:11])
            self.compile_formula(owner["formula"], allow_unknown=False)
        elif kind == "manual":
            owner.update(name=_name(params.get("name") or f"MANUAL{owner['id']}", "Manual value name"),
                         units=str(params.get("units", ""))[:11], type="digital" if params.get("type") == "digital" else "analog")
        elif kind == "pid":
            if sum(o["kind"] == "pid" for o in self.cfg["owners"]) >= PID_PLANNING_LIMIT:
                raise SimError(f"PID planning limit is {PID_PLANNING_LIMIT} loops until measured")
            n = 1 + sum(o["kind"] == "pid" for o in self.cfg["owners"])
            owner.update(name=self.unique_owner_name(f"PID{n}"), pv=None, out=None, kp=1.0, ki=0.2, kd=0.0,
                         interval=1.0, reverse=False, simulate=True, units="")
        elif kind == "feature":
            feature = params.get("feature")
            if feature not in FEATURES:
                raise SimError("Unknown feature")
            if any(o["kind"] == "feature" for o in self.cfg["owners"]):
                raise SimError("One Optimizer feature per controller in this simulator")
            owner.update(name=self.unique_owner_name("OPT"), feature=feature,
                         inputs={k: None for k in FEATURES[feature]["inputs"]},
                         outputs={k: None for k in FEATURES[feature]["outputs"]}, running=False, timer=0.0, arrivals=0)
        else:
            raise SimError("Unknown owner kind")
        pts = self.points_for(owner)
        self.check_room(len(pts))
        self.cfg["owners"].append(owner)
        self.cfg["nextOwnerId"] += 1
        for point in pts:
            tag = self.make_tag(owner, *point)
            if kind == "feature" and point[0].endswith("_SP"):
                tag["setValue"] = {"SHUTIN_SP": 1800.0, "OPEN_SP": 900.0, "AFTERFLOW_SP": 600.0}[point[0]]
            if kind == "pid" and point[0] == "SP":
                tag["setValue"] = 50.0
            self.cfg["tags"].append(tag)
            self.live[tag["id"]] = self.initial_value(tag)
        self.event(f"Added {self.describe(owner)}: {len(pts)} Tags created")
        return owner

    def describe(self, owner):
        k = owner["kind"]
        if k == "m31":
            return f'M31 rack {owner["name"]} ({owner["ai"]} AI, {owner["ao"]} AO, {owner["di"]} DI, {owner["do"]} DO)'
        if k == "modbus":
            return f'{PROFILES[owner["profile"]]["name"]} {owner["name"]}'
        if k == "feature":
            return f'{FEATURES[owner["feature"]]["name"]} {owner["name"]}'
        return {"mvs": "MVS sensor", "flow": "Flow run", "calc": "Calculation", "manual": "Manual value", "pid": "PID loop"}[k] + " " + owner["name"]

    def resize_preview(self, owner, new_owner):
        """Tags removed and added when an owner's channel counts or selected points change."""
        old = {t["point"]: t for t in self.tags_of(owner["id"])}
        new_points = {p[0]: p for p in self.points_for(new_owner)}
        removed = [old[k] for k in old if k not in new_points]
        added = [new_points[k] for k in new_points if k not in old]
        return removed, added

    def change_owner(self, owner_id, params, confirm=False):
        owner = self.owner(owner_id)
        draft = copy.deepcopy(owner)
        if "name" in params:
            pattern = OWNER_NAME_RE if owner["kind"] not in ("calc", "manual") else NAME_RE
            if not pattern.match(str(params["name"])):
                raise SimError("Owner name: letters, digits and _, starting with a letter")
            if any(o["name"] == params["name"] and o["id"] != owner_id for o in self.cfg["owners"]):
                raise SimError("Another owner already has that name")
            draft["name"] = params["name"]
        for key in ("enabled", "online"):
            if key in params:
                draft[key] = bool(params[key])
        if owner["kind"] == "m31":
            for k in ("ai", "ao", "di", "do"):
                if k in params:
                    draft[k] = _int(params[k], k.upper(), 0, 16)
            if not any(draft[k] for k in ("ai", "ao", "di", "do")):
                raise SimError("At least one channel count must be above 0")
        if owner["kind"] == "modbus" and "selected" in params:
            keys = [p[0] for p in PROFILES[owner["profile"]]["points"]]
            if not set(params["selected"]) <= set(keys) or not params["selected"]:
                raise SimError("Select at least one point")
            draft["selected"] = [k for k in keys if k in params["selected"]]
        if owner["kind"] == "flow":
            for key in ("dp", "sp", "pt"):
                if key in params.get("inputs", {}):
                    draft["inputs"][key] = self.ref_or_none(params["inputs"][key], analog=True)
                if key in params.get("manual", {}):
                    draft["manual"][key] = _num(params["manual"][key], f"Manual {key.upper()}")
        if owner["kind"] == "calc":
            if "formula" in params:
                self.compile_formula(params["formula"], allow_unknown=False)
                draft["formula"] = params["formula"]
        if owner["kind"] == "pid":
            for key in ("kp", "ki", "kd"):
                if key in params:
                    draft[key] = _num(params[key], key.upper(), 0, 1000)
            if "interval" in params:
                draft["interval"] = 0.1 if float(params["interval"]) < 0.5 else 1.0
            for key in ("reverse", "simulate"):
                if key in params:
                    draft[key] = bool(params[key])
            if "pv" in params:
                draft["pv"] = self.ref_or_none(params["pv"], analog=True)
            if "out" in params:
                draft["out"] = self.ref_or_none(params["out"], analog=True, output=True, owner=owner_id)
        if owner["kind"] == "feature":
            for key in draft["inputs"]:
                if key in params.get("inputs", {}):
                    draft["inputs"][key] = self.ref_or_none(params["inputs"][key], digital=key in ("arrival", "lshh", "pshh", "esd"),
                                                           analog=key not in ("arrival", "lshh", "pshh", "esd"))
            for key in draft["outputs"]:
                if key in params.get("outputs", {}):
                    draft["outputs"][key] = self.ref_or_none(params["outputs"][key], digital=True, output=True, owner=owner_id)
            if "running" in params:
                draft["running"] = bool(params["running"])
        removed, added = self.resize_preview(owner, draft)
        uses = {t["id"]: self.used_by(t["id"]) for t in removed}
        if removed and any(uses.values()) and not confirm:
            return {"ok": False, "needsConfirm": True, "removed": [self.tag_brief(t, uses[t["id"]]) for t in removed]}
        self.check_room(len(added), len(removed))
        owner.clear()
        owner.update(draft)
        for tag in removed:
            self.drop_tag(tag)
        for point in added:
            tag = self.make_tag(owner, *point)
            self.cfg["tags"].append(tag)
            self.live[tag["id"]] = self.initial_value(tag)
        self.event(f"Changed {self.describe(owner)}" + (f": {len(added)} Tags added, {len(removed)} removed" if added or removed else ""))
        return {"ok": True}

    def ref_or_none(self, value, analog=False, digital=False, output=False, owner=None):
        if value in (None, "", 0, "0"):
            return None
        tag = self.tag(_int(value, "Tag", 1, 10 ** 9))
        if analog and not digital and tag["kind"] != "analog":
            raise SimError(f'{tag["name"]} is not an analog Tag')
        if digital and not analog and tag["kind"] != "digital":
            raise SimError(f'{tag["name"]} is not a digital Tag')
        if output:
            if tag["io"] != "out":
                raise SimError(f'{tag["name"]} is not an output')
            writer = self.output_writer(tag["id"])
            if writer and writer["id"] != owner:
                raise SimError(f'{tag["name"]} is already driven by {writer["name"]}')
        return tag["id"]

    def output_writer(self, tag_id):
        for o in self.cfg["owners"]:
            if o["kind"] == "pid" and o.get("out") == tag_id:
                return o
            if o["kind"] == "feature" and tag_id in o["outputs"].values():
                return o
        return None

    def remove_preview(self, owner_id):
        owner = self.owner(owner_id)
        return {"ok": True, "owner": self.describe(owner),
                "tags": [self.tag_brief(t, self.used_by(t["id"])) for t in self.tags_of(owner_id)]}

    def remove_owner(self, owner_id):
        owner = self.owner(owner_id)
        tags = self.tags_of(owner_id)
        for tag in tags:
            self.drop_tag(tag)
        self.cfg["owners"].remove(owner)
        self.event(f"Removed {self.describe(owner)}: {len(tags)} Tags removed")

    def drop_tag(self, tag):
        """Remove a Tag; every consumer reference becomes 'Tag removed (name)'."""
        gone = {"removed": tag["name"]}
        for o in self.cfg["owners"]:
            for key in ("inputs", "outputs"):
                for k, v in list(o.get(key, {}).items()):
                    if v == tag["id"]:
                        o[key][k] = gone
            for key in ("pv", "out"):
                if o.get(key) == tag["id"]:
                    o[key] = gone
        self.cfg["tags"].remove(tag)
        self.live.pop(tag["id"], None)

    def replace_preview(self, owner_id, profile):
        owner = self.owner(owner_id)
        if owner["kind"] != "modbus" or profile not in PROFILES:
            raise SimError("Replace device applies to Modbus devices and a Device Library profile")
        new_points = PROFILES[profile]["points"]
        rows = []
        for tag in self.tags_of(owner_id):
            guess = next((p[0] for p in new_points if p[0] == tag["point"] and p[2] == tag["kind"]), None)
            rows.append({**self.tag_brief(tag, self.used_by(tag["id"])), "point": tag["point"], "suggest": guess})
        return {"ok": True, "from": PROFILES[owner["profile"]]["name"], "to": PROFILES[profile]["name"], "tags": rows,
                "points": [{"key": p[0], "label": p[1], "kind": p[2], "units": p[3]} for p in new_points]}

    def replace_owner(self, owner_id, profile, mapping):
        """mapping: {tagId: newPointKey or ''}. Mapped Tags keep ID, name, settings and uses."""
        preview = self.replace_preview(owner_id, profile)
        owner = self.owner(owner_id)
        points = {p["key"]: p for p in preview["points"]}
        targets = [v for v in mapping.values() if v]
        if len(targets) != len(set(targets)):
            raise SimError("Each new point can take only one Tag")
        for tag_id, key in mapping.items():
            tag = self.tag(int(tag_id))
            if tag["owner"] != owner_id:
                raise SimError("Tag does not belong to this device")
            if key and (key not in points or points[key]["kind"] != tag["kind"]):
                raise SimError(f'{tag["name"]}: choose a {tag["kind"]} point')
        old_name = PROFILES[owner["profile"]]["name"]
        owner["profile"] = profile
        owner["selected"] = [k for k in points if k in targets]
        for tag in list(self.tags_of(owner_id)):
            key = mapping.get(str(tag["id"])) or mapping.get(tag["id"])
            if key:
                tag["point"] = key
                tag["description"] = points[key]["label"]
                tag["base"] = next(p[4] for p in PROFILES[profile]["points"] if p[0] == key)
            else:
                self.drop_tag(tag)
        self.event(f'Replaced {old_name} with {PROFILES[profile]["name"]} on {owner["name"]}: {len(targets)} Tags kept their history and uses')

    # ----- Tag edits ---------------------------------------------------------------------------
    def save_tags(self, rows):
        draft = copy.deepcopy(self.cfg["tags"])
        by_id = {t["id"]: t for t in draft}
        for row in rows:
            tag = by_id.get(row.get("id"))
            if not tag:
                raise SimError("A Tag in the draft no longer exists; reload")
            if "name" in row:
                tag["name"] = _name(row["name"])
            if "units" in row:
                units = str(row["units"])
                if len(units) > 11 or any(ord(c) < 32 for c in units):
                    raise SimError(f'{tag["name"]}: units up to 11 characters')
                tag["units"] = units
            if "eu" in row and "raw" in tag:
                lo, hi = (_num(v, f'{tag["name"]} range') for v in row["eu"])
                if lo == hi:
                    raise SimError(f'{tag["name"]}: range low and high must differ')
                tag["eu"] = [lo, hi]
            for flag in ("log", "dash"):
                if flag in row:
                    tag[flag] = bool(row[flag])
            if "alarms" in row:
                alarms = {}
                for level in ("hh", "h", "l", "ll"):
                    value = row["alarms"].get(level)
                    if value not in (None, ""):
                        alarms[level] = _num(value, f'{tag["name"]} {level.upper()} alarm')
                if tag["kind"] == "digital" and alarms:
                    raise SimError(f'{tag["name"]}: digital Tags take no analog alarm limits')
                tag["alarms"] = alarms
            if "modbus" in row:
                tag["modbus"] = None if row["modbus"] in (None, "") else _int(row["modbus"], f'{tag["name"]} Modbus address', 40001, 49999)
        names = [t["name"].lower() for t in draft]
        if len(names) != len(set(names)):
            raise SimError("Two Tags have the same name")
        regs = {}
        for t in draft:
            if t["modbus"]:
                width = 2 if t["kind"] == "analog" else 1
                for r in range(t["modbus"], t["modbus"] + width):
                    if r in regs:
                        raise SimError(f'Modbus {r} is used by {regs[r]} and {t["name"]}')
                    regs[r] = t["name"]
        renamed = {t["id"]: (by_old["name"], t["name"]) for t in draft for by_old in self.cfg["tags"]
                   if by_old["id"] == t["id"] and by_old["name"] != t["name"]}
        self.cfg["tags"] = draft
        for o in self.cfg["owners"]:
            if o["kind"] == "calc":
                for old, new in renamed.values():
                    o["formula"] = re.sub(rf"(?<![A-Za-z0-9_.]){re.escape(old)}(?![A-Za-z0-9_.])", new, o["formula"])
        self.event(f"Saved {len(rows)} Tag change(s)" + (f", renamed {len(renamed)}" if renamed else ""))

    def set_value(self, tag_id, value):
        tag = self.tag(tag_id)
        if tag["io"] == "setpoint":
            tag["setValue"] = _num(value, tag["name"]) if tag["kind"] == "analog" else (1 if value in (1, "1", True, "ON") else 0)
            self.live[tag_id] = tag["setValue"]
        elif tag["io"] == "out":
            writer = self.output_writer(tag_id)
            if writer:
                raise SimError(f'{tag["name"]} is driven by {writer["name"]}; change it there')
            owner = self.owner(tag["owner"])
            if not owner["online"] or not owner["enabled"]:
                raise SimError(f'{owner["name"]} is not online; output unchanged')
            if tag["kind"] == "digital":
                self.live[tag_id] = 1 if value in (1, "1", True, "ON") else 0
            else:
                pct = _num(value, tag["name"], 0, 100)
                self.live[tag_id] = 4.0 + 16.0 * pct / 100.0
        else:
            raise SimError(f'{tag["name"]} is an input; its owner sets its value')
        self.event(f'{tag["name"]} set to {value}')

    # ----- references ----------------------------------------------------------------------
    def used_by(self, tag_id):
        uses = []
        tag = next((t for t in self.cfg["tags"] if t["id"] == tag_id), None)
        if not tag:
            return uses
        labels = {"dp": "DP", "sp": "static", "pt": "temperature", "tubing": "tubing pressure", "casing": "casing pressure",
                  "line": "line pressure", "arrival": "plunger arrival", "flow": "flow rate", "lshh": "LSHH", "pshh": "PSHH",
                  "esd": "ESD push button", "valve": "well valve", "siteDo": "site shutdown output"}
        for o in self.cfg["owners"]:
            for key in ("inputs", "outputs"):
                for k, v in o.get(key, {}).items():
                    if v == tag_id:
                        uses.append(f'{o["name"]} {labels.get(k, k)}')
            if o.get("pv") == tag_id:
                uses.append(f'{o["name"]} process variable')
            if o.get("out") == tag_id:
                uses.append(f'{o["name"]} output')
            if o["kind"] == "calc" and tag["name"] in self.formula_names(o["formula"]):
                uses.append(f'{o["name"]} formula')
        if tag["log"]:
            uses.append("Logger")
        if tag["dash"]:
            uses.append("Dashboard")
        if tag["alarms"]:
            uses.append("Alarms")
        if tag["modbus"]:
            uses.append(f'Modbus {tag["modbus"]}')
        return uses

    def tag_brief(self, tag, uses):
        return {"id": tag["id"], "name": tag["name"], "uses": uses}

    # ----- calculations --------------------------------------------------------------------
    @staticmethod
    def formula_names(formula):
        return set(re.findall(r"[A-Za-z_][A-Za-z0-9_.]*", formula))

    def compile_formula(self, formula, allow_unknown=True):
        if not isinstance(formula, str) or len(formula) > 120:
            raise SimError("Formula up to 120 characters")
        names = sorted(self.formula_names(formula), key=len, reverse=True)
        mapping = {}
        expr = formula
        for i, name in enumerate(names):
            if not allow_unknown and self.find_name(name) is None:
                raise SimError(f"Formula uses unknown Tag {name}")
            mapping[f"v{i}"] = name
            expr = re.sub(rf"(?<![A-Za-z0-9_.]){re.escape(name)}(?![A-Za-z0-9_.])", f"v{i}", expr)
        try:
            tree = ast.parse(expr, mode="eval")
        except SyntaxError:
            raise SimError("Formula: use Tag names, numbers, + - * / and parentheses") from None
        allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Name, ast.Load,
                   ast.Add, ast.Sub, ast.Mult, ast.Div, ast.USub, ast.UAdd)
        for node in ast.walk(tree):
            if not isinstance(node, allowed) or (isinstance(node, ast.Constant) and not isinstance(node.value, (int, float))):
                raise SimError("Formula: use Tag names, numbers, + - * / and parentheses")
        return compile(tree, "<formula>", "eval"), mapping

    # ----- live values ---------------------------------------------------------------------
    def eng(self, tag):
        """Engineering value of a Tag (scaled for raw-range Tags) and its quality."""
        if tag["id"] not in self.live:
            return None, "BAD"
        owner = next((o for o in self.cfg["owners"] if o["id"] == tag["owner"]), None)
        if owner is None:
            return None, "BAD"
        if not owner["enabled"]:
            return None, "DISABLED"
        if owner["kind"] in ("m31", "modbus", "mvs") and not owner["online"]:
            return None, "BAD"
        value = self.live[tag["id"]]
        if "raw" in tag:
            (r0, r1), (e0, e1) = tag["raw"], tag["eu"]
            if tag["io"] == "in" and not 3.5 <= value <= 21.5:
                return None, "BAD"
            value = e0 + (value - r0) * (e1 - e0) / (r1 - r0)
        return value, self.quality.get(tag["id"], "GOOD")

    def tick(self):
        now = time.monotonic()
        real = min(now - self.last_tick, 5.0)
        self.last_tick = now
        if real <= 0:
            return
        start = time.perf_counter()
        dt = real * SPEED
        self.sim_seconds += dt
        self.quality = {}
        tags = {t["id"]: t for t in self.cfg["tags"]}
        driven = set()
        for o in self.cfg["owners"]:
            if o["kind"] == "pid" and isinstance(o.get("pv"), int) and o.get("simulate"):
                driven.add(o["pv"])
        for t in self.cfg["tags"]:
            o = next(x for x in self.cfg["owners"] if x["id"] == t["owner"])
            if t["io"] == "in" and o["kind"] in ("m31", "modbus", "mvs") and t["id"] not in driven:
                v = self.live.get(t["id"], 0)
                if t["kind"] == "digital":
                    if self.random.random() < 0.002 * dt and o["kind"] == "m31":
                        self.live[t["id"]] = 1 - int(v)
                elif "raw" in t:
                    self.live[t["id"]] = min(19.5, max(4.5, v + self.random.gauss(0, 0.03) * math.sqrt(dt)))
                else:
                    base = float(t.get("base", 0.0))
                    self.live[t["id"]] = base + 0.02 * abs(base) * math.sin(self.sim_seconds / 300 + t["id"]) + self.random.gauss(0, 0.002 * abs(base) + 1e-6)
        for o in self.cfg["owners"]:
            if o["kind"] == "flow":
                self.tick_flow(o, tags, dt)
            elif o["kind"] == "pid":
                self.tick_pid(o, tags, dt)
            elif o["kind"] == "feature":
                self.tick_feature(o, tags, dt)
        for o in self.cfg["owners"]:
            if o["kind"] == "calc":
                self.tick_calc(o)
        self.scan_ms.append((time.perf_counter() - start) * 1000)
        del self.scan_ms[:-50]
        if self.cfg["logger"]["enabled"] and self.sim_seconds >= self.next_sample:
            for t in self.cfg["tags"]:
                if t["log"]:
                    value, q = self.eng(t)
                    self.samples.append([self.clock(), t["id"], t["name"], "" if value is None else round(value, 4), t["units"], q, "SIMULATED"])
            del self.samples[:-SAMPLE_LIMIT]
            self.next_sample = self.sim_seconds + self.cfg["logger"]["interval"]

    def input_value(self, ref, tags, fallback=None):
        if isinstance(ref, int) and ref in tags:
            value, q = self.eng(tags[ref])
            if q == "GOOD":
                return value, True
            return fallback, False
        return fallback, fallback is not None

    def owner_tag(self, o, point):
        return next(t for t in self.cfg["tags"] if t["owner"] == o["id"] and t["point"] == point)

    def tick_flow(self, o, tags, dt):
        dp, ok1 = self.input_value(o["inputs"]["dp"], tags, o["manual"]["dp"] if o["inputs"]["dp"] is None else None)
        sp, ok2 = self.input_value(o["inputs"]["sp"], tags, o["manual"]["sp"] if o["inputs"]["sp"] is None else None)
        pt, ok3 = self.input_value(o["inputs"]["pt"], tags, o["manual"]["pt"] if o["inputs"]["pt"] is None else None)
        rate_tag = self.owner_tag(o, "RATE")
        if not (ok1 and ok2 and ok3) or not o["enabled"]:
            for p in FLOW_POINTS:
                self.quality[self.owner_tag(o, p[0])["id"]] = "BAD" if o["enabled"] else "DISABLED"
            self.live[rate_tag["id"]] = 0.0
            return
        rate = 5.16 * math.sqrt(max(dp, 0.0) * max(sp + 101.325, 0.0) / max(pt + 273.15, 1.0))   # illustrative, not AGA-3
        self.live[rate_tag["id"]] = rate
        volume = rate * dt / 86400.0
        hour_prev = int((self.sim_seconds - dt) // 3600)
        day_prev = int((self.sim_seconds - dt - 6 * 3600) // 86400)       # contract hour 06:00
        for point, reset in (("HOUR", int(self.sim_seconds // 3600) != hour_prev),
                             ("DAY", int((self.sim_seconds - 6 * 3600) // 86400) != day_prev), ("GRAND", False)):
            tid = self.owner_tag(o, point)["id"]
            self.live[tid] = (0.0 if reset else self.live.get(tid, 0.0)) + volume

    def tick_pid(self, o, tags, dt):
        sp_tag, out_tag, auto_tag = (self.owner_tag(o, p) for p in ("SP", "OUT", "AUTO"))
        out = self.live.get(out_tag["id"], 0.0)
        pv, ok = self.input_value(o.get("pv"), tags)
        if not ok or not o["enabled"]:
            self.quality[out_tag["id"]] = "BAD" if o["enabled"] else "DISABLED"
            return
        if self.live.get(auto_tag["id"], 0):
            pv_tag = tags[o["pv"]]
            span = (pv_tag["eu"][1] - pv_tag["eu"][0]) if "raw" in pv_tag else 100.0
            error = (self.live.get(sp_tag["id"], 0.0) - pv) / (span or 1.0) * 100.0
            if o["reverse"]:
                error = -error
            steps = max(1, int(dt / o["interval"]))
            h = dt / steps
            for _ in range(steps):
                integral = o.setdefault("_i", 0.0) + o["ki"] * error * h
                o["_i"] = min(100.0, max(0.0, integral))
                out = min(100.0, max(0.0, o["kp"] * error + o["_i"] - o["kd"] * (error - o.get("_e", error)) / h))
                o["_e"] = error
        self.live[out_tag["id"]] = out
        if isinstance(o.get("out"), int) and o["out"] in tags:
            self.live[o["out"]] = 4.0 + 16.0 * out / 100.0
        if o.get("simulate") and isinstance(o.get("pv"), int):
            pv_tag = tags[o["pv"]]
            target = out if not o["reverse"] else 100.0 - out
            if "raw" in pv_tag:
                target_raw = 4.0 + 16.0 * target / 100.0
                cur = self.live.get(pv_tag["id"], 4.0)
                self.live[pv_tag["id"]] = cur + (target_raw - cur) * min(1.0, dt / 60.0)
            else:
                cur = self.live.get(pv_tag["id"], 0.0)
                self.live[pv_tag["id"]] = cur + (target - cur) * min(1.0, dt / 60.0)

    def tick_feature(self, o, tags, dt):
        state_t, arrivals_t = self.owner_tag(o, "STATE"), self.owner_tag(o, "ARRIVALS")
        sp = {p: self.live.get(self.owner_tag(o, p)["id"], 0.0) for p in ("SHUTIN_SP", "OPEN_SP", "AFTERFLOW_SP")}
        state = int(self.live.get(state_t["id"], 0))
        if not o["running"] or not o["enabled"]:
            state = 0
        else:
            missing = [k for k in ("tubing", "casing", "line") if not isinstance(o["inputs"].get(k), int)]
            if missing:
                state = 0
                o["running"] = False
                self.event(f'{o["name"]} stopped: {", ".join(missing)} input not selected')
            else:
                if state == 0:
                    state, o["timer"] = 1, 0.0
                o["timer"] += dt
                limit = {1: sp["SHUTIN_SP"], 2: sp["OPEN_SP"], 3: sp["AFTERFLOW_SP"]}[state]
                if state == 2 and o["timer"] > min(limit, 300.0) and self.random.random() < 0.02 * dt:
                    o["arrivals"] += 1
                    state, o["timer"] = 3, 0.0
                elif o["timer"] >= limit:
                    state, o["timer"] = (2 if state == 1 else 1), 0.0
        self.live[state_t["id"]] = state
        self.live[arrivals_t["id"]] = float(o["arrivals"])
        if "STEP" in {t["point"] for t in self.tags_of(o["id"])}:
            self.live[self.owner_tag(o, "STEP")["id"]] = float(state)
            self.live[self.owner_tag(o, "SITE")["id"]] = 1
            self.live[self.owner_tag(o, "CRIT_RATE")["id"]] = 12.0
        valve = o["outputs"].get("valve")
        if isinstance(valve, int) and valve in tags:
            self.live[valve] = 1 if state in (2, 3) else 0

    def tick_calc(self, o):
        tag = self.owner_tag(o, "VALUE")
        try:
            code, mapping = self.compile_formula(o["formula"])
            env = {}
            for var, name in mapping.items():
                src = self.find_name(name)
                value, q = self.eng(src) if src else (None, "BAD")
                if q != "GOOD":
                    raise SimError("input not GOOD")
                env[var] = value
            self.live[tag["id"]] = float(eval(code, {"__builtins__": {}}, env))  # AST-restricted arithmetic only
        except (SimError, ZeroDivisionError, OverflowError):
            self.quality[tag["id"]] = "BAD"

    # ----- snapshot --------------------------------------------------------------------------
    def alarm_state(self, tag, value):
        if value is None or not tag["alarms"]:
            return ""
        a = tag["alarms"]
        for level, test in (("HH", lambda: "hh" in a and value >= a["hh"]), ("LL", lambda: "ll" in a and value <= a["ll"]),
                            ("H", lambda: "h" in a and value >= a["h"]), ("L", lambda: "l" in a and value <= a["l"])):
            if test():
                return level
        return ""

    def snapshot(self):
        self.tick()
        owners = {o["id"]: o for o in self.cfg["owners"]}
        tags = []
        for t in self.cfg["tags"]:
            value, q = self.eng(t)
            raw = self.live.get(t["id"]) if "raw" in t else None
            row = {k: v for k, v in t.items() if k not in ("base",)}
            row.update(value=None if value is None else round(value, 4), quality=q, raw=t.get("raw"),
                       rawValue=None if raw is None else round(raw, 3), owner=t["owner"], ownerName=owners[t["owner"]]["name"],
                       ownerKind=owners[t["owner"]]["kind"], usedBy=self.used_by(t["id"]), alarm=self.alarm_state(t, value),
                       writer=(self.output_writer(t["id"]) or {}).get("name"))
            tags.append(row)
        counts = {}
        for t in self.cfg["tags"]:
            kind = owners[t["owner"]]["kind"]
            counts[kind] = counts.get(kind, 0) + 1
        scan = sorted(self.scan_ms)
        return {"ok": True, "simulated": True, "time": self.clock(), "owners": [self.owner_view(o) for o in self.cfg["owners"]],
                "tags": tags, "logger": self.cfg["logger"], "samples": len(self.samples), "events": self.cfg["events"][-40:],
                "profiles": {k: {"name": v["name"], "points": [{"key": p[0], "label": p[1], "kind": p[2], "units": p[3]} for p in v["points"]]}
                             for k, v in PROFILES.items()},
                "features": {k: {"name": v["name"], "inputs": v["inputs"], "outputs": v["outputs"]} for k, v in FEATURES.items()},
                "measure": {"tags": len(self.cfg["tags"]), "tagLimit": TAG_LIMIT, "tagBytes": TAG_BYTES,
                            "memoryBytes": len(self.cfg["tags"]) * TAG_BYTES, "byOwnerKind": counts,
                            "pidLoops": sum(o["kind"] == "pid" for o in self.cfg["owners"]), "pidLimit": PID_PLANNING_LIMIT,
                            "logged": sum(t["log"] for t in self.cfg["tags"]), "dashboard": sum(t["dash"] for t in self.cfg["tags"]),
                            "alarmed": sum(bool(t["alarms"]) for t in self.cfg["tags"]),
                            "modbus": sum(t["modbus"] is not None for t in self.cfg["tags"]),
                            "scanMsAvg": round(sum(scan) / len(scan), 3) if scan else 0,
                            "scanMsMax": round(scan[-1], 3) if scan else 0, "owners": len(self.cfg["owners"])}}

    def owner_view(self, o):
        view = {k: v for k, v in o.items() if not k.startswith("_")}
        view["label"] = self.describe(o)
        view["tagCount"] = len(self.tags_of(o["id"]))
        if o["kind"] == "feature":
            view["featureName"] = FEATURES[o["feature"]]["name"]
        if o["kind"] == "modbus":
            view["profileName"] = PROFILES[o["profile"]]["name"]
        return view

    def csv(self):
        out = io.StringIO(newline="")
        writer = csv.writer(out)
        writer.writerow(["UtcTime", "TagId", "Name", "Value", "Units", "Quality", "DataSource"])
        writer.writerows(self.samples)
        return out.getvalue()

    # ----- example sites -----------------------------------------------------------------------
    def load_example(self, name):
        self.blank()
        if name == "blank":
            self.event("Blank controller")
            return
        if name == "plunger":
            m31 = self.add_owner({"kind": "m31", "ai": 8, "ao": 4, "di": 4, "do": 4})
            names = {"AI1": ("TUBING_PRESS", "kPa", [0, 10000]), "AI2": ("CASING_PRESS", "kPa", [0, 10000]),
                     "AI3": ("LINE_PRESS", "kPa", [0, 7000]), "AI4": ("SEP_LEVEL", "%", [0, 100]),
                     "DI1": ("PLUNGER_ARRIVAL", "", None), "DO1": ("WELL_VALVE", "", None), "AO1": ("CHOKE_POS", "%", [0, 100])}
            rows = []
            for t in self.tags_of(m31["id"]):
                if t["point"] in names:
                    n, u, r = names[t["point"]]
                    row = {"id": t["id"], "name": n, "units": u, "log": True, "dash": True}
                    if r and "raw" in t:
                        row["eu"] = r
                    rows.append(row)
            self.save_tags(rows)
            mvs = self.add_owner({"kind": "mvs"})
            flow = self.add_owner({"kind": "flow"})
            dp, sp, pt = (self.owner_tag(mvs, p)["id"] for p in ("DP", "SP", "PT"))
            self.change_owner(flow["id"], {"inputs": {"dp": dp, "sp": sp, "pt": pt}})
            for p in ("RATE", "DAY"):
                t = self.owner_tag(flow, p)
                t["log"] = t["dash"] = True
            opt = self.add_owner({"kind": "feature", "feature": "opt8"})
            ids = {t["name"]: t["id"] for t in self.cfg["tags"]}
            self.change_owner(opt["id"], {"inputs": {"tubing": ids["TUBING_PRESS"], "casing": ids["CASING_PRESS"], "line": ids["LINE_PRESS"],
                                                     "arrival": ids["PLUNGER_ARRIVAL"], "flow": self.owner_tag(flow, "RATE")["id"]},
                                          "outputs": {"valve": ids["WELL_VALVE"]}, "running": True})
            pid = self.add_owner({"kind": "pid"})
            self.change_owner(pid["id"], {"pv": ids["SEP_LEVEL"], "out": ids["CHOKE_POS"]})
            self.live[self.owner_tag(pid, "AUTO")["id"]] = 1
            self.owner_tag(pid, "AUTO")["setValue"] = 1
            self.owner_tag(self.owner(opt["id"]), "STATE")["dash"] = True
            self.cfg["tags"][self.cfg["tags"].index(self.find_name("TUBING_PRESS"))]["alarms"] = {"hh": 9000.0, "h": 8000.0}
            self.add_owner({"kind": "calc", "name": "CASING_TUBING_DIFF", "formula": "CASING_PRESS - TUBING_PRESS", "units": "kPa"})
            self.event("Example site: plunger-lift well")
        elif name == "hub":
            for i in range(5):
                self.add_owner({"kind": "modbus", "profile": "rtu40", "name": f"RTU{i + 1}"})
            for i in range(6):
                self.add_owner({"kind": "flow"})
            self.add_owner({"kind": "m31", "ai": 8, "ao": 0, "di": 4, "do": 4})
            self.event("Example site: gathering hub near the 256-Tag limit")
        else:
            raise SimError("Unknown example")

    # ----- actions -------------------------------------------------------------------------
    def action(self, body):
        with self.lock:
            before = copy.deepcopy((self.cfg, self.live, self.samples, self.sim_seconds, self.next_sample))
            try:
                result = self.apply(body) or {"ok": True}
                if result.get("ok", True) and body.get("op") not in ("removePreview", "replacePreview"):
                    self.persist()
                return result
            except (SimError, OSError, KeyError, TypeError, ValueError):
                self.cfg, self.live, self.samples, self.sim_seconds, self.next_sample = before
                raise

    def apply(self, body):
        op = body.get("op")
        if op == "addOwner":
            owner = self.add_owner(body.get("params", {}))
            return {"ok": True, "owner": owner["id"]}
        if op == "changeOwner":
            return self.change_owner(int(body["owner"]), body.get("params", {}), bool(body.get("confirm")))
        if op == "removePreview":
            return self.remove_preview(int(body["owner"]))
        if op == "removeOwner":
            self.remove_owner(int(body["owner"]))
            return None
        if op == "replacePreview":
            return self.replace_preview(int(body["owner"]), body.get("profile"))
        if op == "replaceOwner":
            self.replace_owner(int(body["owner"]), body.get("profile"), body.get("mapping", {}))
            return None
        if op == "saveTags":
            self.save_tags(body.get("rows", []))
            return None
        if op == "setValue":
            self.set_value(int(body["tag"]), body.get("value"))
            return None
        if op == "logger":
            self.cfg["logger"] = {"enabled": bool(body.get("enabled")), "interval": _int(body.get("interval", 60), "Interval", 10, 3600)}
            self.next_sample = 0.0
            self.event("Logger " + ("on" if self.cfg["logger"]["enabled"] else "off"))
            return None
        if op == "example":
            self.load_example(body.get("name"))
            return None
        raise SimError("Unknown action")
