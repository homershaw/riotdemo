# Build: RIOT Modbus TCP Test Unit (ESP32-2432S028 "Cheap Yellow Display")

## 1. Goal

Build PlatformIO / Arduino firmware for an **ESP32-2432S028** that acts as a **Modbus TCP server (slave) over Wi-Fi**. It is a bench tool for testing a RIOT controller, which polls it as a Modbus client (master).

The unit **emulates field devices described by RIOT Device Library TOML profiles**. These are the exact same `.toml` files that get uploaded to the RIOT. Load `scadapack_4102_metric.toml` and the unit answers polls exactly where the RIOT expects a SCADAPack 4102: same function codes, same addresses, same data types, word order and scaling. Simulated values, fault injection and a traffic log let the user verify the RIOT's polling, decoding, alarming and timeout handling without real field hardware.

Reference profiles live in https://github.com/homershaw/MutliFlowOTA/tree/main/toml. Clone that repo and use every file in `toml/` as a parser test fixture (see §12).

**All configuration files on the device are TOML. Do not use JSON for anything the user edits, imports or exports.** JSON is acceptable only as the internal wire format between the web UI and the firmware's HTTP/WebSocket API.

---

## 2. Hardware: ESP32-2432S028R (single-USB, ILI9341 variant)

Use these pins and **put them all in one `board_pins.h`**, so the dual-USB / ST7789 variant can be supported later by changing one file.

| Function | Pins |
|---|---|
| Display ILI9341 (HSPI) | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, RST -1, Backlight 21 (PWM) |
| Touch XPT2046 (**separate** SPI bus, VSPI) | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| RGB LED (**active LOW**) | R 4, G 16, B 17 |
| Light sensor (LDR) | GPIO 34 (ADC1) |
| SD card (VSPI default) | SCK 18, MISO 19, MOSI 23, CS 5 |
| BOOT button | GPIO 0 (active LOW, usable at runtime) |
| Speaker | GPIO 26 (optional, not required) |

- The flash is 4 MB and there is **no PSRAM**. Keep heap use modest and parse large profiles into compact structs.
- The touch controller does **not** share the display's SPI bus. Use `XPT2046_Touchscreen` on its own `SPIClass(VSPI)`. Store touch calibration in `/config/touch.toml`, and provide a calibration screen that runs on first boot or when the screen is held during power-up.

---

## 3. Toolchain and libraries

- **PlatformIO**, `framework = arduino`, `board = esp32dev`. Build with `-std=gnu++17`.
- Libraries. Pin exact versions in `platformio.ini`.
  - **eModbus**: use `ModbusServerTCPasync` for the Modbus TCP server.
  - **ESPAsyncWebServer** + **AsyncTCP** (the maintained mathieucarbou forks) for the web UI, REST API and WebSocket.
  - **DNSServer** (core) for the captive portal.
  - **TFT_eSPI**: configure it entirely through `build_flags` in `platformio.ini`. Never edit the library's `User_Setup.h`.
  - **XPT2046_Touchscreen** for touch.
  - **toml++ (tomlplusplus)**, header-only, compiled with `TOML_EXCEPTIONS=0`. Use it for parsing. Write TOML output with a small hand-written emitter, so the format stays clean and stable.
  - **qrcode** (ricmoo) for the on-screen QR code.
  - **LittleFS** (core) for storage.
  - ESPmDNS and Update / ArduinoOTA (core).
- **Do not** use tzapu WiFiManager. Its synchronous WebServer conflicts with the async stack. Write a small captive portal on ESPAsyncWebServer + DNSServer instead.
- Create a custom `partitions.csv`: two OTA app slots of about 1.6 MB each, plus a LittleFS partition with the remainder (about 700 KB), plus nvs/otadata. Check that the firmware fits, and report the actual sizes.
- Add a `native` PlatformIO environment for host-side unit tests (Unity) of the profile parser, address conversion and value codec (§12).

---

## 4. Wi-Fi behaviour

### 4.1 Station (normal operation)
- Default credentials, compiled in and also written to `/config/unit.toml` on first boot:
  - SSID **`modbus-test`**
  - password **`riotwifi`**
- On boot, try the saved credentials for `connect_timeout_s` (default 20 s).
- Support DHCP **or a static IP** (IP, mask, gateway, DNS). The RIOT polls a fixed address, so static IP must be easy to set in both the portal and the web UI.
- Register mDNS hostname **`riot-XXXX.local`**.
- If the connection drops, reconnect with backoff. If it hasn't reconnected after `portal_after_s` (default 120 s), also start the setup AP while it keeps retrying.

