# Riot I/O Tester — Continuation Context

Use this note when returning to development in a new ChatGPT conversation.

## Project

Repository: `homershaw/MultiFlow`

Branch: `feature/multiflow-common-core`

Tool path:

```text
tools/riot_io_tester_2432s028
```

Local Windows path:

```text
C:\Users\rob\MultiFlow\tools\riot_io_tester_2432s028
```

PlatformIO environment:

```text
riot_io_tester_2432s028
```

## Current state

The portable ESP32-2432S028/CYD M31 I/O tester is working and ready for field testing.

Hardware is the **dual-USB CYD v3 ST7789 variant**, not the older ILI9341 variant.

Known-good display configuration:

- ST7789
- rotation 3
- 320x240 landscape
- direct rendering to TFT
- no sprite/framebuffer
- `TFT_RGB_ORDER=TFT_BGR`
- `TFT_INVERSION_OFF`
- current colors/layout accepted
- touch alignment accepted
- change-driven display refresh added
- there is still a small occasional quick LCD shimmer; it is acceptable for now and should only be tuned later

Expected serial banner:

```text
LCD ST7789 direct r3 change-driven 320x240
```

## Touch

Touch uses HSPI and must remain separate from TFT SPI.

Pins:

```text
IRQ  36
MISO 39
MOSI 32
CLK  25
CS   33
```

Measured working mapping:

```cpp
x=map(constrain(p.x,250,3425),3425,250,0,319);
y=map(constrain(p.y,490,3540),3540,490,0,239);
```

Do not change touch mapping unless bench evidence shows an actual alignment problem.

## RS485

```text
TX GPIO22
RX GPIO35
GND common
9600 8N1
```

GPIO21 is LCD backlight and must not be used.

HW-0519 auto-direction RS485 module is the usual bench interface.

## UI

Touch tabs:

```text
AI / DI / DO / AO / MB
```

Browser dashboard is working and includes a runtime Modbus slave-address selector.

The selector:

- accepts 1–247
- immediately redirects all Modbus reads/writes
- resets diagnostics when changed
- does not reprogram the M31 address
- is runtime-only and returns to address 1 after reboot

Changing the dashboard address to 2 was bench-tested successfully.

## Current useful features

- M31 ONLINE/OFFLINE
- AI1–AI8 live mA
- DI1–DI4
- DO1–DO4 controls
- AO1–AO4 readback/manual force
- Modbus good/bad/timeout/CRC diagnostics
- raw last TX/RX data in browser API/dashboard
- WiFiManager captive portal
- browser dashboard/API
- runtime slave selection

## Current caveats

1. Small occasional LCD shimmer remains; leave it alone until after field testing unless it becomes annoying enough to matter.
2. M31 register addresses/scaling in this portable tester are still preliminary and should be compared with the proven main MultiFlow M31 implementation before trusting all outputs.
3. Test AO with a meter before process use.
4. Modbus ONLINE logic is still simple cumulative good/bad logic and can be improved later with consecutive failure tracking.

## Important commits

```text
755d64f  Switch CYD v3 display driver to ST7789
c807360  Native ST7789 landscape rendering
52f2cac  Flip ST7789 landscape orientation to rotation 3
009bbbd  Set ST7789 inversion mode
6176ab9  Render directly to ST7789, remove sprite artifacts
932435f  Partial live-value refresh
48a9399  Runtime Modbus address selector on web dashboard
fa1d69e  Change-driven display refresh
```

## Build / upload

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

Portable board has commonly been COM7.

## Suggested next session

Start by reviewing field-test observations. If the unit works properly in the field, avoid changing the display unless the remaining shimmer is worth addressing. Then compare the portable tester M31 register map/scaling with the main MultiFlow implementation and validate DI/AI/DO/AO systematically.

## Paste-this continuation prompt

```text
Continue development of the portable Riot I/O Tester in homershaw/MultiFlow on branch feature/multiflow-common-core, path tools/riot_io_tester_2432s028.

The working hardware is the dual-USB ESP32-2432S028/CYD v3 with an ST7789 display, not ILI9341. The known-good LCD setup is ST7789, rotation 3, 320x240 landscape, direct rendering, BGR order, inversion off, and the current touch calibration must be preserved. Touch is on HSPI with IRQ36/MISO39/MOSI32/CLK25/CS33. RS485 is TX22/RX35 at 9600 8N1.

The UI has AI/DI/DO/AO/MB tabs, WiFiManager, browser dashboard, Modbus diagnostics, and a runtime dashboard Modbus address selector 1–247. Changing the address to 2 was bench-tested successfully. Current display refresh is change-driven and mostly clean, with a small occasional quick shimmer that is acceptable for now.

Please preserve the current colors, orientation, touch mapping, dashboard behavior, and working Modbus address selector. First review field-test observations before making changes. The next technical priority after field testing is validating the portable tester's M31 register map/scaling against the proven main MultiFlow implementation, then bench-validating DI/AI/DO/AO. Important commits include 755d64f, c807360, 52f2cac, 009bbbd, 6176ab9, 932435f, 48a9399, and fa1d69e.
```
