#pragma once

#include <time.h>
#include <stdint.h>

// Portable UTC struct-tm to Unix epoch conversion for embedded libc variants
// that do not expose timegm(). This header is intentionally C/C++ compatible
// because the P4 build force-includes it into framework translation units.
static inline int riotUtcIsLeapYear(int year){
  return (year%4==0) && ((year%100)!=0 || (year%400)==0);
}

static inline time_t riot_timegm(struct tm* t){
  if(!t)return (time_t)-1;
  const int year=t->tm_year+1900;
  const int month=t->tm_mon+1;
  if(year<1970 || month<1 || month>12 || t->tm_mday<1 || t->tm_mday>31)return (time_t)-1;

  static const uint16_t daysBeforeMonth[12]={0,31,59,90,120,151,181,212,243,273,304,334};
  int64_t days=0;
  for(int y=1970;y<year;y++)days+=riotUtcIsLeapYear(y)?366:365;
  days+=daysBeforeMonth[month-1];
  if(month>2 && riotUtcIsLeapYear(year))days++;
  days+=(int64_t)t->tm_mday-1;

  const int64_t seconds=days*86400LL+
    (int64_t)t->tm_hour*3600LL+
    (int64_t)t->tm_min*60LL+
    (int64_t)t->tm_sec;
  return (time_t)seconds;
}