### 4.2 Setup AP and captive portal
- Start the AP when there are no credentials, when the station connection fails, or when the user requests it (BOOT button or touch).
- AP SSID: **`riot-XXXX`**, where XXXX is the last 4 hex characters of the station MAC address, in uppercase.
  - The AP is open by default. An optional `ap_password` can be set in `unit.toml`.
- AP IP: **`192.168.4.1`**. Run a DNS server that answers every name with 192.168.4.1.
- Answer the OS captive-portal probes so the portal opens automatically on iOS, macOS, Android and Windows:
  - `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/library/test/success.html`, `/ncsi.txt`, `/connecttest.txt`, `/redirect`, `/fwlink`
  - Any unknown host or path redirects to `http://192.168.4.1/`.
- Portal page:
  - Scan and list networks with RSSI and lock icons, plus a manual SSID field and a password field.
  - DHCP/static IP fields.
  - "Save & connect" button. Show the result: success with the new IP, or failure with the reason.
- After a successful connection, keep the AP up for 60 s so the phone can show the new IP, then turn it off.
- The on-device screen shows the portal SSID, `192.168.4.1`, and a Wi-Fi QR code (`WIFI:T:nopass;S:riot-XXXX;;`) so a phone can join by scanning.

### 4.3 BOOT button (GPIO 0)
- Hold **3 s**: start the setup AP and portal.
- Hold **10 s**: factory reset of network settings. This restores the `modbus-test` / `riotwifi` defaults and **keeps profiles**.
- The screen shows a countdown while the button is held.

---

## 5. Device profiles: the RIOT TOML schema

The firmware must read **unmodified RIOT profile files**. It must never require extra keys in them, and must never rewrite them. Simulation settings go in a separate sidecar file (§6).

### 5.1 Schema observed in the reference files (`schema_version = 1` and `2`)

```toml
schema_version = 2

[device]
profile_id = "scadapack_4102_metric"
profile_version = 1
vendor = "Schneider Electric"
model = "SCADAPack 4102"
description = "..."

[modbus]
transport = "rtu"                # informational here: we always serve Modbus TCP
unit_id = 1                      # optional
address_format = "reference"     # or "zero_based"; optional
address_basis = "one_based"      # alternative key seen in some files; optional
short_packing = "AB"             # optional: 16-bit byte order, "AB" or "BA"
packing = "..."                  # optional, may appear
long_packing = "..."             # optional, may appear
default_baud = 9600              # ignore (RTU only)
baud = 9600                      # ignore (RTU only)

[[tags]]
id = "dp"
name = "Differential Pressure"
description = "..."              # optional
read_function = 3                # 1, 2, 3, 4. Older files use `function` instead
write_function = 6               # optional: 5, 6, or 16
register = 40401
data_type = "float32"            # "bool", "uint16", "int16", "float32"
word_order = "HIGH_LOW"          # float32 only: HIGH_LOW (ABCD) or LOW_HIGH (CDAB)
scale = 1.0                      # optional, default 1.0
units = "kPa"                    # optional
access = "read"                  # "read" or "read_write"
default_enabled = false
poll_group = "process"
pool_group = "Measurements"      # display grouping only
```

A profile with `[driver]` and no `[modbus]`/`[[tags]]` (for example `riot_m31.toml`, `type = "m31"`) is **not a Modbus profile**. Reject it with a clear message, not a crash.

### 5.2 Parsing rules
- Function code: use `read_function`. If it's absent, fall back to `function`. If both are absent, load the tag as **unmapped**, show it in the UI with a warning, and skip it.
- Unknown keys anywhere: ignore them and list them as warnings. Unknown `data_type` or `word_order`: skip the tag with a warning.
- `scale`: engineering value = raw × scale. When serving, raw = round(eng ÷ scale) for integer types. For float32 the scaled value is sent as an IEEE-754 float.
- `default_enabled` and `poll_group` don't affect serving, but show them in the register browser.
- Keep every `#` comment block out of RAM after parsing. Retain only what's needed to serve and to display.

### 5.3 Address conversion: profile register → on-wire address
| Mode | Rule |
|---|---|
| `address_format = "reference"` | FC1/5/15: `reg − 1` (0xxxx). FC2: `reg − 10001`. FC4: `reg − 30001`. FC3/6/16: `reg − 40001`. Also accept 6-digit references (`400001` → 0, and so on). Example: 40401 → 400. |
| `address_basis = "one_based"` | `reg − 1` |
| `address_format = "zero_based"` | `reg` unchanged |
| none of the above | `reg` unchanged, **and flag the profile with "address basis not stated"** |

