"""P0 configuration relationships only; no transport, polling or alarm engine."""
import copy
import json
import math
import os
import time
from datetime import datetime, timezone
from pathlib import Path

VERSION = 1
STATES = ("online", "disconnected", "disabled", "missing", "incompatible")
ROLES = ("", "dp", "static", "temp", "reverse")
QUANTITIES = ("pressure", "temperature", "current", "number", "boolean")


class PrototypeError(ValueError):
    pass


def name(value):
    if not isinstance(value, str) or not value.strip() or len(value.encode("utf-8")) > 80:
        raise PrototypeError("Name must contain 1..80 UTF-8 bytes.")
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise PrototypeError("Control characters are not permitted.")
    return value.strip()


def number(value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise PrototypeError("A finite numeric value is required.")
    return value


def flag(value):
    if type(value) is not bool:
        raise PrototypeError("Expected a true/false setting.")
    return value


def point(ident, label, value, units, quantity, reference="none", selected=True):
    return {"id": ident, "name": label, "value": value, "units": units,
            "quantity": quantity, "reference": reference, "selected": selected,
            "type": "boolean" if type(value) is bool else "real"}


def transmitter_points(selected=False):
    return [
        point("dp", "Differential Pressure", 8.4, "kPa", "pressure", "differential", selected),
        point("pressure", "Static Pressure", 620.0, "kPa", "pressure", "gauge", selected),
        point("temperature", "Process Temperature", 24.6, "C", "temperature", selected=selected),
        point("unit_code", "Configured Unit Code (illustrative)", 3, "", "number", selected=False),
    ]


def seed():
    original = json.loads((Path(__file__).parent / "scenarios" / "normal_site.json").read_text(encoding="utf-8"))
    profiles = copy.deepcopy(original["profiles"])
    profiles.extend([
        {"id": "p0_imv25", "name": "IMV25-M (P0 fixture)", "vendor": "Foxboro", "model": "IMV25-M",
         "available": 4, "validation": "Illustrative configuration fixture; no parser or hardware validation"},
        {"id": "p0_rosemount", "name": "Rosemount 3095FB (P0 fixture)", "vendor": "Rosemount", "model": "3095FB",
         "available": 4, "validation": "Illustrative configuration fixture; no parser or hardware validation"},
    ])
    devices = []
    for index, old in enumerate(original["devices"], 1):
        points = []
        for t in original["tags"]:
            if t["device"] != old["id"]:
                continue
            quantity = "boolean" if type(t["value"]) is bool else "current" if t["units"] == "mA" else "temperature" if t["id"] == "temperature" else "number"
            points.append(point(t["id"], t["name"], t["value"], "C" if quantity == "temperature" else t["units"], quantity))
        devices.append({"id": f"D{index:06}", "slot": old["id"], "name": old["name"],
                        "profile": old["profile"], "address": old["address"], "state": "online", "points": points})
    for index, profile, label in [(5, "p0_imv25", "Sales IMV25"), (6, "p0_rosemount", "Replacement Rosemount")]:
        devices.append({"id": f"D{index:06}", "slot": f"LD{index:02}", "name": label,
                        "profile": profile, "address": "Simulated RTU / Unit 1",
                        "state": "online", "points": transmitter_points(index == 5)})
    tags = []

    def add(label, device_id, point_id, quantity, units, reference="none", role="", conversion=None):
        tags.append({"id": f"P{len(tags)+1:06}", "name": label, "quantity": quantity,
                     "units": units, "reference": reference, "binding_revision": 1,
                     "binding": {"device_id": device_id, "point_id": point_id, "conversion": conversion},
                     "history": {"enabled": quantity != "boolean", "sample_sec": 1, "aggregation": "hourly_min_max_avg"},
                     "alarms": {"levels": {k: {"enabled": False, "threshold": 0.0} for k in ("HH", "H", "L", "LL")},
                                "deadband": 0.0, "delay_sec": 0, "latch": False},
                     "usage": {"dashboard": True, "mqtt": False, "rule": False, "flow_role": role}})
    add("Sales DP", "D000005", "dp", "pressure", "kPa", "differential", "dp")
    add("Sales Static Pressure", "D000005", "pressure", "pressure", "kPa", "gauge", "static")
    add("Sales Temperature", "D000005", "temperature", "temperature", "C", role="temp")
    add("Casing Pressure", "D000001", "ai2", "pressure", "kPa", "gauge",
        conversion={"kind": "current_range", "eu4": 0.0, "eu20": 1000.0})
    add("Burner Temperature", "D000002", "temperature", "temperature", "C")
    add("Sales Flow Rate", "D000003", "flow_rate", "number", "E3m3/day")
    add("Methane", "D000004", "methane", "number", "mol %")
    add("Analysis Ready", "D000004", "analysis_ready", "boolean", "")
    return {"version": VERSION, "namespace": "RIOT-P0-DEMO", "revision": 1,
            "next_device": 7, "next_tag": 9, "profiles": profiles, "devices": devices,
            "tags": tags, "events": []}


class TagPrototype:
    def __init__(self, state_path=None):
        self.state_path = Path(state_path) if state_path is not None else None
        self.samples = {}
        self.data = seed()
        if self.state_path is not None and self.state_path.exists():
            try:
                saved = json.loads(self.state_path.read_text(encoding="utf-8"))
                self.validate_state(saved)
            except (ValueError, KeyError, TypeError, UnicodeError) as exc:
                raise PrototypeError(f"Prototype state invalid; file retained at {self.state_path}: {exc}") from exc
            self.data = saved

    @staticmethod
    def device(data, ident):
        return next((d for d in data["devices"] if d["id"] == ident), None)

    @staticmethod
    def tag(data, ident):
        return next((t for t in data["tags"] if t["id"] == ident), None)

    def resolve_point(self, data, binding):
        if binding is None:
            return None, None
        d = self.device(data, binding["device_id"])
        q = next((p for p in d["points"] if p["id"] == binding["point_id"]), None) if d else None
        return d, q

    def compatibility(self, data, tag, binding):
        if binding is None:
            return []
        d, q = self.resolve_point(data, binding)
        if not d or d["state"] == "missing" or not q:
            return ["Device or point is missing; instance IDs are never reused."]
        errors = []
        if d["state"] in ("disabled", "incompatible"):
            errors.append("Device is " + d["state"] + ".")
        if not q["selected"]:
            errors.append("Select this Device Tag on the Device first.")
        conversion = binding.get("conversion")
        if conversion:
            if q["quantity"] != "current" or q["units"] != "mA" or tag["quantity"] != "pressure" or tag["units"] != "kPa":
                errors.append("4-20 mA conversion requires a current point and a kPa pressure Tag.")
        elif (q["quantity"], q["units"], q["reference"]) != (tag["quantity"], tag["units"], tag["reference"]):
            errors.append("Quantity, units or pressure reference do not match.")
        return errors

    def validate_tag(self, data, tag):
        name(tag["name"])
        if tag["quantity"] not in QUANTITIES or tag["reference"] not in ("none", "gauge", "absolute", "differential"):
            raise PrototypeError("Invalid quantity or pressure reference.")
        if not isinstance(tag["units"], str) or len(tag["units"]) > 16:
            raise PrototypeError("Invalid units.")
        binding = tag["binding"]
        if binding is not None:
            if not isinstance(binding, dict) or set(binding) != {"device_id", "point_id", "conversion"}:
                raise PrototypeError("Invalid binding.")
            name(binding["device_id"])
            name(binding["point_id"])
            c = binding["conversion"]
            if c is not None:
                if set(c) != {"kind", "eu4", "eu20"} or c["kind"] != "current_range":
                    raise PrototypeError("Only explicit 4-20 mA conversion is simulated.")
                if number(c["eu20"]) <= number(c["eu4"]):
                    raise PrototypeError("20 mA engineering value must exceed the 4 mA value.")
        history = tag["history"]
        flag(history["enabled"])
        if type(history["sample_sec"]) is not int or not 1 <= history["sample_sec"] <= 3600:
            raise PrototypeError("History sample interval must be 1..3600 seconds.")
        if history["aggregation"] != "hourly_min_max_avg":
            raise PrototypeError("Only illustrative hourly min/max/average configuration is supported.")
        if tag["quantity"] == "boolean" and history["enabled"]:
            raise PrototypeError("Numeric history cannot be enabled for a Boolean Tag.")
        alarm = tag["alarms"]
        if set(alarm["levels"]) != {"HH", "H", "L", "LL"}:
            raise PrototypeError("Expected HH/H/L/LL settings.")
        previous = None
        for key in ("LL", "L", "H", "HH"):
            level = alarm["levels"][key]
            threshold = number(level["threshold"])
            if flag(level["enabled"]):
                if tag["quantity"] == "boolean":
                    raise PrototypeError("Numeric alarms require a numeric Tag.")
                if previous is not None and threshold < previous:
                    raise PrototypeError("Enabled alarm thresholds must satisfy LL <= L <= H <= HH.")
                previous = threshold
        if number(alarm["deadband"]) < 0 or type(alarm["delay_sec"]) is not int or not 0 <= alarm["delay_sec"] <= 86400:
            raise PrototypeError("Alarm deadband must be nonnegative; delay must be 0..86400 seconds.")
        flag(alarm["latch"])
        usage = tag["usage"]
        for key in ("dashboard", "mqtt", "rule"):
            flag(usage[key])
        role = usage["flow_role"]
        if role not in ROLES:
            raise PrototypeError("Unknown Flow role.")
        if role:
            expected = ("temperature", "C", "none") if role == "temp" else ("pressure", "kPa", tag["reference"] if role == "static" else "differential")
            if (tag["quantity"], tag["units"], tag["reference"]) != expected or (role == "static" and tag["reference"] not in ("gauge", "absolute")):
                raise PrototypeError("Flow requires kPa DP, gauge/absolute kPa pressure, and Celsius temperature.")
        if type(tag["binding_revision"]) is not int or tag["binding_revision"] < 1:
            raise PrototypeError("Invalid binding revision.")

    def validate_state(self, data):
        if not isinstance(data, dict) or data.get("version") != VERSION or data.get("namespace") != "RIOT-P0-DEMO":
            raise PrototypeError("Unsupported prototype state version/namespace.")
        for key in ("revision", "next_device", "next_tag"):
            if type(data[key]) is not int or data[key] < 1:
                raise PrototypeError("Invalid revision or identity counter.")
        if not isinstance(data["profiles"], list) or not isinstance(data["devices"], list) or not isinstance(data["tags"], list):
            raise PrototypeError("Invalid prototype collections.")
        device_ids, slots, max_device = set(), set(), 0
        for d in data["devices"]:
            ident = d["id"]
            if not isinstance(ident, str) or len(ident) != 7 or ident[0] != "D" or not ident[1:].isascii() or not ident[1:].isdigit() or ident in device_ids:
                raise PrototypeError("Duplicate/invalid Device identity.")
            device_ids.add(ident)
            max_device = max(max_device, int(ident[1:]))
            name(d["name"])
            name(d["slot"])
            if d["state"] not in STATES or not any(p["id"] == d["profile"] for p in data["profiles"]):
                raise PrototypeError("Invalid Device state/profile.")
            if d["state"] != "missing":
                if d["slot"] in slots:
                    raise PrototypeError("Active Device slot is already occupied.")
                slots.add(d["slot"])
            point_ids = set()
            for q in d["points"]:
                if q["id"] in point_ids:
                    raise PrototypeError("Duplicate point ID.")
                point_ids.add(name(q["id"]))
                name(q["name"])
                flag(q["selected"])
                if q["quantity"] not in QUANTITIES or q["reference"] not in ("none", "gauge", "absolute", "differential"):
                    raise PrototypeError("Invalid point semantics.")
                if q["type"] == "boolean":
                    flag(q["value"])
                elif q["type"] == "real":
                    number(q["value"])
                else:
                    raise PrototypeError("Unsupported point type.")
        tag_ids, tag_names, roles, max_tag = set(), set(), set(), 0
        for t in data["tags"]:
            ident = t["id"]
            if not isinstance(ident, str) or len(ident) != 7 or ident[0] != "P" or not ident[1:].isascii() or not ident[1:].isdigit() or ident in tag_ids:
                raise PrototypeError("Duplicate/invalid process identity.")
            tag_ids.add(ident)
            max_tag = max(max_tag, int(ident[1:]))
            self.validate_tag(data, t)
            if t["name"].casefold() in tag_names:
                raise PrototypeError("Process names must be unique in this prototype.")
            tag_names.add(t["name"].casefold())
            role = t["usage"]["flow_role"]
            if role and role in roles:
                raise PrototypeError("A Flow role is already assigned. Remove its existing assignment first.")
            if role:
                roles.add(role)
        if data["next_device"] <= max_device or data["next_tag"] <= max_tag:
            raise PrototypeError("Identity counters would reuse an existing ID.")
        if not isinstance(data["events"], list):
            raise PrototypeError("Invalid binding audit.")
        for index, event in enumerate(data["events"], 1):
            if not isinstance(event, dict) or event.get("sequence") != index:
                raise PrototypeError("Invalid audit sequence.")
            for key in ("kind", "time", "message"):
                if not isinstance(event.get(key), str):
                    raise PrototypeError("Invalid audit record.")

    def event(self, data, kind, message, **fields):
        data["events"].append({"sequence": len(data["events"]) + 1,
                               "time": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                               "kind": kind, "message": message, **fields})

    def persist(self, candidate):
        if self.state_path is None:
            return
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.state_path.with_suffix(".tmp")
        try:
            with tmp.open("w", encoding="utf-8") as out:
                json.dump(candidate, out, indent=2, allow_nan=False)
                out.write("\n")
                out.flush()
                os.fsync(out.fileno())
            os.replace(tmp, self.state_path)
        finally:
            if tmp.exists():
                tmp.unlink()

    def snapshot(self):
        data = copy.deepcopy(self.data)
        now = time.monotonic()
        for tag in data["tags"]:
            d, q = self.resolve_point(data, tag["binding"])
            errors = self.compatibility(data, tag, tag["binding"])
            quality, reason = "GOOD", "Illustrative sample; no acquisition engine"
            if tag["binding"] is None:
                quality, reason = "UNAVAILABLE", "Unbound"
            elif not d or d["state"] == "missing" or not q:
                quality, reason = "UNAVAILABLE", "Device or point missing"
            elif d["state"] == "disabled":
                quality, reason = "DISABLED", "Device disabled"
            elif errors:
                quality, reason = "INCOMPATIBLE" if d["state"] == "incompatible" else "UNAVAILABLE", "; ".join(errors)
            elif d["state"] == "disconnected":
                quality, reason = "BAD", "Device disconnected"
            if quality == "GOOD":
                value = q["value"]
                c = tag["binding"]["conversion"]
                if c:
                    value = c["eu4"] + (value - 4) * (c["eu20"] - c["eu4"]) / 16
                self.samples[tag["id"]] = (tag["binding_revision"], value, now)
            sample = self.samples.get(tag["id"])
            if sample and sample[0] != tag["binding_revision"]:
                sample = None
            tag["live"] = {"quality": quality, "reason": reason, "value": sample[1] if sample else None,
                           "age_sec": round(now - sample[2], 1) if sample else None,
                           "source": f'{d["name"]} / {q["name"]}' if d and q else "Unbound or missing"}
        data.update(ok=True, simulated=True, label="P0 configuration prototype - not firmware",
                    flow_policy="Illustrative Flow 1: kPa, Celsius, gauge/absolute basis; optional separate reverse DP. No calculation or totalization.")
        return data

    def replacement_preview(self, target_id):
        target = self.device(self.data, target_id)
        if not target or target["profile"] != "p0_rosemount":
            raise PrototypeError("Choose a Rosemount fixture.")
        rows = []
        for tag in self.data["tags"]:
            d, q = self.resolve_point(self.data, tag["binding"])
            if d and d["profile"] == "p0_imv25" and q and q["id"] in ("dp", "pressure", "temperature"):
                binding = {"device_id": target_id, "point_id": q["id"], "conversion": None}
                errors = self.compatibility(self.data, tag, binding)
                rows.append({"tag_id": tag["id"], "name": tag["name"], "old": copy.deepcopy(tag["binding"]),
                             "new": binding, "compatible": not errors, "reasons": errors,
                             "usage": copy.deepcopy(tag["usage"])})
        if len(rows) != 3 or {r["new"]["point_id"] for r in rows} != {"dp", "pressure", "temperature"}:
            raise PrototypeError("Replacement requires exactly three IMV25 DP/pressure/temperature process bindings.")
        return {"ok": True, "revision": self.data["revision"], "rows": rows,
                "compatible": all(r["compatible"] for r in rows)}

    def write(self, payload):
        if not isinstance(payload, dict) or type(payload.get("revision")) is not int or payload["revision"] != self.data["revision"]:
            raise PrototypeError("Configuration changed elsewhere. Reload before saving; your draft is retained.")
        draft = copy.deepcopy(self.data)
        action = payload.get("action")
        if action in ("tag", "tag-create"):
            submitted = payload.get("tag")
            if not isinstance(submitted, dict):
                raise PrototypeError("Complete Tag draft required.")
            if action == "tag":
                old = self.tag(draft, submitted.get("id"))
                if old is None:
                    raise PrototypeError("Unknown process Tag.")
                candidate = copy.deepcopy(old)
            else:
                candidate = {"id": f'P{draft["next_tag"]:06}', "binding_revision": 1}
                old = None
            for key in ("name", "quantity", "units", "reference", "binding", "history", "alarms", "usage"):
                candidate[key] = copy.deepcopy(submitted[key])
            candidate["name"] = name(candidate["name"])
            if old and (candidate["quantity"], candidate["units"], candidate["reference"]) != (old["quantity"], old["units"], old["reference"]):
                raise PrototypeError("Existing process semantics are immutable in P0; create a new Tag for a different meaning.")
            self.validate_tag(draft, candidate)
            changed_binding = old is None or old["binding"] != candidate["binding"]
            if changed_binding:
                errors = self.compatibility(draft, candidate, candidate["binding"])
                if errors:
                    raise PrototypeError("Binding rejected: " + " ".join(errors))
                if old:
                    candidate["binding_revision"] += 1
                self.event(draft, "binding", f'{candidate["name"]}: new provenance segment; consumers preserved.',
                           tag_id=candidate["id"], binding_revision=candidate["binding_revision"],
                           old=old["binding"] if old else None, new=candidate["binding"])
            if old:
                if old["name"] != candidate["name"]:
                    self.event(draft, "rename", f'{old["name"]} renamed to {candidate["name"]}; identity preserved.', tag_id=candidate["id"])
                draft["tags"][draft["tags"].index(old)] = candidate
            else:
                draft["tags"].append(candidate)
                draft["next_tag"] += 1
        elif action == "device":
            device = self.device(draft, payload.get("device_id"))
            if not device or device["state"] == "missing":
                raise PrototypeError("Device missing. Recreate it with a new identity instead.")
            if payload.get("state") not in STATES:
                raise PrototypeError("Unknown Device state.")
            selected = payload.get("selected")
            if not isinstance(selected, list) or len(set(selected)) != len(selected) or not set(selected) <= {p["id"] for p in device["points"]}:
                raise PrototypeError("Invalid selected Device Tags.")
            bound = {t["binding"]["point_id"] for t in draft["tags"] if t["binding"] and t["binding"]["device_id"] == device["id"]}
            if not bound <= set(selected):
                raise PrototypeError("Unbind process Tags before deselecting their Device points.")
            if payload["state"] == "missing" and payload.get("confirm") != "DELETE":
                raise PrototypeError("Confirm DELETE; dependent Tags will become unavailable.")
            device["name"] = name(payload["name"])
            device["state"] = payload["state"]
            for q in device["points"]:
                q["selected"] = q["id"] in selected
            self.event(draft, "device", f'{device["name"]}: {device["state"]}; selections saved.', device_id=device["id"])
        elif action == "device-create":
            profile = payload.get("profile")
            if profile not in ("p0_imv25", "p0_rosemount"):
                raise PrototypeError("P0 creates only illustrative IMV25/Rosemount fixtures.")
            slot = name(payload["slot"])
            if any(d["slot"] == slot and d["state"] != "missing" for d in draft["devices"]):
                raise PrototypeError("Slot is occupied.")
            device = {"id": f'D{draft["next_device"]:06}', "slot": slot, "name": name(payload["name"]),
                      "profile": profile, "state": "online", "address": "Simulated RTU / Unit 1",
                      "points": transmitter_points()}
            draft["devices"].append(device)
            draft["next_device"] += 1
            self.event(draft, "device", "Device created with a new instance identity.", device_id=device["id"])
        elif action == "replace":
            if payload.get("confirm") != "REPLACE":
                raise PrototypeError("Confirm REPLACE after reviewing compatibility.")
            preview = self.replacement_preview(payload.get("target_id"))
            if not preview["compatible"]:
                raise PrototypeError("Replacement incompatible: select compatible Rosemount points and enable the Device first.")
            for row in preview["rows"]:
                tag = self.tag(draft, row["tag_id"])
                tag["binding"] = row["new"]
                tag["binding_revision"] += 1
                self.event(draft, "binding", f'{tag["name"]}: IMV25 replaced by Rosemount; new provenance segment.',
                           tag_id=tag["id"], binding_revision=tag["binding_revision"], old=row["old"], new=row["new"])
        else:
            raise PrototypeError("Unsupported prototype action; no equipment writes are available.")
        draft["revision"] += 1
        self.validate_state(draft)
        if payload.get("simulate_failure") is True:
            raise PrototypeError("Simulated save failure; prior committed state retained.")
        self.persist(draft)
        self.data = draft
        return self.snapshot()
