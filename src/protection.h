#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>

namespace protection {
constexpr uint32_t Magic = 0x50524f34, LegacyMagic = 0x50524f32, LatchingMagic = 0x50524f33;
constexpr uint16_t ProbePeriodMs = 100;
// User's thermal allowance for the potted 5.6-ohm resistor. Conservative USB
// voltage and -5 % resistance; ignore bridge losses and use actual PWM on-time.
constexpr unsigned ThermalBudgetMw = 2000, SupplyMaxMv = 5250, ResistanceMinMohm = 5320;
inline unsigned shortPowerMw(unsigned a, unsigned b) {
    const uint64_t denominator = uint64_t(ResistanceMinMohm) * 1024;
    return (uint64_t(SupplyMaxMv) * SupplyMaxMv * (a + b) + denominator - 1) / denominator;
}
inline bool needsMonitoring(unsigned a, unsigned b) { return shortPowerMw(a, b) > ThermalBudgetMw; }
constexpr unsigned MaxSafeDuty = uint64_t(ThermalBudgetMw)*ResistanceMinMohm*1024/(uint64_t(SupplyMaxMv)*SupplyMaxMv);
inline unsigned maxIgnored(unsigned bank) { return MaxSafeDuty/(bank==2 ? 10 : 5); }
struct Limits {
    uint32_t magic=0x4c494d31, signature=0;
    uint8_t automatic[3]={}, ignored[3]={};
    uint16_t noise[3]={}, error[3]={};
    uint32_t checksum=0;
};
inline uint32_t checksum(const Limits &c) {
    uint32_t h=2166136261u; const auto *p=reinterpret_cast<const uint8_t *>(&c);
    for (size_t i=0;i<offsetof(Limits,checksum);++i) h=(h^p[i])*16777619u;
    return h;
}
inline bool valid(const Limits &c) {
    if (c.magic!=0x4c494d31 || c.checksum!=checksum(c)) return false;
    for (unsigned i=0;i<3;++i) if (c.automatic[i]>100 || c.ignored[i]>100) return false;
    return true;
}
inline unsigned fadeIgnored(const Limits &c) {
    return std::min(100u,std::max(unsigned(c.ignored[0]),std::max(unsigned(c.ignored[1]),2u*c.ignored[2])));
}
inline const char *parseLimits(JsonObjectConst doc, Limits &c) {
    if (doc.isNull() || !doc.size()) return "Očekávám hranice ochrany.";
    auto next=c;
    if (doc.size()==1 && doc["automatic"].is<bool>() && doc["automatic"].as<bool>()) {
        memcpy(next.ignored,next.automatic,sizeof(next.ignored));
    } else {
        const char *names[]={"first","second","all"};
        for (JsonPairConst pair:doc) {
            unsigned i=0;while (i<3 && strcmp(pair.key().c_str(),names[i])) ++i;
            if (i==3) return "Neznámá hranice ochrany.";
            auto v=pair.value();
            if (!v.is<unsigned>() || v.as<unsigned>()>100) return "Hranice musí být celé procento od 0 do 100.";
            next.ignored[i]=v.as<unsigned>();
        }
    }
    next.checksum=checksum(next); c=next; return nullptr;
}
struct Config {
    uint32_t magic = Magic;
    uint16_t delayMs = 500;
    uint16_t referenceMv = 0, zeroMv = 0, reverseMv = 0;
    uint16_t settleUs = 2000;
    uint8_t thresholdPercent = 80;
    bool enabled = true, calibrated = false;
    uint8_t latchFault = 0; // Previously padding; zero means ordinary ON can retry.
    uint16_t profile[3][100] = {}; // Calibrated PWM estimates: first/second/combined; not an unphased mean.
    uint32_t checksum = 0;
};
static_assert(offsetof(Config, latchFault) == 17 && offsetof(Config, profile) == 18 &&
              offsetof(Config, checksum) == 620 && sizeof(Config) == 624,
              "Keep the 0.3.1 calibration layout for migration");
inline uint32_t checksum(const Config &c) {
    uint32_t hash = 2166136261u;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&c);
    for (size_t i = 0; i < offsetof(Config, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
inline bool valid(const Config &c) {
    return c.magic == Magic && c.latchFault <= 1 && c.delayMs >= 100 && c.delayMs <= 10000 &&
        c.thresholdPercent >= 30 && c.thresholdPercent <= 95 && c.settleUs == 2000 &&
        (!c.calibrated || (c.referenceMv <= 2450 &&
                          c.zeroMv <= 600 && c.referenceMv >= c.zeroMv + 100)) &&
        c.checksum == checksum(c);
}
inline bool load(Config &c) {
    if (c.magic == LegacyMagic || c.magic == LatchingMagic) {
        // Check the OLD bytes including padding before changing the new field.
        if (c.checksum != checksum(c)) return false;
        if (c.magic == LegacyMagic) c.latchFault = 0;
        c.magic = Magic; c.calibrated = false; c.checksum = checksum(c);
    }
    return valid(c);
}
inline bool restartBlocked(const Config &c, uint8_t fault) { return fault && c.latchFault; }
inline uint16_t threshold(const Config &c) {
    return c.zeroMv + (c.referenceMv - c.zeroMv) * c.thresholdPercent / 100u;
}
inline bool healthy(const Config &c, unsigned mv) {
    return mv >= threshold(c) && mv <= 2450;
}
// Consecutive bad probes, unsigned clock arithmetic, no accumulation across OFF
// or healthy readings. Gaps are a separate monitor fault, not a healthy sample.
struct Timer {
    bool pending = false;
    uint32_t since = 0;
    void clear() { pending = false; since = 0; }
    bool observe(bool good, uint32_t now, uint16_t delayMs) {
        if (good) { clear(); return false; }
        if (!pending) { pending = true; since = now; }
        return uint32_t(now - since) >= delayMs;
    }
};
inline const char *parse(JsonObjectConst doc, Config &config) {
    if (doc.isNull() || !doc.size()) return "Očekávám nastavení ochrany.";
    Config next = config;
    for (JsonPairConst p : doc) {
        const char *key = p.key().c_str();
        if (!strcmp(key, "enabled")) {
            if (!p.value().is<bool>()) return "enabled musí být true nebo false.";
            next.enabled = p.value();
        } else if (!strcmp(key, "latch_fault")) {
            if (!p.value().is<bool>()) return "latch_fault musí být true nebo false.";
            next.latchFault = p.value().as<bool>();
        } else if (!strcmp(key, "delay_ms")) {
            if (!p.value().is<unsigned>() || p.value().as<unsigned>() < 100 || p.value().as<unsigned>() > 10000)
                return "Prodleva musí být 100–10000 ms.";
            next.delayMs = p.value();
        } else if (!strcmp(key, "threshold_percent")) {
            if (!p.value().is<unsigned>() || p.value().as<unsigned>() < 30 || p.value().as<unsigned>() > 95)
                return "Práh musí být 30–95 % kalibrovaného signálu.";
            next.thresholdPercent = p.value();
        } else return "Neznámé pole nastavení ochrany.";
    }
    next.checksum = checksum(next); config = next; return nullptr;
}
}
