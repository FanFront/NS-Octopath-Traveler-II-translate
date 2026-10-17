#pragma once
#include <cstdint>
#include <span>
namespace ot2 {
struct CalendarTime {int16_t year;uint8_t month,day,hour,minute,second,pad;};
struct CalendarAdditionalInfo {uint32_t weekday,yearday;char zone[8];uint32_t dst;int32_t offset;};
static_assert(sizeof(CalendarTime)==8 && sizeof(CalendarAdditionalInfo)==24);
// SDK rules are opaque 0x4000-byte values. Both their producer and every
// imported consumer are host adapters. Store a validated TZif snapshot, not a
// pointer or timezone ID: copying a rule preserves its meaning independently
// of the default zone and subsequent database changes.
uint32_t time_zone_location(void* location36);
uint32_t time_zone_load(void* rule16384,const char* location36);
uint32_t time_zone_encode(void* rule16384,std::span<const uint8_t> tzif);
uint32_t time_zone_convert(CalendarTime*,CalendarAdditionalInfo*,int64_t seconds,const void* rule16384);
uint32_t time_zone_utc(CalendarTime*,CalendarAdditionalInfo*,int64_t seconds);
}
