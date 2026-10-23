# Riot I/O Tester — ESP32-2432S028 / CYD v3

Portable M31 I/O commissioning tool built around the inexpensive ESP32-2432S028 dual-USB touchscreen board.

## Current status

**Working bench build. Suitable for field testing.**

The tester currently provides:

- M31 Modbus ONLINE/OFFLINE status
- AI1–AI8 live mA display
- DI1–DI4 live status
- DO1–DO4 touch and browser control
- AO1–AO4 readback/manual force controls
- Modbus good/bad/timeout/CRC diagnostics
- Wi-Fi captive portal and browser dashboard
- Runtime Modbus slave address selection from the browser dashboard
- Touchscreen pages: AI / DI / DO / AO / MB
- Direct ST7789 rendering with change-driven display updates

There is still a small amount of visible LCD activity on some updates. It is good enough for current field use and can be tuned later. Do not disturb the working LCD orientation/touch configuration unless there is a real problem.

## Hardware actually used

The tested board is the **dual-USB CYD v3 variant**:

- ESP32-WROOM-32
- 2.8-inch 320x240 display
- **ST7789 display controller**
- XPT2046 resistive touch controller
- USB-C + micro-USB connectors
- no PSRAM required

This is important: earlier CYD examples commonly assume an ILI9341. That driver was wrong for this board and caused rotation/overlay/right-edge artifacts. The working build uses ST7789.

### Working display configuration

- driver: `ST7789_DRIVER`
- native controller geometry: 240x320
- physical landscape UI: 320x240
- rotation: `tft.setRotation(3)`
- `TFT_RGB_ORDER=TFT_BGR`
- `TFT_INVERSION_OFF`
- direct rendering to the TFT — no sprite/framebuffer
- change-driven live-value refresh

Expected serial banner:

```text
LCD ST7789 direct r3 change-driven 320x240
```

## Touch configuration

Touch uses a separate HSPI bus and must remain separate from the TFT SPI bus.

Pins:

| Function | GPIO |
|---|---:|
| Touch IRQ | 36 |
| Touch MISO | 39 |
| Touch MOSI | 32 |
| Touch CLK | 25 |
| Touch CS | 33 |

Working measured calibration:

```cpp
x=map(constrain(p.x,250,3425),3425,250,0,319);
y=map(constrain(p.y,490,3540),3540,490,0,239);
```

Five-point measurements that established the mapping:

```text
TOP LEFT     rawX=3403 rawY=3502
TOP RIGHT    rawX=156  rawY=3574
CENTER       rawX=1889 rawY=1996
BOTTOM LEFT  rawX=3446 rawY=396
BOTTOM RIGHT rawX=336  rawY=588
```

Do not change the touch mapping just because the LCD orientation changes; the current combination is bench-proven.

## RS485 wiring

The tester uses the P3 header:

| CYD P3 | Function |
|---|---|
| GND | RS485 module GND |
| GPIO35 / RX | RS485 RO / RX |
| GPIO22 / TX | RS485 DI / TX |
| GPIO21 | Do not use — LCD backlight |

UART configuration:

- TX = GPIO22
- RX = GPIO35
- 9600 baud
- 8N1
- default Modbus slave address = 1

GPIO35 is input-only and is well suited to RX.

The common bench arrangement uses an HW-0519 auto-direction RS485 interface, so no DE/RE GPIO is needed.

## Modbus address selection

The browser dashboard can change the active slave address at runtime.

- valid range: 1–247
- default after boot: 1
- changing the address immediately redirects all reads/writes to the selected slave
- diagnostics reset when the address changes
- the address is **runtime-only** and is not written to permanent storage
- this changes which slave the tester talks to; it does **not** change the address stored inside the M31 device

The address control is intentionally on the browser dashboard rather than the small touchscreen.

## LCD pages

Touch tabs along the bottom:

- **AI** — AI1–AI8 live mA
- **DI** — DI1–DI4 status
- **DO** — DO1–DO4 manual control
- **AO** — AO1–AO4 readback and ±10% force controls
- **MB** — active slave address, Modbus statistics, Wi-Fi/IP and last error

The current screen colors/layout are considered good. Preserve them unless there is a specific reason to change them.

## Wi-Fi

The firmware uses WiFiManager.

If no saved network connects, join:

```text
Riot-IO-Tester
```

and complete the captive portal setup.

Once connected, open the IP shown on the LCD or serial monitor.

## Browser dashboard

The browser interface provides the same I/O information plus Modbus address selection.

Endpoints:

- `GET /` — dashboard
- `GET /api/state` — JSON state and diagnostics
- `POST /api/do` — `ch=1..4`, `on=0|1`
- `POST /api/ao` — `ch=1..4`, `pct=0..100`
- `POST /api/modbus-address` — `address=1..247`

## Current firmware / milestone

Current working firmware series:

```text
Riot-IOTEST-CYD-v23+
```

Important milestone commits:

- `755d64f` — switch CYD v3 display driver to ST7789
- `c807360` — native ST7789 landscape rendering
- `52f2cac` — correct landscape orientation using rotation 3
- `009bbbd` — set ST7789 inversion mode
- `6176ab9` — remove 4-bit sprite and render directly to ST7789
- `932435f` — partial live-value refresh
- `48a9399` — runtime Modbus address selector on web dashboard
- `fa1d69e` — change-driven display refresh

## Build and upload

From PowerShell:

```powershell
cd C:\Users\rob\MultiFlow
git pull

cd tools\riot_io_tester_2432s028

%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -d . -e riot_io_tester_2432s028 -t upload
```

Serial monitor:

```powershell
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe device monitor -b 115200
```

The portable tester has commonly appeared as COM7 during bench work.

## M31 mapping status

The tester currently uses the preliminary M31 map already built into the code:

- DI: function 02, start 0, qty 4
- DO: function 01, start 0, qty 4
- AI1–AI4: function 04, registers 0–3
- AI5–AI8: function 04, registers 4–7
- AO readback: function 03, registers 0–3
- DO write: function 05
- AO write: function 06
- analog scaling currently assumes raw value / 1000 = mA

Before relying on outputs in a real installation, compare/validate this mapping against the proven MultiFlow M31 implementation and test AO with a meter.

## Recommended field test order

1. Power the tester and confirm the screen is upright and stable.
2. Confirm Wi-Fi/dashboard access if needed.
3. Connect RS485 TX22 / RX35 / GND.
4. Set the correct Modbus slave address from the dashboard.
5. Confirm M31 ONLINE.
6. Check AI values.
7. Exercise DI inputs.
8. Test one DO only when safe.
9. Test AO with a meter before connecting to process equipment.
10. Note any remaining LCD shimmer, but do not tune it in the field unless it affects usability.

## Safety

This is a commissioning/test tool. DO and AO controls directly issue Modbus output commands. Confirm an output is safe to operate before commanding it from either the touchscreen or browser dashboard.

## Next possible improvements

Not required before field testing:

- further reduce the remaining occasional LCD shimmer
- validate/finalize M31 register map and analog scaling against the main MultiFlow implementation
- improve Modbus ONLINE/OFFLINE logic using consecutive failures rather than cumulative counters
- optionally remember the selected Modbus address in NVS, if that becomes desirable
- optional cosmetic cleanup of the MB diagnostics page
