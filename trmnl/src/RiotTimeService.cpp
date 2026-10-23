#include "RiotTimeService.h"

namespace RiotTimeService {

static EventCallback eventCallback = nullptr;
static uint32_t largeStepSeconds = 30;

static void emit(EventType type, Source source, time_t oldEpoch, time_t newEpoch, bool rtcOsf){
  Event e{};
  e.type = type;
  e.source = source;
  e.oldEpoch = oldEpoch;
  e.newEpoch = newEpoch;
  e.correctionSeconds = (int32_t)(newEpoch - oldEpoch);
  e.rtcOsf = rtcOsf;
  if(eventCallback) eventCallback(e);
}

static CalendarKeys makeKeys(const struct tm& t){
  CalendarKeys k{};
  const int year=t.tm_year+1900;
  const int month=t.tm_mon+1;
  const int day=t.tm_mday;
  const int hour=t.tm_hour;
  if(year<1970 || month<1 || month>12 || day<1 || day>31 || hour<0 || hour>23)return k;
  k.dateKey=(uint32_t)year*10000UL+(uint32_t)month*100UL+(uint32_t)day;
  k.hourKey=k.dateKey*100UL+(uint32_t)hour;
  k.valid=true;
  return k;
}

static BoundaryRelation compareKey(uint32_t previousKey,uint32_t currentKey){
  if(!previousKey || !currentKey)return BOUNDARY_INVALID;
  if(currentKey==previousKey)return BOUNDARY_SAME;
  return currentKey>previousKey?BOUNDARY_FORWARD:BOUNDARY_BACKWARD;
}

void setEventCallback(EventCallback cb){ eventCallback = cb; }
void setLargeStepThresholdSeconds(uint32_t seconds){ largeStepSeconds = seconds ? seconds : 1; }
uint32_t largeStepThresholdSeconds(){ return largeStepSeconds; }

void noteRtcLostPower(time_t rtcEpoch){
  emit(EVENT_RTC_LOST_POWER, SOURCE_RTC, rtcEpoch, rtcEpoch, true);
}

void noteRtcInitialized(time_t oldEpoch, time_t newEpoch){
  emit(EVENT_RTC_INITIALIZED, SOURCE_NTP, oldEpoch, newEpoch, false);
}

void noteNtpCorrection(time_t oldEpoch, time_t newEpoch){
  const int64_t delta = (int64_t)newEpoch - (int64_t)oldEpoch;
  emit((delta >= (int64_t)largeStepSeconds || delta <= -(int64_t)largeStepSeconds)
         ? EVENT_TIME_STEP_LARGE : EVENT_TIME_SYNC_NTP,
       SOURCE_NTP, oldEpoch, newEpoch, false);
}

void noteTimeSourceFallback(Source source, time_t currentEpoch){
  emit(EVENT_TIME_SOURCE_FALLBACK, source, currentEpoch, currentEpoch, false);
}

CalendarKeys utcCalendarKeys(time_t epoch){
  CalendarKeys k{};
  if(epoch<=1000000000)return k;
  struct tm t{};
  if(!gmtime_r(&epoch,&t))return k;
  return makeKeys(t);
}

CalendarKeys localCalendarKeys(time_t epoch){
  CalendarKeys k{};
  if(epoch<=1000000000)return k;
  struct tm t{};
  if(!localtime_r(&epoch,&t))return k;
  return makeKeys(t);
}

BoundaryRelation compareDateKey(uint32_t previousKey,uint32_t currentKey){return compareKey(previousKey,currentKey);}
BoundaryRelation compareHourKey(uint32_t previousKey,uint32_t currentKey){return compareKey(previousKey,currentKey);}

const char* boundaryRelationName(BoundaryRelation relation){
  switch(relation){
    case BOUNDARY_BACKWARD:return "BACKWARD";
    case BOUNDARY_SAME:return "SAME";
    case BOUNDARY_FORWARD:return "FORWARD";
    default:return "INVALID";
  }
}

const char* sourceName(Source source){
  switch(source){
    case SOURCE_RTC: return "RTC";
    case SOURCE_NTP: return "NTP";
    default: return "NONE";
  }
}

const char* eventName(EventType type){
  switch(type){
    case EVENT_RTC_INITIALIZED: return "RTC_INITIALIZED";
    case EVENT_TIME_SYNC_NTP: return "TIME_SYNC_NTP";
    case EVENT_TIME_STEP_LARGE: return "TIME_STEP_LARGE";
    case EVENT_RTC_LOST_POWER: return "RTC_LOST_POWER";
    case EVENT_TIME_SOURCE_FALLBACK: return "TIME_SOURCE_FALLBACK";
    default: return "TIME_EVENT";
  }
}

String formatEvent(const Event& event){
  char b[160];
  snprintf(b,sizeof(b),"%s source=%s old=%lld new=%lld correction=%lds%s",
    eventName(event.type),sourceName(event.source),
    (long long)event.oldEpoch,(long long)event.newEpoch,
    (long)event.correctionSeconds,event.rtcOsf?" OSF=SET":"");
  return String(b);
}

} // namespace RiotTimeService
