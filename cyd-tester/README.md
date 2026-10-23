# CYD-TESTER

Cheap Yellow Display Modbus tester for the RIOT controller: a Modbus TCP server on an ESP32-2432S028R that emulates field devices described by RIOT Device Library TOML profiles. `PROMPT.md` has the full specification.

## Status

| Phase | Scope | State |
|---|---|---|
| 1 | Board bring-up: display, touch and calibration, RGB LED, BOOT button | Done, tested on hardware |
| 2 | Wi-Fi station, setup AP, captive portal, mDNS, BOOT 3 s / 10 s | Done, tested on hardware |
| 3 | Profile parser, address map, codec, host tests | Done: 8/8 host tests, all reference profiles load |
| 4 | Modbus TCP server with static values | Done: 19/19 host tests, live checks over Wi-Fi |
| 5 | Simulation engine, sim TOML writer, template generator | Done: 25/25 host tests, waveforms verified over Modbus |
| 6 | Web UI, REST API, WebSocket | Done: API and WebSocket checks pass, UI checked in Edge |
| 7 | Fault injection and diagnostics | Next |

Scope set by the user (2026-10-20):
- The unit only needs to serve Modbus responses.
- Not built: SD card traffic logging, OTA firmware updates, HTTPS "Fetch from URL". Profiles are uploaded through the web UI instead.

Sizes (env `cyd`): flash was 1.11 MB after phase 1, 1.24 MB after phase 2, 1.27 MB after phase 3, 1.29 MB after phase 4 and 1.30 MB after phase 5. Phase 6 uses RAM 54.3 KB of 320 KB and flash 1.39 MB of the 1600 KB app slot. The web UI is 7.5 KB gzipped.

## Pairing with a RIOT (no network needed)

The unit pairs itself with a RIOT so sales staff can just power both on:

1. On boot it scans. If the **saved network** is in range, it joins that; a manually chosen network always wins.
2. Otherwise it **pairs with the strongest RIOT service hotspot** (`Riot-XXXX`, the P4 Walkup AP):
   - password `riotwifi`, fixed address **192.168.4.200** (no DHCP needed);
   - the screen and dashboard show "paired" and "(RIOT pairing)".
3. If neither is in range, it opens its setup hotspot and keeps rescanning, so a RIOT switched on later is still found.

On the RIOT, set the device or M31 rack address to **192.168.4.200**, port 502, once. To use a bigger network instead, hold BOOT 3 s, join `riot-XXXX`, open http://192.168.5.1 and pick the network; it is saved and preferred from then on.

The `[pairing]` section of `unit.toml` holds these defaults:

```toml
[pairing]
enabled = true
ssid_prefix = "Riot-"     # case-sensitive; never matches a CYD's own "riot-XXXX" hotspot
password = "riotwifi"     # RIOT Walkup default
ip = "192.168.4.200"      # outside the RIOT hotspot's DHCP pool (about .2-.101)
netmask = "255.255.255.0"
gateway = "192.168.4.1"
```

- **Second CYD on the same RIOT:** set its `ip` to `192.168.4.201`.
- **Several RIOTs in range:** the unit pairs with the strongest signal, so keep one RIOT per CYD at a demo. To pin a specific RIOT, set `ssid_prefix` to its full name, e.g. `Riot-6183`.
- **Setup AP address:** the setup hotspot is at **192.168.5.1**, not the spec's 192.168.4.1, so it can never overlap the RIOT network the unit pairs with.
- **On-screen Setup AP button:** it needs a deliberate 0.5 s press, so a stray touch cannot open the hotspot.

## Web UI

Open `http://<unit IP>/` or `http://riot-XXXX.local/`. The setup AP side of `/` is the Wi-Fi portal.

- **Dashboard:** Wi-Fi, Modbus counters by function code and exception code, last request, and a "POLLED" indicator for the last 5 s.
- **Registers:** a live table per unit (WebSocket).
  - Tap a value to override it, or tap a bool to toggle it. Resume puts a tag (or every tag) back on the simulation.
  - Rows flash when a client reads them. Reads and the measured poll interval show which tags the RIOT actually polls.
- **Devices:** the running device map, and the saved map (applies after Reboot).
- **Profiles:**
  - Upload `.toml` with validation (warnings plus the ref -> wire table).
  - Stored byte-for-byte. Driver profiles such as `riot_m31.toml` are refused.
  - Validate, download, and delete (refused while a device uses the profile).
- **Simulation:** edit the sidecar, Generate template (placeholder ranges marked), and Save to apply live without a reboot.
- **Settings:** `unit.toml` editor with import, export, save and reboot, plus a link to Wi-Fi setup.

REST API (JSON; config and sims are TOML):
- `GET /api/status`
- `GET`/`POST /api/devices`
- `GET`/`POST /api/profiles`; `GET`/`DELETE /api/profiles/<file>`; `GET /api/profiles/<file>/validate`
- `GET`/`POST /api/values/<unit>`; `POST /api/values/<unit>/resume`
- `GET`/`PUT /api/sim/<unit>`; `GET /api/sim/<unit>/template`
- `GET`/`PUT /api/config`; `POST /api/reboot`
- WebSocket `/ws`: status and values at 2 Hz.

State-changing calls need HTTP Basic auth (user `admin`) when `admin_password` is set.

## Loading profiles from the MutliFlowOTA repo

`tools/sync_profiles.py` pushes profiles from a local folder, such as a clone of MutliFlowOTA, to the unit. It needs only the Python standard library.

```
python tools/sync_profiles.py 192.168.1.14 C:\Users\scada\MutliFlowOTA\toml
python tools/sync_profiles.py riot-2A14.local ..\MutliFlowOTA\toml --pull      # git pull first
python tools/sync_profiles.py 192.168.1.14 ..\MutliFlowOTA\toml "scada*" --dry-run
```