- If a reference number doesn't match the function code (for example a 3xxxx register on an FC3 tag), load the tag but warn.
- Show the full ref → wire table in the web UI. The user needs it to check the RIOT's addressing.

### 5.4 Data encoding
- `bool`: a single coil (FC1) or discrete input (FC2).
- `uint16` / `int16`: one register. Honour `short_packing = "BA"` by swapping bytes.
- `float32`: two consecutive registers.
  - `HIGH_LOW` = ABCD, high word first.
  - `LOW_HIGH` = CDAB, word-swapped.
  - If `packing` / `long_packing` specify byte orders (`ABCD`, `CDAB`, `BADC`, `DCBA`), support all four. Precedence: tag `word_order` > `long_packing`/`packing` > default ABCD.
- Coils, discrete inputs, input registers and holding registers are **four separate address spaces**. The same number can exist in more than one.

### 5.5 Multiple emulated devices
`/config/unit.toml` maps unit IDs to profiles. One IP address then emulates several devices, as if they sat behind a serial-to-TCP gateway. Each device has its own register space.

---

## 6. Simulation sidecar: `<profile_id>.sim.toml`

This file is optional, lives next to the profile, and is matched by `profile_id`. Any tag without an entry holds a static value (its `initial` value, or 0).

```toml
schema_version = 1
profile_id = "scadapack_4102_metric"

[[sim]]
tag = "dp"            # tag id from the profile
mode = "sine"         # static | ramp | sine | random | step | toggle | counter | echo | ldr
initial = 12.5        # engineering units
min = 0.0
max = 25.0
period_s = 60         # sine / ramp / step / toggle period
noise = 0.1           # optional ± random noise added each update
update_ms = 1000
```

- `echo` means the value only changes when the RIOT writes it. Use it for setpoints.
- `ldr` is a live reading from the onboard light sensor (GPIO 34, raw ADC 0–4095, then scaled). It gives one genuinely physical value you can cover with a finger to watch it change on the RIOT.
- Any value written by the RIOT overrides the simulation until "Resume simulation" is pressed for that tag (web or touch).
- The web UI edits sims live and has a **Save** button that writes the sidecar file back as TOML.
- Provide **"Generate sim template"**: create a `.sim.toml` from a profile with sensible defaults for every tag. It must use the min/max ranges, the units, and `static` for config/code registers. Real-unit ranges are not in the profile, so leave them as clearly marked placeholder defaults the user edits.

---

## 7. Unit configuration: `/config/unit.toml`

```toml
schema_version = 1

[wifi]
ssid = "modbus-test"
password = "riotwifi"
ip_mode = "dhcp"            # or "static"
static_ip = "192.168.1.50"
netmask = "255.255.255.0"
gateway = "192.168.1.1"
dns = "192.168.1.1"
connect_timeout_s = 20
portal_after_s = 120

[portal]
ap_password = ""            # empty = open AP

[modbus]
port = 502
max_clients = 4
respond_to_any_unit_id = false   # true = answer every unit ID with device[0]
unmapped_read = "exception"      # "exception" (02) | "zero"
strict_function_codes = true     # reject FCs a tag does not declare (exception 01)
idle_timeout_s = 60

[[devices]]
unit_id = 1
profile = "scadapack_4102_metric.toml"
sim = "scadapack_4102_metric.sim.toml"   # optional
enabled = true

[web]
admin_password = ""         # if set: protects config, OTA, uploads

[display]
rotation = 1
brightness = 80
sleep_after_s = 0           # 0 = never
```

- Profiles are stored in `/profiles/`, sims in `/sims/`, config in `/config/`.
- On export, **redact passwords** (`password = "********"`). On import, keep the existing password if the imported value is the redacted string.

---

## 8. Modbus server behaviour

- Function codes: **01, 02, 03, 04, 05, 06, 15, 16**. Also implement **08 sub-function 0** (echo) and **43/14** (device identification: vendor and model from the profile, plus the firmware version).
- A request spanning several tags is fine. A float read as two registers in one transaction must be atomic: never return half of an old value and half of a new one.
- Writes:
  - Allowed only to `access = "read_write"` tags. Anything else returns exception 02.
  - With `strict_function_codes = true`, writes must also use the tag's declared `write_function`. FC16 on a tag that declares only FC6 returns exception 01.
  - FC16 on a float32 must cover both registers. A partial write returns exception 02.
