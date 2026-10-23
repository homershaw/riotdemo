#include "server.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>

namespace mbserver {
namespace {
AsyncServer* tcp = nullptr;
store::Store* values = nullptr;
mbproc::Settings settings;
uint8_t clientLimit = 4;
uint16_t idleS = 60;
Stats counters;
mb::Lock statsLock;

struct Connection {
  AsyncClient* client;
  std::vector<uint8_t> rx;
  uint32_t ip;
};

uint8_t fcIndex(uint8_t fc) { return fc <= 16 ? fc : fc == 0x2B ? 17 : 0; }

void count(uint32_t ip, uint8_t unit, const uint8_t* pdu, size_t pduLen, const mbproc::Result& r, bool sent) {
  mb::Guard g(statsLock);
  ++counters.requests;
  ++counters.byFc[fcIndex(pdu[0])];
  counters.lastRequestMs = millis();
  counters.lastClientIp = ip;
  counters.lastUnit = unit;
  counters.lastFc = pdu[0];
  counters.lastAddress = pduLen >= 3 ? uint16_t(pdu[1] << 8 | pdu[2]) : 0;
  counters.lastException = r.exception;
  if (!r.reply) ++counters.silent;
  else if (!sent) ++counters.dropped;
  else ++counters.replies;
  if (r.exception) {
    ++counters.exceptions;
    if (r.exception < 5) ++counters.exceptionsByCode[r.exception];
  }
}

// Handles one complete MBAP frame (header + PDU).
void handleFrame(Connection& conn, const uint8_t* frame, size_t len) {
  const uint8_t unit = frame[6];
  const uint8_t* pdu = frame + 7;
  const size_t pduLen = len - 7;
  uint8_t out[7 + mbproc::MaxPdu];
  const mbproc::Result r = mbproc::handle(*values, settings, unit, pdu, pduLen, out + 7);
  bool sent = false;
  if (r.reply) {
    out[0] = frame[0];  // transaction ID
    out[1] = frame[1];
    out[2] = 0;  // protocol ID
    out[3] = 0;
    const uint16_t length = uint16_t(r.length + 1);
    out[4] = uint8_t(length >> 8);
    out[5] = uint8_t(length);
    out[6] = unit;
    const size_t total = 7 + r.length;
    if (conn.client->space() >= total) {
      conn.client->add(reinterpret_cast<const char*>(out), total, ASYNC_WRITE_FLAG_COPY);
      sent = conn.client->send();
    }
  }
  count(conn.ip, unit, pdu, pduLen, r, sent);
}

void onData(Connection& conn, const uint8_t* data, size_t len) {
  conn.rx.insert(conn.rx.end(), data, data + len);
  // TCP may split or merge requests: consume every complete frame in the buffer.
  while (conn.rx.size() >= 8) {
    const uint16_t protocol = uint16_t(conn.rx[2] << 8 | conn.rx[3]);
    const uint16_t length = uint16_t(conn.rx[4] << 8 | conn.rx[5]);  // unit ID + PDU
    if (protocol != 0 || length < 2 || length > 1 + mbproc::MaxPdu) {
      conn.rx.clear();  // not Modbus TCP: drop the connection like a real gateway would
      conn.client->close();
      return;
    }
    const size_t frameLen = 6u + length;
    if (conn.rx.size() < frameLen) return;  // wait for the rest
    handleFrame(conn, conn.rx.data(), frameLen);
    conn.rx.erase(conn.rx.begin(), conn.rx.begin() + frameLen);
  }
}
}  // namespace

bool begin(store::Store& s, const mbproc::Settings& cfg, uint16_t port, uint8_t maxClients, uint16_t idleTimeoutS) {
  values = &s;
  settings = cfg;
  clientLimit = maxClients;
  idleS = idleTimeoutS;
  tcp = new AsyncServer(port);
  tcp->onClient(
      [](void*, AsyncClient* c) {
        {
          mb::Guard g(statsLock);
          if (counters.activeClients >= clientLimit) {
            ++counters.rejectedClients;
            c->onDisconnect([](void*, AsyncClient* cl) { delete cl; });
            c->close();
            return;
          }
          ++counters.activeClients;
          ++counters.totalClients;
        }
        auto* conn = new Connection{c, {}, uint32_t(c->remoteIP())};
        conn->rx.reserve(64);
        c->setNoDelay(true);
        if (idleS) c->setRxTimeout(idleS);
        c->onData([](void* arg, AsyncClient*, void* data, size_t len) {
          onData(*static_cast<Connection*>(arg), static_cast<const uint8_t*>(data), len);
        }, conn);
        c->onTimeout([](void*, AsyncClient* cl, uint32_t) { cl->close(); }, nullptr);
        c->onDisconnect([](void* arg, AsyncClient* cl) {
          {
            mb::Guard g(statsLock);
            if (counters.activeClients) --counters.activeClients;
          }
          delete static_cast<Connection*>(arg);
          delete cl;
        }, conn);
      },
      nullptr);
  tcp->begin();
  return true;
}

Stats stats() {
  mb::Guard g(statsLock);
  return counters;
}
}  // namespace mbserver
