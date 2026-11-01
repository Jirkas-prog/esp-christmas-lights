#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>
#include <cmath>
#include <algorithm>

namespace lights {
constexpr unsigned Period = 1024, Half = Period / 2, Gap = 12;
// Append only: persisted states and presets store these byte values.
enum class Mode : uint8_t { All, First, Second, Alternate, Crossfade, SoftCrossfade, Breathe, Tide, Candlelight };
constexpr unsigned ModeCount = 9;
struct State {
    bool on = true;
    uint8_t brightness = 100;
    Mode mode = Mode::All;
    uint16_t intervalMs = 1000;
};
struct Preset {
    uint8_t brightness;
    Mode mode;
    Preset(uint8_t b=100, Mode m=Mode::All):brightness(b),mode(m) {}
};
inline bool valid(const Preset &p, unsigned minimum=2) { return p.brightness>=minimum && p.brightness<=100 && unsigned(p.mode)<ModeCount; }
inline const char *modeName(Mode mode) {
    switch (mode) {
        case Mode::First: return "first";
        case Mode::Second: return "second";
        case Mode::Alternate: return "alternate";
        case Mode::Crossfade: return "crossfade";
        case Mode::SoftCrossfade: return "soft_crossfade";
        case Mode::Breathe: return "breathe";
        case Mode::Tide: return "tide";
        case Mode::Candlelight: return "candlelight";
        default: return "all";
    }
}
inline const char *modeLabel(Mode mode) {
    switch (mode) {
        case Mode::First: return "First Half (První polovina)";
        case Mode::Second: return "Second Half (Druhá polovina)";
        case Mode::Alternate: return "Alternate (Střídání)";
        case Mode::Crossfade: return "Linear Crossfade (Lineární prolínání)";
        case Mode::SoftCrossfade: return "Soft Crossfade (Jemné prolínání)";
        case Mode::Breathe: return "Breathe (Dýchání)";
        case Mode::Tide: return "Tide (Přelévání)";
        case Mode::Candlelight: return "Candlelight (Svíčka)";
        default: return "All (Celý řetěz)";
    }
}
inline bool varyingPower(Mode mode) {return mode==Mode::Breathe || mode==Mode::Candlelight;}
inline uint32_t cycleMs(const State &s) {
    unsigned factor=s.mode==Mode::SoftCrossfade?4:s.mode==Mode::Breathe?6:s.mode==Mode::Tide?8:s.mode==Mode::Candlelight?12:2;
    return factor*(factor>2?std::max(500u,unsigned(s.intervalMs)):unsigned(s.intervalMs));
}
inline bool valid(const State &s) {
    return s.brightness <= 100 && unsigned(s.mode) < ModeCount && s.intervalMs >= 100 && s.intervalMs <= 10000;
}
// Validate into a copy: a malformed command never partially changes the light.
inline const char *parse(JsonObjectConst doc, State &s) {
    if (doc.isNull() || !doc.size()) return "Očekávám neprázdný JSON objekt.";
    State next = s;
    for (JsonPairConst p : doc) {
        const char *key = p.key().c_str();
        if (!strcmp(key, "state")) {
            if (!p.value().is<const char *>()) return "state musí být ON nebo OFF.";
            const char *v = p.value();
            if (p.value().as<JsonString>().size() != strlen(v)) return "Neplatný text stavu.";
            if (!strcmp(v, "ON")) next.on = true;
            else if (!strcmp(v, "OFF")) next.on = false;
            else return "state musí být ON nebo OFF.";
        } else if (!strcmp(key, "brightness")) {
            if (!p.value().is<unsigned>() || p.value().as<unsigned>() > 100) return "Jas musí být 0–100.";
            next.brightness = p.value();
        } else if (!strcmp(key, "effect")) {
            if (!p.value().is<const char *>()) return "Neplatný režim.";
            const char *v = p.value(); bool found = false;
            if (p.value().as<JsonString>().size() != strlen(v)) return "Neplatný text režimu.";
            for (unsigned i = 0; i < ModeCount; ++i) if (!strcmp(v, modeName(Mode(i))) || !strcmp(v, modeLabel(Mode(i)))) { next.mode = Mode(i); found = true; }
            if (!found) return "Neznámý režim.";
        } else if (!strcmp(key, "interval_ms")) {
            if (!p.value().is<unsigned>() || p.value().as<unsigned>() < 100 || p.value().as<unsigned>() > 10000)
                return "Interval musí být 100–10000 ms.";
            next.intervalMs = p.value();
        } else return "Neznámé pole příkazu.";
    }
    s = next;
    return nullptr;
}
inline const char *parsePreset(JsonObjectConst doc, Preset &p, unsigned minimum=2) {
    for (JsonPairConst field:doc) if (strcmp(field.key().c_str(),"brightness") && strcmp(field.key().c_str(),"effect"))
        return "Předvolba obsahuje jen brightness a effect.";
    State next;next.brightness=p.brightness;next.mode=p.mode;
    if (const char *error=parse(doc,next)) return error;
    Preset candidate{next.brightness,next.mode};
    if (!valid(candidate,minimum)) return minimum ? "Jas předvolby musí být 2–100 %." : "Jas musí být 0–100 %.";
    p=candidate;return nullptr;
}
// A plain ON starts from the preset. Explicit fields win; a slider changes
// brightness and power together, while an explicit OFF always remains OFF.
inline const char *control(JsonObjectConst doc, State &s, const Preset &preset, bool onePercentOff) {
    State next=s;
    if (doc["state"]=="ON" && !doc.containsKey("brightness")) {
        // HA also sends ON with slider/effect changes. Do not reset the other
        // live parameter; only a plain power command restores the whole preset.
        if (!s.on || !doc.containsKey("effect")) next.brightness=preset.brightness;
        if (!doc.containsKey("effect")) next.mode=preset.mode;
    }
    if (const char *error=parse(doc,next)) return error;
    if (doc.containsKey("brightness") && doc["state"]!="OFF") next.on=true;
    if (next.brightness==0 || (onePercentOff && next.brightness==1)) next.on=false;
    s=next;return nullptr;
}
inline void json(JsonObject o, const Preset &p) {o["brightness"]=p.brightness;o["effect"]=modeName(p.mode);}
// Separate NVS record: keep the existing schedule and calibration layouts intact.
struct WindowPresets { Preset evening, morning; };
inline bool valid(const WindowPresets &p) {return valid(p.evening,0) && valid(p.morning,0);}
inline const char *parsePresets(JsonObjectConst doc, WindowPresets &p) {
    if (doc.isNull() || !doc.size()) return "Očekávám večerní nebo ranní předvolbu.";
    auto next=p;
    for (JsonPairConst field:doc) {
        const char *key=field.key().c_str();
        if (strcmp(key,"evening") && strcmp(key,"morning")) return "Neznámá předvolba intervalu.";
        auto &preset=!strcmp(key,"evening")?next.evening:next.morning;
        if (const char *error=parsePreset(field.value().as<JsonObjectConst>(),preset,0)) return error;
    }
    p=next;return nullptr;
}
inline void json(JsonObject o, const WindowPresets &p) {
    json(o.createNestedObject("evening"),p.evening);json(o.createNestedObject("morning"),p.morning);
}
struct Output { unsigned a, b; };
// Rebase once per complete cycle so millis() wrapping cannot jump the effect.
inline uint32_t phaseTime(uint32_t now, uint32_t &started, uint16_t intervalMs) {
    uint32_t elapsed = now - started, cycle = 2u * intervalMs;
    started += (elapsed / cycle) * cycle;
    return elapsed % cycle;
}
inline uint32_t phaseTime(uint32_t now, uint32_t &started, const State &s) {
    return phaseTime(now,started,uint16_t(cycleMs(s)/2));
}
inline Output output(const State &s, uint32_t elapsedMs) {
    if (!s.on) return {0, 0};
    unsigned duty = (Half - Gap) * s.brightness / 100;
    if (s.mode>=Mode::SoftCrossfade) {
        constexpr float Pi=3.14159265358979323846f;
        float x=2*Pi*(elapsedMs%cycleMs(s))/cycleMs(s);
        float wave=(1-std::cos(x))*.5f;
        if (s.mode==Mode::SoftCrossfade) {
            unsigned b=unsigned(std::lround(duty*wave));return {duty-b,b};
        }
        if (s.mode==Mode::Tide) {
            unsigned floor=(duty+3)/4,b=floor+unsigned(std::lround((duty-floor)*wave));
            return {duty+floor-b,b}; // Constant total light; both banks stay gently lit.
        }
        if (s.mode==Mode::Breathe) {
            float peak=1-wave;
            unsigned level=unsigned(std::lround(duty*(.2f+.8f*peak*peak)));
            return {level,level}; // 20..100 %, eased brightness, no full blackout.
        }
        // Smooth, unequal harmonics: organic candle motion without abrupt random steps.
        float a=.76f+.13f*std::sin(x)+.07f*std::sin(3*x+.7f)+.04f*std::sin(7*x+1.4f);
        float b=.76f+.13f*std::sin(x+2)+.07f*std::sin(2*x+.3f)+.04f*std::sin(5*x+1.1f);
        return {unsigned(std::lround(duty*a)),unsigned(std::lround(duty*b))};
    }
    uint32_t phase = elapsedMs % (2u * s.intervalMs);
    bool second = phase >= s.intervalMs;
    if (s.mode == Mode::Crossfade) {
        uint32_t towardB = second ? 2u * s.intervalMs - phase : phase;
        unsigned b = duty * towardB / s.intervalMs;
        // Constant combined duty: no dark gap or doubled brightness at the midpoint.
        return {duty - b, b};
    }
    bool a = s.mode == Mode::All || s.mode == Mode::First || (s.mode == Mode::Alternate && !second);
    bool b = s.mode == Mode::All || s.mode == Mode::Second || (s.mode == Mode::Alternate && second);
    // Each bank retains the same duty even when the other bank is disabled.
    return {a ? duty : 0, b ? duty : 0};
}
inline void json(JsonDocument &doc, const State &s) {
    doc["state"] = s.on ? "ON" : "OFF";
    doc["brightness"] = s.brightness;
    doc["effect"] = modeName(s.mode);
    doc["effect_name"] = modeLabel(s.mode);
    doc["effect_cycle_ms"] = s.mode>=Mode::Alternate?cycleMs(s):0;
    doc["interval_ms"] = s.intervalMs;
}
}
