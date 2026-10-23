#pragma once

#include <Arduino.h>
#include <time.h>

namespace RiotTimeService {

enum Source : uint8_t {
  SOURCE_NONE = 0,
  SOURCE_RTC = 1,
  SOURCE_NTP = 2
};

enum EventType : uint8_t {
  EVENT_RTC_INITIALIZED = 0,
  EVENT_TIME_SYNC_NTP = 1,
  EVENT_TIME_STEP_LARGE = 2,
  EVENT_RTC_LOST_POWER = 3,
  EVENT_TIME_SOURCE_FALLBACK = 4
};

enum BoundaryRelation : int8_t {
  BOUNDARY_INVALID = -2,
  BOUNDARY_BACKWARD = -1,
  BOUNDARY_SAME = 0,
  BOUNDARY_FORWARD = 1
};

struct Event {
  EventType type;
  Source source;
  time_t oldEpoch;
  time_t newEpoch;
  int32_t correctionSeconds;
  bool rtcOsf;
};

struct CalendarKeys {
  uint32_t dateKey; // YYYYMMDD
  uint32_t hourKey; // YYYYMMDDHH
  bool valid;
};

using EventCallback = void (*)(const Event& event);

void setEventCallback(EventCallback cb);
void setLargeStepThresholdSeconds(uint32_t seconds);
uint32_t largeStepThresholdSeconds();

void noteRtcLostPower(time_t rtcEpoch = 0);
void noteRtcInitialized(time_t oldEpoch, time_t newEpoch);
void noteNtpCorrection(time_t oldEpoch, time_t newEpoch);
void noteTimeSourceFallback(Source source, time_t currentEpoch);

CalendarKeys utcCalendarKeys(time_t epoch);
CalendarKeys localCalendarKeys(time_t epoch);
BoundaryRelation compareDateKey(uint32_t previousKey, uint32_t currentKey);
BoundaryRelation compareHourKey(uint32_t previousKey, uint32_t currentKey);
const char* boundaryRelationName(BoundaryRelation relation);

const char* sourceName(Source source);
const char* eventName(EventType type);
String formatEvent(const Event& event);

} // namespace RiotTimeService
