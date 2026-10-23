#include "wifi_manager.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include "captive_portal.h"

namespace wifimgr {
namespace {
constexpr uint32_t ApLingerAfterConnectMs = 60000;   // spec 4.2: keep AP 60 s so the phone sees the new IP
constexpr uint32_t ManualApIdleMs = 10 * 60000;      // AP opened on request while connected
constexpr uint32_t BackoffMinMs = 5000, BackoffMaxMs = 60000;
constexpr uint32_t ScanTimeoutMs = 8000;
// Setup AP subnet. Not 192.168.4.x: RIOT Walkup hotspots use 192.168.4.1, and pairing
// joins that network, so the CYD's own AP must never overlap it.
const IPAddress ApIp(192, 168, 5, 1), ApMask(255, 255, 255, 0);

State st = State::NoCredentials;
unitcfg::Wifi active;      // credentials currently in use (saved network or pairing target)
unitcfg::Wifi candidate;   // portal trial being tested
String name, pairedWith;   // pairedWith: RIOT hotspot SSID when the current link is an automatic pairing
bool ap = false, manualAp = false, everConnected = false, mdnsUp = false;
uint32_t connectStarted = 0, nextRetry = 0, backoff = BackoffMinMs, droppedAt = 0, apOffAt = 0, scanStarted = 0;
volatile uint8_t disconnectReason = 0;

// Shared with AsyncTCP's task (portal handlers): guarded by `lock`.
SemaphoreHandle_t lock = nullptr;
struct Guard {
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
};
Trial trialState = Trial::Idle;
String failure, trialMsg;
bool trialPending = false;
unitcfg::Wifi pendingTrial;

void setFailure(const char* why) { Guard g; failure = why; }
void setTrial(Trial t, const String& msg) { Guard g; trialState = t; trialMsg = msg; }

const char* reasonText(uint8_t r) {
  switch (r) {
    case 0: return "timed out";
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wrong password or security mismatch";
    case WIFI_REASON_ASSOC_FAIL: return "association refused";
    case WIFI_REASON_AUTH_EXPIRE: return "authentication timed out";
    case WIFI_REASON_BEACON_TIMEOUT: return "signal lost";
    default: return "disconnected";
  }
}

void connect(const unitcfg::Wifi& w, const String& pairing = String()) {
  WiFi.disconnect(false, false);
  IPAddress ip, mask, gw, dns;
  if (w.staticIp && ip.fromString(w.ip) && mask.fromString(w.netmask) && gw.fromString(w.gateway)) {
    if (!dns.fromString(w.dns)) dns = gw;
    WiFi.config(ip, gw, mask, dns);
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
  disconnectReason = 0;
  pairedWith = pairing;
  WiFi.begin(w.ssid.c_str(), w.password.length() ? w.password.c_str() : nullptr);
  connectStarted = millis();
  st = State::Connecting;
  Serial.printf("Wi-Fi: connecting to '%s' (%s%s)\n", w.ssid.c_str(), w.staticIp ? "static " : "DHCP",
                w.staticIp ? w.ip.c_str() : "");
}

// The saved network wins when it is in range; otherwise pair with the strongest RIOT hotspot.
void startSearch() {
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) WiFi.scanNetworks(true, false);  // else reuse the portal's scan
  scanStarted = millis();
  st = State::Searching;
}

unitcfg::Wifi pairingTarget(const String& ssid) {
  const auto& p = unitcfg::current().pairing;
  unitcfg::Wifi w = unitcfg::current().wifi;
  w.ssid = ssid;
  w.password = p.password;
  w.staticIp = true;
  w.ip = p.ip;
  w.netmask = p.netmask;
  w.gateway = p.gateway;
  w.dns = p.gateway;
  return w;
}

void startAp(uint32_t offAfterMs) {
  if (!ap) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(ApIp, ApIp, ApMask);
    const String& pw = unitcfg::current().apPassword;
    WiFi.softAP(name.c_str(), pw.length() ? pw.c_str() : nullptr);
    portal::start();
    ap = true;
    Serial.printf("Setup AP '%s' started at %s\n", name.c_str(), WiFi.softAPIP().toString().c_str());
  }
  apOffAt = offAfterMs ? millis() + offAfterMs : 0;
}

void stopAp() {
  if (!ap) return;
  portal::stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
  manualAp = false;
  apOffAt = 0;
  Serial.println("Setup AP stopped");
}

void scheduleRetry() {
  nextRetry = millis() + backoff;
  backoff = min(backoff * 2, BackoffMaxMs);
  st = State::Retrying;
}

// Scan finished (or timed out): choose what to join.
void finishSearch(int16_t found) {
  const auto saved = unitcfg::current().wifi;
  const auto& pairing = unitcfg::current().pairing;
  bool savedVisible = false;
  String riot;
  int32_t riotRssi = -1000;
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    if (saved.ssid.length() && ssid == saved.ssid) savedVisible = true;
    if (pairing.enabled && ssid.startsWith(pairing.ssidPrefix) && WiFi.RSSI(i) > riotRssi) {
      riot = ssid;
      riotRssi = WiFi.RSSI(i);
    }
  }
  if (found >= 0) WiFi.scanDelete();
  Serial.printf("Wi-Fi: scan found %d network(s); saved '%s' %s; RIOT hotspot %s\n", int(found), saved.ssid.c_str(),
                savedVisible ? "in range" : "not seen", riot.length() ? riot.c_str() : "none");
  if (savedVisible) { active = saved; connect(active); return; }
  if (riot.length()) { active = pairingTarget(riot); connect(active, riot); return; }
  if (saved.ssid.length()) { active = saved; connect(active); return; }  // may be a hidden network
  setFailure("no network or RIOT hotspot in range");
  startAp(0);
  scheduleRetry();
}