- Each file goes through the normal upload API, so it is validated and stored byte-for-byte, exactly like a browser upload.
- Identical files are skipped. Driver profiles (`riot_m31.toml`) are reported as skipped.
- Replaced profiles that a device is serving take effect after a reboot.
- `--force` re-uploads everything, `--password` handles `admin_password`, and the exit code is non-zero if anything fails.
- The unit itself never contacts GitHub. That would need an HTTPS client of about 124 KB, against about 10 KB of built-in profiles.

Memory: the profile and sim parsers feed toml++ one table at a time (`src/profiles/toml_chunks.*`), so peak memory is one tag's document rather than the whole file's. The 79-tag Brennan profile peaks at about 48 KB instead of 175 KB (PC figures; less on the ESP32). If a parse still runs out of heap, the API returns HTTP 507 instead of restarting the unit.

## Build and flash

```
pio run -e cyd -t upload        # board on COM14 (CH340)
pio device monitor -e cyd
```

- Host tests: `pio test -e native`. These need a host GCC, by default WinLibs at `C:\Users\scada\tools\winlibs-gcc-16.2.0` (set `CYD_HOST_GCC` to override). They parse every profile in `test/fixtures/` (copied from MutliFlowOTA/toml), check the spec's address and codec vectors, and exercise every Modbus function code and write rule.
- On first boot, LittleFS formats itself and touch calibration runs (tap three crosses). Hold the screen during power-up to recalibrate.
- Calibration is stored in `/config/touch.toml`.
- The built-in `test_bench` and `riot_m31_emulation` profiles and sims are embedded in the firmware and written to `/profiles/` and `/sims/` only when missing, so a fresh unit works without a filesystem upload.
- Default device map: unit 1 = `test_bench`, unit 2 = `riot_m31_emulation`.
- `riot_m31_emulation` answers like a RIOT M31 I/O rack:
  - DI1-4 on FC2 and DO1-4 on FC1 (written with FC15), at wire 0-3.
  - AI1-4 on FC4 and AO1-4 on FC3 (written with FC6), at wire 0-3, as uint16 microamps.
  - Point a RIOT "M31 Rack" at this unit's IP and the matching unit ID.
- `GET /api/config` downloads `unit.toml` with passwords shown as `********`. `PUT /api/config` uploads it (validated; a `********` keeps the saved password). `POST /api/reboot` applies it. If `admin_password` is set, both write endpoints need HTTP Basic auth (user `admin`).

## Hardware notes

- All pins are in `include/board_pins.h`. `build_flags` force-includes it, so TFT_eSPI is configured without editing the library.
- Partitions (`partitions.csv`): two 1600 KB OTA app slots, 768 KB LittleFS, nvs, otadata and coredump.

## Assumptions

Each item is to be confirmed with the user (see PROMPT.md §14), except where marked confirmed.

- Confirmed: the RIOT polls this unit using Modbus TCP only (port 502 over Wi-Fi), even though the reference profiles say `transport = "rtu"`.
- The Modbus TCP server is our own small AsyncTCP implementation, not eModbus `ModbusServerTCPasync` as the spec suggests. eModbus answers unknown unit IDs with an error, and sends a malformed zero-length frame for "no reply". Its synchronous workers also can't delay replies, alter transaction IDs, or close after N requests, which the phase 7 faults need. Request handling is transport-free and covered by host tests.
- A `read_write` tag with no `write_function` accepts any suitable write function code, even with `strict_function_codes = true`. Strict mode only enforces a declared `write_function`.
- A broadcast write (unit ID 0) is applied to every enabled device, and never answered.
- Each tag starts at its sim `initial` value, or else the middle of `min`/`max`, or else 0.
- Sim modes where the spec leaves details open:
  - `step` climbs from `min` to `max` in 5 equal steps per `period_s`, then restarts.
  - `toggle` is on (`max`, or 1) for the first half of `period_s` and off (`min`, or 0) for the second.
  - `ramp` is a sawtooth from `min` to `max`.
  - `counter` adds 1 per update from `initial` (or `min`), wrapping to `min` after `max`.
  - `noise` is clamped to `min`/`max` for every mode except `static`.
- "Generate sim template" settings:
  - Writable tags use `echo`.
  - Bits and unscaled integers without units (codes and settings) use `static`.
  - Other measurements use `sine` over a 0-100 range marked `# PLACEHOLDER`, to be replaced with real limits.
- `modbus-test` / `riotwifi` are the default station credentials (the network the unit joins). The `riot-XXXX` setup AP is open.
- Profiles that state no address basis use the register number unchanged on the wire, and are flagged "address basis not stated".
- The first test unit has no light sensor fitted (`HAS_LDR 0`). The `ldr` simulation mode falls back to a static value with a warning. Set `HAS_LDR 1` on boards that have one.
- Arduino core 2.0.14 (espressif32 6.5.0) is used, because TFT_eSPI is not reliable on core 3.x.
- The portal's "Save & connect" tries the new network before saving it. A failed attempt reports the reason and keeps the previous settings.
- A setup AP opened from the BOOT button or touch while the station is connected stays up while a phone is connected, and closes after 10 idle minutes. After a successful portal setup the AP always closes after 60 s, as the spec says.
- Station retries pause while a phone is connected to the setup AP, because retries change channel and would drop the phone.
- Wi-Fi modem-sleep is off. The RIOT opens a new TCP connection for every poll with a 200 ms connect timeout. With power saving on, the ESP32's connection reply can take 100-300 ms, and polls fail as `CONNECT_FAIL`.
