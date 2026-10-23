#pragma once
#include <cstdint>
#include "request_handler.h"
#include "value_store.h"

// Modbus TCP server on AsyncTCP: MBAP framing, client limits, idle timeout and
// counters. Request handling itself is in request_handler (transport-free).
namespace mbserver {
struct Stats {
  uint32_t requests = 0, replies = 0, exceptions = 0, silent = 0, dropped = 0;
  uint32_t byFc[256 / 8] = {};  // counts for FC 1-16, 43 (index = FC, others pooled at 0)
  uint32_t exceptionsByCode[5] = {};
  uint8_t activeClients = 0;
  uint32_t totalClients = 0, rejectedClients = 0;
  uint32_t lastRequestMs = 0;
  uint32_t lastClientIp = 0;
  uint8_t lastUnit = 0, lastFc = 0, lastException = 0;
  uint16_t lastAddress = 0;
};

bool begin(store::Store& s, const mbproc::Settings& cfg, uint16_t port, uint8_t maxClients, uint16_t idleTimeoutS);
Stats stats();
}  // namespace mbserver
