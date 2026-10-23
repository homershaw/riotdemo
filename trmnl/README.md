# Riot-Flow TRMNL demo controller

Riot-Flow firmware for the Seeed XIAO ESP32-S3 with the 7.5" TRMNL ePaper display. It was the first Riot-Flow controller; production RIOT controllers are now ESP32-P4 based and developed separately. This copy is kept as a working demo.

- Source: snapshot of the TRMNL firmware as of 2026-09-17 (A10a: virgin SPIFFS provisioning). The public OTA "stable" release is `2.0.0-a7b2` (2026-09-14) in [MutliFlowOTA](https://github.com/homershaw/MutliFlowOTA) under `P4-TRMNL/`.
- Main source: `src/main.cpp`. Shared modules: the AGA engine (`aga_engine.*`), Detail calculations, history stores and the time service.
- Features: M31 I/O over Modbus, flow runs with AGA calculation and totals, rules and alarms, history and audit log, MQTT, browser OTA, technician access, and a KEY1 demo QR screen.

## Build and flash

PlatformIO, env `trmnl_controller` (espressif32 6.5.0):

```
pio run -e trmnl_controller
pio run -e trmnl_controller -t upload
```

`usb_reset_upload.py` resets the XIAO into the bootloader before upload.

Building this env installs esptool 4.5.1 into PlatformIO's shared Python, which cannot flash ESP32-P4 boards. On a PC that also flashes P4 controllers, reinstall esptool 5.x afterwards.
