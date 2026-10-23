#pragma once
// Short critical sections around the value store. Never allocate or block inside.
#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
namespace mb {
class Lock {
 public:
  void lock() { portENTER_CRITICAL(&mux_); }
  void unlock() { portEXIT_CRITICAL(&mux_); }
 private:
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
}  // namespace mb
#else
#include <mutex>
namespace mb {
class Lock {
 public:
  void lock() { m_.lock(); }
  void unlock() { m_.unlock(); }
 private:
  std::mutex m_;
};
}  // namespace mb
#endif

namespace mb {
struct Guard {
  explicit Guard(Lock& l) : l_(l) { l_.lock(); }
  ~Guard() { l_.unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
 private:
  Lock& l_;
};
}  // namespace mb