void onConnected() {
  st = State::Connected;
  everConnected = true;
  droppedAt = 0;
  backoff = BackoffMinMs;
  setFailure("");
  if (mdnsUp) MDNS.end();
  mdnsUp = MDNS.begin(name.c_str());
  if (mdnsUp) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("modbus", "tcp", unitcfg::current().modbus.port);
  }
  Serial.printf("Wi-Fi: connected to '%s'%s, IP %s, RSSI %d, mDNS %s.local %s\n", WiFi.SSID().c_str(),
                pairedWith.length() ? " (RIOT pairing)" : "", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                name.c_str(), mdnsUp ? "up" : "FAILED");
  if (trial() == Trial::Running) {
    { Guard g; unitcfg::current().wifi = candidate; }
    const bool saved = unitcfg::save();
    setTrial(Trial::Succeeded, "Connected to " + candidate.ssid + " as " + WiFi.localIP().toString() + "." +
                                   (saved ? "" : " WARNING: the settings could not be saved."));
  }
  if (ap && !manualAp) apOffAt = millis() + ApLingerAfterConnectMs;
}

void takePendingTrial() {
  {
    Guard g;
    if (!trialPending) return;
    trialPending = false;
    candidate = pendingTrial;
  }
  manualAp = false;  // after a successful trial the AP lingers 60 s, then closes
  active = candidate;
  everConnected = false;
  droppedAt = 0;
  connect(active);
}
}  // namespace

void begin() {
  lock = xSemaphoreCreateMutex();
  uint8_t mac[6];
  WiFi.mode(WIFI_STA);
  WiFi.macAddress(mac);
  char buf[12];
  snprintf(buf, sizeof buf, "riot-%02X%02X", mac[4], mac[5]);
  name = buf;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);  // reconnects are ours, with backoff
  // No modem-sleep: as a server we must answer within the poller's timeout. With power
  // saving on, connection replies can take 100-300 ms, and the RIOT gives up after 200 ms.
  WiFi.setSleep(false);
  WiFi.setHostname(name.c_str());
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) { disconnectReason = info.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  active = unitcfg::current().wifi;
  if (!active.ssid.length() && !unitcfg::current().pairing.enabled) {
    st = State::NoCredentials;
    startAp(0);
    return;
  }
  startSearch();
}

