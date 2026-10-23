# CYD-TESTER

Modbus TCP test unit for the RIOT controller, running on an ESP32-2432S028R "Cheap Yellow Display". It emulates field devices from RIOT Device Library TOML profiles. `PROMPT.md` is the full specification: follow its phases, and record any assumptions in README.md.

## Build and flash (Windows)

- Run `pio` from PowerShell, not Git Bash.
- Env `cyd`: espressif32 6.5.0 (Arduino core 2.0.14; TFT_eSPI is reliable on core 2.x). The board is on COM14 (CH340). `pio run -e cyd -t upload` works here.
- All pins are in `include/board_pins.h`, which `build_flags` force-includes so TFT_eSPI is configured without editing the library.
- toml++ v3.4.0 is vendored in `lib/tomlplusplus` because it isn't in the PlatformIO registry. XPT2046_Touchscreen is pinned to the GitHub tag v1.4.
- 4 MB flash, no PSRAM. App slots are 1600 KB each (`partitions.csv`), and phase 6 uses about 85% of one (no SD logging, OTA or HTTPS by request), so report flash use after each phase. The Modbus server is our own AsyncTCP code (see README Assumptions), not eModbus.

## Shared PlatformIO install

This PC also builds ESP32-P4 projects that need esptool 5.4.0. The core-2.x platform installed esptool 4.5.1 into `~/.platformio/packages/tool-esptoolpy`, which made the shared penv `python -m esptool` load 4.5.1. The fix is to repoint it with `pip install --no-deps -e` to the 5.4.0 copy in `tool-esptoolpy@src-...`. CYD uploads are unaffected, because the platform runs its own `esptool.py` script.

## Pairing design (decided with the user, 2026-10-21)

- One tester, any RIOT. Every tester pairs at the same fixed address, 192.168.4.200, and every demo RIOT polls that address, so testers and RIOTs are interchangeable.
- With several RIOTs in range the tester pairs with the nearest one (strongest signal) and stays with it while the link holds. Do not add sticky or per-kit pairing; it was considered and rejected.
- A saved (manually chosen) network always wins over pairing. The setup AP is on 192.168.5.1 so it never overlaps a RIOT's 192.168.4.x hotspot.
