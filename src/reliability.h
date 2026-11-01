#pragma once
#include "light.h"
#include <ctime>

namespace reliability {
constexpr uint32_t WatchdogSeconds=60, HttpTimeoutMs=30000;
enum class NetworkAction { None, Retry, Station, Radio };
struct NetworkRecovery {
    uint32_t offlineAt=0, attemptAt=0, stationAt=0, radioAt=0;
    NetworkAction tick(uint32_t now,bool connected,bool configured,bool paused=false) {
        if (connected) {offlineAt=attemptAt=stationAt=radioAt=now;return NetworkAction::None;}
        if (!configured || paused) return NetworkAction::None;
        if (uint32_t(now-radioAt)>=600000) {
            radioAt=stationAt=attemptAt=now;return NetworkAction::Radio;
        }
        if (uint32_t(now-stationAt)>=120000) {
            stationAt=attemptAt=now;return NetworkAction::Station;
        }
        if (uint32_t(now-attemptAt)>=30000) {attemptAt=now;return NetworkAction::Retry;}
        return NetworkAction::None;
    }
};
// RTC memory survives software/watchdog resets, not loss of power. No flash writes
// per heartbeat. Checksum rejects an interrupted checkpoint or an old layout.
struct Resume {
    uint32_t magic=0x52534D32,sequence=0;
    lights::State light;
    bool manual=false;
    time_t until=0;
    uint32_t checksum=0;
};
inline uint32_t checksum(const Resume &r) {
    const auto *p=reinterpret_cast<const uint8_t *>(&r);
    uint32_t h=2166136261u;
    for (size_t i=0;i<offsetof(Resume,checksum);++i) h=(h^p[i])*16777619u;
    return h;
}
inline bool valid(const Resume &r) {
    return r.magic==0x52534D32 && lights::valid(r.light) && r.checksum==checksum(r);
}
inline const Resume *latest(const Resume (&slots)[2]) {
    const bool a=valid(slots[0]),b=valid(slots[1]);
    if (!a) return b?&slots[1]:nullptr;
    return b && int32_t(slots[1].sequence-slots[0].sequence)>0?&slots[1]:&slots[0];
}
}