void service() {
  const uint32_t now = millis();
  const bool linked = WiFi.status() == WL_CONNECTED;
  if (ap) portal::service();
  takePendingTrial();

  switch (st) {
    case State::NoCredentials:
      break;
    case State::Searching: {
      const int16_t n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING && now - scanStarted < ScanTimeoutMs) break;
      finishSearch(n >= 0 ? n : 0);
      break;
    }
    case State::Connecting:
      if (linked) { onConnected(); break; }
      if (now - connectStarted >= uint32_t(active.connectTimeoutS) * 1000) {
        const char* why = reasonText(disconnectReason);
        setFailure(why);
        Serial.printf("Wi-Fi: connect failed: %s\n", why);
        if (trial() == Trial::Running) {
          setTrial(Trial::Failed, "Could not connect to " + candidate.ssid + ": " + why + ". Previous settings kept.");
          backoff = BackoffMinMs;
        }
        // Spec 4.2: a failed station connection starts the AP; keep retrying in the background.
        if (droppedAt == 0) startAp(0);
        scheduleRetry();
      }
      break;
    case State::Connected:
      if (!linked) {
        const char* why = reasonText(disconnectReason);
        setFailure(why);
        Serial.printf("Wi-Fi: connection lost: %s\n", why);
        droppedAt = now;
        backoff = BackoffMinMs;
        scheduleRetry();
      }
      break;
    case State::Retrying:
      if (linked) { onConnected(); break; }
      // Spec 4.1: after a drop, add the setup AP once portal_after_s passes without reconnecting.
      if (droppedAt && !ap && now - droppedAt >= uint32_t(unitcfg::current().wifi.portalAfterS) * 1000) startAp(0);
      // Scans and station retries hop channels and drop phones off the AP, so hold them while someone is in the portal.
      if (ap && WiFi.softAPgetStationNum() > 0) break;
      if (int32_t(now - nextRetry) >= 0) startSearch();  // the saved network or a RIOT may have appeared
      break;
  }

  if (ap && apOffAt && st == State::Connected) {
    // A manually opened AP stays up while someone is using it; the post-setup linger always ends.
    if (manualAp && WiFi.softAPgetStationNum() > 0) apOffAt = now + ManualApIdleMs;
    if (int32_t(now - apOffAt) >= 0) stopAp();
  }
}

void requestPortal() {
  manualAp = st == State::Connected;
  startAp(manualAp ? ManualApIdleMs : 0);
}

void resetNetwork() {
  auto& c = unitcfg::current();
  {
    Guard g;
    c.wifi = unitcfg::Wifi{};
    c.pairing = unitcfg::Pairing{};
    c.apPassword = "";
  }
  const bool saved = unitcfg::save();
  Serial.printf("Network settings reset to defaults (%s)\n", saved ? "saved" : "SAVE FAILED");
  setTrial(Trial::Idle, "");
  everConnected = false;
  droppedAt = 0;
  backoff = BackoffMinMs;
  startSearch();
}

void startTrial(const unitcfg::Wifi& w) {
  // Called from a portal handler on AsyncTCP's task: service() picks it up on the loop task.
  Guard g;
  pendingTrial = w;
  trialPending = true;
  trialState = Trial::Running;
  trialMsg = "Connecting to " + w.ssid + "...";
}

Trial trial() { Guard g; return trialState; }
String trialMessage() { Guard g; return trialMsg; }
State state() { return st; }
const char* stateText() {
  switch (st) {
    case State::NoCredentials: return "no network set";
    case State::Searching: return "searching";
    case State::Connecting: return "connecting";
    case State::Connected: return pairedWith.length() ? "paired" : "connected";
    case State::Retrying: return "retrying";
  }
  return "?";
}
bool searching() { return st == State::Searching; }
bool apActive() { return ap; }
uint8_t apClients() { return ap ? WiFi.softAPgetStationNum() : 0; }
uint32_t apOffInS() {
  if (!ap || !apOffAt || st != State::Connected) return 0;
  const int32_t left = int32_t(apOffAt - millis());
  return left > 0 ? (left + 999) / 1000 : 0;
}
const String& unitName() { return name; }
IPAddress stationIp() { return WiFi.localIP(); }
IPAddress apIp() { return WiFi.softAPIP(); }
String network() { return st == State::Connected ? WiFi.SSID() : String(); }
String pairedRiot() { return st == State::Connected ? pairedWith : String(); }
int rssi() { return st == State::Connected ? WiFi.RSSI() : 0; }
String lastFailure() { Guard g; return failure; }
unitcfg::Wifi savedWifi() { Guard g; return unitcfg::current().wifi; }
}  // namespace wifimgr
