#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include "../profiles/unit_config.h"

// Station connection, setup AP and mDNS (PROMPT.md section 4).
namespace wifimgr {
// Searching: scanning to pick the saved network (if in range) or a RIOT hotspot to pair with.
enum class State : uint8_t { NoCredentials, Searching, Connecting, Connected, Retrying };
enum class Trial : uint8_t { Idle, Running, Succeeded, Failed };

void begin();
void service();

// BOOT 3 s or touch: start the setup AP while keeping the station side running.
void requestPortal();
// BOOT 10 s: restore the default Wi-Fi settings (profiles and the rest of unit.toml are kept).
void resetNetwork();

// Portal "Save & connect": try the candidate and save it to unit.toml only if it connects.
void startTrial(const unitcfg::Wifi& candidate);
Trial trial();
String trialMessage();

State state();
const char* stateText();
bool apActive();
uint8_t apClients();
uint32_t apOffInS();  // 0 = no scheduled shutdown
const String& unitName();  // riot-XXXX
IPAddress stationIp();
IPAddress apIp();             // setup AP address (192.168.5.1)
String network();             // SSID currently joined, "" if not connected
String pairedRiot();          // RIOT hotspot SSID when paired automatically, else ""
bool searching();             // a Wi-Fi scan is in progress (the portal must not start or delete one)
int rssi();
String lastFailure();
// Copy of the saved station settings, safe to call from AsyncTCP's task.
unitcfg::Wifi savedWifi();
}  // namespace wifimgr
