#!/usr/bin/env python3
"""Push RIOT device profiles from a local folder (e.g. a MutliFlowOTA clone) to a CYD test unit.

Each file goes through the unit's normal upload API, so it is validated and stored
byte-for-byte exactly as a browser upload would be. Driver profiles such as
riot_m31.toml are refused by the unit and reported as skipped.

Examples:
  python tools/sync_profiles.py 192.168.1.14 C:\\Users\\scada\\MutliFlowOTA\\toml
  python tools/sync_profiles.py riot-2A14.local ..\\MutliFlowOTA\\toml --pull --dry-run
  python tools/sync_profiles.py 192.168.1.14 profiles\\ scadapack*.toml --password secret

Standard library only. Exit code 0 when nothing failed (skipped driver profiles are fine).
"""
import argparse
import base64
import fnmatch
import json
import pathlib
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

MAX_BYTES = 48 * 1024  # the unit's upload limit


class Unit:
    def __init__(self, host, password=None):
        self.base = "http://" + host
        self.auth = None
        if password:
            self.auth = "Basic " + base64.b64encode(f"admin:{password}".encode()).decode()

    def request(self, path, data=None, method="GET", content_type=None):
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if content_type:
            req.add_header("Content-Type", content_type)
        if self.auth:
            req.add_header("Authorization", self.auth)
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def json(self, path):
        status, body = self.request(path)
        if status != 200:
            raise RuntimeError(f"GET {path} returned HTTP {status}")
        return json.loads(body)

    def upload(self, name, content):
        boundary = "----cydsync" + base64.b16encode(name.encode())[:16].decode()
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + content + f"\r\n--{boundary}--\r\n".encode()
        status, raw = self.request("/api/profiles", body, "POST", f"multipart/form-data; boundary={boundary}")
        try:
            return status, json.loads(raw)
        except ValueError:
            return status, {"error": raw.decode(errors="replace")[:200]}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("host", help="unit IP address or riot-XXXX.local")
    ap.add_argument("folder", type=pathlib.Path, help="folder holding .toml profiles")
    ap.add_argument("patterns", nargs="*", default=["*.toml"], help="file name filters (default: *.toml)")
    ap.add_argument("--pull", action="store_true", help="run 'git pull' in the folder first")
    ap.add_argument("--force", action="store_true", help="upload even when the unit already has an identical file")
    ap.add_argument("--dry-run", action="store_true", help="show what would be uploaded without changing the unit")
    ap.add_argument("--password", help="admin_password, if one is set on the unit")
    args = ap.parse_args()

    if not args.folder.is_dir():
        sys.exit(f"Folder not found: {args.folder}")
    if args.pull:
        print(f"git pull in {args.folder} ...")
        subprocess.run(["git", "-C", str(args.folder), "pull", "--ff-only"], check=True)

    files = sorted(p for p in args.folder.glob("*.toml")
                   if any(fnmatch.fnmatch(p.name, pat) for pat in args.patterns) and not p.name.endswith(".sim.toml"))
    if not files:
        sys.exit("No matching .toml files")

    unit = Unit(args.host, args.password)
    try:
        status = unit.json("/api/status")
        remote = {p["file"]: p for p in unit.json("/api/profiles")["profiles"]}
    except (OSError, RuntimeError) as e:
        sys.exit(f"Cannot reach the unit at {args.host}: {e}")
    print(f"{status['name']} firmware {status['firmware']}: {len(remote)} profile(s) on the unit, {len(files)} local file(s)\n")

    counts = {"uploaded": 0, "unchanged": 0, "skipped": 0, "failed": 0}
    restart = []
    for path in files:
        name, content = path.name, path.read_bytes()
        label = f"{name:40}"
        if len(content) > MAX_BYTES:
            print(f"{label} FAILED     larger than {MAX_BYTES // 1024} KB")
            counts["failed"] += 1
            continue
        if name in remote and not args.force:
            code, current = unit.request("/api/profiles/" + urllib.parse.quote(name))
            if code == 200 and current == content:
                print(f"{label} unchanged")
                counts["unchanged"] += 1
                continue
        if args.dry_run:
            print(f"{label} would upload ({'replace' if name in remote else 'new'}, {len(content)} bytes)")
            continue
        code, r = unit.upload(name, content)
        v = r.get("validation", {})
        if code == 200 and r.get("saved"):
            warns = v.get("warnings", [])
            print(f"{label} uploaded   {v.get('vendor', '')} {v.get('model', '')}: {v.get('mapped')}/{v.get('tags')} tags, "
                  f"address {v.get('addressMode')}" + (f", {len(warns)} warning(s)" if warns else ""))
            for w in warns:
                print(f"{'':40}   - {w}")
            counts["uploaded"] += 1
            used = remote.get(name, {}).get("usedBy", [])
            if used:
                restart.append(f"{name} (unit {', '.join(map(str, used))})")
        elif v.get("status") == "not_modbus":
            print(f"{label} skipped    not a Modbus profile: {v.get('message', '')[:90]}")
            counts["skipped"] += 1
        else:
            print(f"{label} FAILED     HTTP {code}: {v.get('message') or r.get('error', '')}")
            counts["failed"] += 1

    print("\n" + ", ".join(f"{n} {k}" for k, n in counts.items()))
    if restart:
        print("Replaced profiles in use; reboot the unit to serve the new versions: " + "; ".join(restart))
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
