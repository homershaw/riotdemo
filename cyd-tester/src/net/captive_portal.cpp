#include "captive_portal.h"
#include <DNSServer.h>
#include <WiFi.h>
#include "../web/web_server.h"
#include "../web/www_assets.h"
#include "wifi_manager.h"

namespace portal {
namespace {
DNSServer dns;
bool dnsUp = false;
String portalUrl() { return "http://" + WiFi.softAPIP().toString() + "/"; }

const char PortalHtml[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>RIOT test unit setup</title>
<style>body{font:16px system-ui,sans-serif;margin:0;background:#f3f5f7;color:#18202a}
main{max-width:460px;margin:auto;padding:16px}h1{font-size:20px;margin:4px 0 2px}.muted{color:#5d6974;font-size:13px}
.card{background:#fff;border:1px solid #cbd3da;border-radius:8px;padding:12px;margin:12px 0}
label{display:block;font-size:13px;color:#5d6974;margin:10px 0 4px}input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #cbd3da;border-radius:6px}
button{width:100%;padding:12px;font-size:16px;border:0;border-radius:6px;background:#24394b;color:#fff;margin-top:14px}
button.link{background:none;color:#24394b;padding:6px;margin:0;width:auto}
#nets div{display:flex;justify-content:space-between;padding:9px 4px;border-bottom:1px solid #eef2f5;cursor:pointer}
#result{font-weight:600;margin-top:12px}.good{color:#16753a}.bad{color:#b52424}</style></head><body><main>
<h1 id="title">RIOT test unit</h1><div class="muted" id="state">Loading...</div>
<div class="card"><div style="display:flex;justify-content:space-between;align-items:center"><b>Networks</b>
<button class="link" onclick="scan()">Rescan</button></div><div id="nets" class="muted">Scanning...</div></div>
<form class="card" id="f"><label>Network name (SSID)</label><input name="ssid" id="ssid" maxlength="32" required>
<label>Password</label><input name="password" id="pw" type="password" maxlength="63" placeholder="Leave empty for an open network">
<label>IP address</label><select name="ip_mode" id="mode"><option value="dhcp">Automatic (DHCP)</option><option value="static">Static</option></select>
<div id="st" hidden><label>Static IP</label><input name="static_ip" id="ip"><label>Netmask</label><input name="netmask" id="nm">
<label>Gateway</label><input name="gateway" id="gw"><label>DNS</label><input name="dns" id="dn"></div>
<button>Save &amp; connect</button><div id="result"></div></form>
<div class="muted">The RIOT polls this unit at a fixed address, so a static IP is usually easiest.</div></main>
<script>const $=i=>document.getElementById(i);let poll;
function bars(r){return r>-55?'▂▄▆█':r>-67?'▂▄▆':r>-78?'▂▄':'▂'}
async function scan(){$('nets').textContent='Scanning...';for(let i=0;i<20;i++){const r=await(await fetch('/portal/scan')).json();
if(!r.scanning){$('nets').innerHTML='';if(!r.networks.length)$('nets').textContent='No networks found';
r.networks.forEach(n=>{const d=document.createElement('div');d.innerHTML='<span></span><span></span>';
d.children[0].textContent=n.ssid;d.children[1].textContent=(n.secure?'🔒 ':'')+bars(n.rssi);
d.onclick=()=>{$('ssid').value=n.ssid;$('pw').focus()};$('nets').appendChild(d)});return}
await new Promise(s=>setTimeout(s,700))}$('nets').textContent='Scan timed out'}
async function status(){const s=await(await fetch('/portal/status')).json();$('title').textContent=s.name+' setup';
$('state').textContent='Station: '+s.state+(s.ip?' ('+s.ip+')':'')+(s.failure?' - last error: '+s.failure:'');return s}
$('mode').onchange=()=>$('st').hidden=$('mode').value!='static';
$('f').onsubmit=async e=>{e.preventDefault();$('result').className='';$('result').textContent='Connecting...';
const r=await fetch('/portal/save',{method:'POST',body:new URLSearchParams(new FormData($('f')))});
if(!r.ok){$('result').className='bad';$('result').textContent=await r.text();return}
clearInterval(poll);poll=setInterval(async()=>{const s=await status();if(s.trial=='running')return;clearInterval(poll);
$('result').className=s.trial=='succeeded'?'good':'bad';$('result').textContent=s.message+(s.trial=='succeeded'?' This setup network closes in 60 s.':'')},1000)};
status().then(s=>{$('ssid').value=s.ssid;$('mode').value=s.static?'static':'dhcp';$('st').hidden=!s.static;
$('ip').value=s.static_ip;$('nm').value=s.netmask;$('gw').value=s.gateway;$('dn').value=s.dns});scan();</script></body></html>)HTML";

bool viaAp(AsyncWebServerRequest* r) {
  return wifimgr::apActive() && r->client() && r->client()->localIP() == WiFi.softAPIP();
}

void sendPortal(AsyncWebServerRequest* r) {
  AsyncWebServerResponse* res = r->beginResponse(200, "text/html", reinterpret_cast<const uint8_t*>(PortalHtml), strlen_P(PortalHtml));
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

String arg(AsyncWebServerRequest* r, const char* name) {
  const AsyncWebParameter* p = r->getParam(name, true);
  String v = p ? p->value() : String();
  v.trim();
  return v;
}

void handleSave(AsyncWebServerRequest* r) {
  unitcfg::Wifi w = wifimgr::savedWifi();
  w.ssid = arg(r, "ssid");
  w.password = r->getParam("password", true) ? r->getParam("password", true)->value() : String();
  w.staticIp = arg(r, "ip_mode") == "static";
  if (w.staticIp) {
    w.ip = arg(r, "static_ip"); w.netmask = arg(r, "netmask"); w.gateway = arg(r, "gateway"); w.dns = arg(r, "dns");
  }
  IPAddress check;
  if (!w.ssid.length() || w.ssid.length() > 32) { r->send(400, "text/plain", "Enter a network name (1-32 characters)."); return; }
  if (w.password.length() && (w.password.length() < 8 || w.password.length() > 63)) {
    r->send(400, "text/plain", "Wi-Fi passwords are 8-63 characters, or empty for an open network."); return;
  }
  if (w.staticIp && (!check.fromString(w.ip) || !check.fromString(w.netmask) || !check.fromString(w.gateway) ||
                     (w.dns.length() && !check.fromString(w.dns)))) {
    r->send(400, "text/plain", "Check the static IP, netmask, gateway and DNS addresses."); return;
  }
  r->send(202, "text/plain", "Trying the new settings");
  wifimgr::startTrial(w);
}

void handleStatus(AsyncWebServerRequest* r) {
  const auto w = wifimgr::savedWifi();
  static const char* trials[] = {"idle", "running", "succeeded", "failed"};
  const bool connected = wifimgr::state() == wifimgr::State::Connected;
  String j = "{\"name\":\"" + web::jsonEscape(wifimgr::unitName()) + "\",\"state\":\"" + wifimgr::stateText() + "\"";
  j += ",\"ip\":\"" + (connected ? wifimgr::stationIp().toString() : String()) + "\"";
  j += ",\"failure\":\"" + web::jsonEscape(wifimgr::lastFailure()) + "\"";
  j += ",\"trial\":\"" + String(trials[int(wifimgr::trial())]) + "\",\"message\":\"" + web::jsonEscape(wifimgr::trialMessage()) + "\"";
  j += ",\"ssid\":\"" + web::jsonEscape(w.ssid) + "\",\"static\":" + (w.staticIp ? "true" : "false");
  j += ",\"static_ip\":\"" + web::jsonEscape(w.ip) + "\",\"netmask\":\"" + web::jsonEscape(w.netmask) + "\"";
  j += ",\"gateway\":\"" + web::jsonEscape(w.gateway) + "\",\"dns\":\"" + web::jsonEscape(w.dns) + "\"}";
  r->send(200, "application/json", j);
}

void handleScan(AsyncWebServerRequest* r) {
  // The Wi-Fi manager owns the scan while it is searching for a network or a RIOT to pair with.
  if (wifimgr::searching()) { r->send(200, "application/json", "{\"scanning\":true}"); return; }
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { r->send(200, "application/json", "{\"scanning\":true}"); return; }
  if (n < 0) {  // none yet, or the last one failed: start a new one
    WiFi.scanNetworks(true, false);
    r->send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  String j = "{\"scanning\":false,\"networks\":[";
  int listed = 0;
  for (int i = 0; i < n && listed < 20; ++i) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;  // one row per SSID, strongest first (results are sorted by RSSI)
    for (int k = 0; k < i; ++k) dup |= WiFi.SSID(k) == ssid;
    if (dup) continue;
    if (listed++) j += ',';
    j += "{\"ssid\":\"" + web::jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  j += "]}";
  WiFi.scanDelete();  // the next request starts a fresh scan
  r->send(200, "application/json", j);
}
}  // namespace

void registerRoutes(AsyncWebServer& server) {
  // Handlers match by prefix ("/portal" also matches "/portal/..."), so register the specific ones first.
  server.on("/portal/status", HTTP_GET, handleStatus);
  server.on("/portal/scan", HTTP_GET, handleScan);
  server.on("/portal/save", HTTP_POST, handleSave);
  server.on("/portal", HTTP_GET, sendPortal);
  // OS connectivity probes: answer with a redirect so the portal opens automatically.
  for (const char* probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                            "/ncsi.txt", "/connecttest.txt", "/redirect", "/fwlink"}) {
    server.on(probe, HTTP_ANY, [](AsyncWebServerRequest* r) { r->redirect(portalUrl()); });
  }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (viaAp(r)) { sendPortal(r); return; }
    www::sendIndex(r);  // station side: the web UI
  });
  server.onNotFound([](AsyncWebServerRequest* r) {
    if (viaAp(r)) r->redirect(portalUrl());  // any unknown host or path on the AP lands on the portal
    else r->send(404, "text/plain", "Not found");
  });
}

void start() {
  if (dnsUp) return;
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dnsUp = dns.start(53, "*", WiFi.softAPIP());
}

void stop() {
  if (!dnsUp) return;
  dns.stop();
  dnsUp = false;
}

void service() {
  if (dnsUp) dns.processNextRequest();
}
}  // namespace portal
