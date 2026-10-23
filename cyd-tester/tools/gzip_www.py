# PlatformIO pre-script for env:cyd: gzip the web UI so the firmware can embed it.
# Output is deterministic (mtime 0), so files only change when the source does.
import gzip
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

www = os.path.join(env.subst("$PROJECT_DIR"), "data", "www")  # noqa: F821
total = 0
for name in ("index.html", "app.js", "style.css"):
    src = os.path.join(www, name)
    with open(src, "rb") as f:
        packed = gzip.compress(f.read(), compresslevel=9, mtime=0)
    dst = src + ".gz"
    old = open(dst, "rb").read() if os.path.exists(dst) else None
    if old != packed:
        with open(dst, "wb") as f:
            f.write(packed)
    total += len(packed)
print(f"Web UI: {total} bytes gzipped")