- Unmapped addresses follow `unmapped_read`. Unmapped writes always return exception 02.
- Unit ID 0 is broadcast: write, no reply.
- A request to an unknown unit ID gets **no reply**, like a dead device behind a gateway (unless `respond_to_any_unit_id` is set).

### 8.1 Fault injection
Settable live from the web UI and from a touch screen page. Every fault is **per device**, plus one global master switch. Faults reset to **off** on reboot unless "persist faults" is ticked.

- Response delay: fixed ms, plus a random jitter ms.
- Drop X % of requests (no reply).
- Return exception code N for: all requests, a chosen tag, or every Nth request.
- Wrong transaction ID or wrong unit ID in the reply.
- Close the TCP connection after N requests, or after T seconds.
- Freeze values: simulation stops and values hold. This mimics a stale device.
- "Device offline": no reply for this unit ID at all.
- Bad float: replace one tag's value with NaN or ±Inf.

---

## 9. Diagnostics (the main point of the tool)

- **Traffic log**: ring buffer of the last 200 frames. Each entry has:
  - timestamp, client IP, transaction ID, unit ID, FC, start address, count
  - result (OK / exception code / dropped / fault-injected)
  - response time
  - raw request and response bytes in hex
  - decoded tag names
- **Unmapped-request detector**: when a request hits an unmapped address, log it and look for a tag at ±1 address or in another function code's space. Show a hint, for example: "Read FC3 @ 401 not mapped. Tag `dp` is at wire 400. Possible off-by-one / reference-vs-zero-based error." This is the most common commissioning problem, so make it prominent.
- **Poll coverage**: for each tag, show the last read time, read count, poll interval (computed) and last write. Highlight tags the RIOT never reads.
- **Counters**: requests, by FC, exceptions by code, drops, active and total clients, uptime, free heap, RSSI.
- **SD card logging** (optional, when a card is present): append the traffic log as CSV to `/logs/YYYYMMDD.csv`, rotating at 5 MB. Use NTP time if available, otherwise uptime.

---

## 10. On-device UI (320×240 touch)

Use a bottom tab bar with large touch targets (at least 40 px). The screens are:

1. **Status**:
   - SSID, IP, RSSI, `riot-XXXX.local`
   - active devices (unit ID → model)
   - requests/s, last client IP, last FC/address, error count
   - a large green/red "polled in last 5 s" indicator
2. **Registers**: pick a device, then a scrollable tag list showing id, wire address, value and units. Tap a tag to edit its value with a numeric keypad, or toggle a bool. Tags polled recently flash briefly.
3. **Faults**: quick toggles for delay, drop %, offline and freeze, per device.
4. **Log**: the last ~10 frames, compact.
5. **Setup**: a QR code for the web UI URL, the firmware version, a "start setup AP" button and a touch calibration button.

The RGB LED flashes for 30 ms on each poll: green for OK, red for an exception or drop, blue in AP/portal mode, slow yellow blink when no Wi-Fi. Dim the backlight after `sleep_after_s`, and wake it on touch.

---

## 11. Web UI and API

The web UI is a single-page app in plain HTML/JS/CSS, with no build step and no CDN (the unit may be offline). Serve it **gzipped from LittleFS** and keep the total under ~80 KB. It must be usable on a phone.

Pages:
- **Dashboard**: the status and counters from §9, live over WebSocket.
- **Devices**: unit ID → profile mapping, plus enable/disable.
- **Profiles**:
  - upload `.toml` (with validation results shown: errors, warnings, the ref→wire table), download, delete
  - "Fetch from URL", e.g. a raw GitHub link, used when the station has internet access
- **Registers**: live table per device (tag, FC, wire address, raw hex, decoded value, units, sim mode, last read). Values and sims can be edited inline.
- **Simulation**: edit the sidecar, generate a template, save.
- **Faults**: everything in §8.1.
- **Traffic**: the log with filters, pause, and CSV download.
- **Settings**: Wi-Fi/static IP, `unit.toml` import/export (TOML), admin password, and display settings.
- **Firmware**: OTA upload (`.bin`) with a progress bar. Also LittleFS image upload, version info, reboot, and factory reset.

REST API, JSON over HTTP, under `/api/...`:
`status`, `devices`, `profiles` (GET/POST/DELETE), `profiles/{id}/validate`, `values/{unit}` (GET; PUT for overrides), `sim/{unit}`, `faults`, `log`, `config` (GET/PUT, with passwords redacted), `ota`, `reboot`. WebSocket at `/ws` pushes value, counter and log updates at most 4 times per second.

If `admin_password` is set, HTTP Basic auth protects every state-changing endpoint, OTA and uploads. Read-only pages stay open.

---

## 12. Testing and acceptance

**Host-side unit tests** (`pio test -e native`):
- Parse **every file** in `MutliFlowOTA/toml/`.
  - All Modbus profiles must load with zero errors (warnings allowed).
  - `riot_m31.toml` must be rejected cleanly as a non-Modbus profile.
  - Print a per-file summary (tag count, warnings).
- Address conversion: 40401 → 400 (reference, FC3), 30001 → 0 (FC4), 10001 → 0 (FC2), 1 → 0 (FC1), 401 → 400 (one_based), 401 → 401 (zero_based).
- Codec: 12.5 float32 encodes as `0x4148 0x0000` for HIGH_LOW and `0x0000 0x4148` for LOW_HIGH. Also round-trip int16 −1, uint16 65535 and `short_packing = "BA"`, and test scale 0.01.

**Host-side integration script** (`tools/modbus_selftest.py`, using pymodbus):
- Given the unit's IP and a profile `.toml`, read every tag the way the RIOT would (same FC, wire address, count, word order).
- Decode the values and compare them with `GET /api/values/{unit}`.
- Write to every `read_write` tag and read it back.
- Run each fault mode and confirm the expected client-side result (timeout, exception code, disconnect).
- Exit non-zero on any mismatch.

**Acceptance criteria**:
1. On a fresh flash, the unit tries `modbus-test`/`riotwifi`. When that fails, `riot-XXXX` appears and the portal opens automatically on a phone. If it doesn't, `http://192.168.4.1` works.
2. After portal setup, the unit joins the network and shows its IP on screen. `riot-XXXX.local` resolves.
3. Upload `scadapack_4102_metric.toml` and map it to unit 1. A RIOT, or `modbus_selftest.py`, configured with the same profile reads correct DP/SP/T values that match the screen.
4. Two devices at unit IDs 1 and 2 with different profiles can be polled at the same time by 2 clients.
5. A deliberately off-by-one poll produces the off-by-one hint in the traffic log.
6. OTA update through the web UI works, and the settings and profiles survive it.
7. Leave the unit polled at 10 Hz by 2 clients for 1 hour, 2 devices loaded, sims running. There must be no reboot or watchdog trigger, and free heap must stay stable (log the minimum free heap).

---

## 13. Project layout

```
platformio.ini
partitions.csv
include/board_pins.h
src/main.cpp
src/net/        wifi_manager, captive_portal, mdns
src/modbus/     server, address_map, codec, faults, traffic_log
src/profiles/   profile_parser (toml++), sim_engine, toml_writer, unit_config
src/ui/         display, touch, screens/*, led
src/web/        web_server, api, websocket, ota
data/www/       index.html, app.js, style.css (gzipped by a pre-build script)
data/profiles/  test_bench.toml
data/sims/      test_bench.sim.toml
data/config/    unit.toml (defaults)
test/           native unit tests + fixtures (copied from MutliFlowOTA/toml)
tools/          modbus_selftest.py, gzip_www.py
README.md       flashing, first-time setup, usage, TOML formats
```

---

## 14. Working instructions for Claude Code

- Build in phases and **make sure each phase compiles and passes its tests before moving on**:
  1. Board bring-up: display, touch, LED, button.
  2. Wi-Fi station + captive portal.
  3. Profile parser + native tests.
  4. Modbus server with static values.
  5. Simulation engine.
  6. Web UI + API.
  7. Fault injection + diagnostics.
  8. OTA, SD logging, polish.
- Keep the Modbus server responsive.
  - No blocking calls, file I/O or display drawing inside Modbus request callbacks.
  - Protect the shared value store with a mutex or a lock-free snapshot.
  - Run display drawing in its own FreeRTOS task with a lower priority than networking.
- Don't guess about the RIOT's behaviour. Where this document is ambiguous, pick the safest reasonable option, write it under "Assumptions" in the README, and carry on.
- Don't modify or reformat uploaded RIOT profile files. Store them byte-for-byte.
- Report the flash and RAM usage after each phase.

### Assumptions to confirm with the user (list them in the README)
- The RIOT polls this unit using **Modbus TCP** (port 502) over Wi-Fi, even though the reference profiles say `transport = "rtu"`.
- `modbus-test` / `riotwifi` are the default **station** credentials, i.e. the network the unit joins. The setup AP `riot-XXXX` is open.
- Profiles that state no address basis use the register number unchanged on the wire.
