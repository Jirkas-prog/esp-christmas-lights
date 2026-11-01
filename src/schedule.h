#pragma once
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>

namespace schedule {
struct Zone { const char *name, *rule; };
constexpr Zone Zones[] = {
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"UTC", "UTC0"}
};
constexpr unsigned ZoneCount = sizeof(Zones)/sizeof(Zones[0]);
enum Kind : uint8_t { Fixed, Sunrise, Sunset };
struct Point { uint8_t kind; uint16_t minute; int16_t offset;
    constexpr Point(uint8_t k=Fixed,uint16_t m=0,int16_t o=0):kind(k),minute(m),offset(o) {}
};
struct Window { bool enabled,overnight; Point start,end;
    constexpr Window(bool e=true,Point a={Sunset,0,0},Point b={},bool cross=true):enabled(e),overnight(cross),start(a),end(b) {}
};
struct Config {
    uint32_t magic=0x53434831; // SCH1, separate NVS record; old light/network layouts stay intact.
    float latitude=0.0f, longitude=0.0f; // Configure your location before enabling solar scheduling.
    uint8_t zone=0;
    bool enabled=false, onePercentOff=false;
    Window windows[2] = {{true,{Sunset,0,0},{Fixed,0,0}}, {true,{Fixed,300,0},{Sunrise,0,0},false}};
    uint32_t checksum=0;
};
inline uint32_t checksum(const Config &c) {
    const auto *p=reinterpret_cast<const uint8_t *>(&c);
    uint32_t h=2166136261u;
    for (size_t i=0;i<offsetof(Config,checksum);++i) h=(h^p[i])*16777619u;
    return h;
}
inline bool valid(const Config &c) {
    if (c.magic!=0x53434831 || c.zone>=ZoneCount || !std::isfinite(c.latitude) || !std::isfinite(c.longitude) ||
        c.latitude < -90 || c.latitude > 90 || c.longitude < -180 || c.longitude > 180) return false;
    for (auto &w:c.windows) for (auto &p:{w.start,w.end})
        if (p.kind>Sunset || p.minute>1439 || p.offset < -720 || p.offset > 720) return false;
    return true;
}
inline const char *kindName(unsigned k) { return k==Sunrise ? "sunrise" : k==Sunset ? "sunset" : "fixed"; }
inline bool textEquals(JsonVariantConst v,const char *s) {
    return v.is<const char *>() && v.as<JsonString>().size()==strlen(s) && !strcmp(v.as<const char *>(),s);
}
inline bool pointParse(JsonVariantConst v,Point &p) {
    if (!v.is<JsonObjectConst>() || v.size()!=3) return false;
    auto o=v.as<JsonObjectConst>();
    bool found=false;
    for (unsigned i=0;i<3;++i) if (textEquals(o["kind"],kindName(i))) {p.kind=i;found=true;}
    if (!found || !o["minute"].is<unsigned>() || o["minute"].as<unsigned>()>1439 ||
        !o["offset"].is<int>() || o["offset"].as<int>() < -720 || o["offset"].as<int>() > 720) return false;
    p.minute=o["minute"]; p.offset=o["offset"]; return true;
}
inline const char *parse(JsonObjectConst o,Config &c) {
    if (o.isNull() || !o.size()) return "Prázdné nastavení plánu.";
    auto next=c;
    for (JsonPairConst p:o) {
        const char *k=p.key().c_str(); auto v=p.value();
        if (!strcmp(k,"enabled") || !strcmp(k,"one_percent_off")) {
            if (!v.is<bool>()) return "Přepínač musí být true/false.";
            if (!strcmp(k,"enabled")) next.enabled=v; else next.onePercentOff=v;
        } else if (!strcmp(k,"latitude") || !strcmp(k,"longitude")) {
            if (!v.is<float>()) return "Souřadnice musí být číslo ve stupních.";
            if (!strcmp(k,"latitude")) next.latitude=v; else next.longitude=v;
        } else if (!strcmp(k,"timezone")) {
            bool found=false;
            for (unsigned i=0;i<ZoneCount;++i) if (textEquals(v,Zones[i].name)) {next.zone=i;found=true;}
            if (!found) return "Neznámé časové pásmo.";
        } else if (!strcmp(k,"windows")) {
            if (!v.is<JsonArrayConst>() || v.size()!=2) return "Plán musí mít dva intervaly.";
            for (unsigned i=0;i<2;++i) {
                auto w=v[i];
                if (!w.is<JsonObjectConst>() || w.size()!=4 || !w["enabled"].is<bool>() || !w["overnight"].is<bool>() ||
                    !pointParse(w["start"],next.windows[i].start) || !pointParse(w["end"],next.windows[i].end))
                    return "Neplatný interval. Čas 0–1439 min, posun −720 až +720 min.";
                next.windows[i].enabled=w["enabled"]; next.windows[i].overnight=w["overnight"];
            }
        } else return "Neznámé pole nastavení plánu.";
    }
    if (!valid(next)) return "Šířka musí být −90 až 90°, délka −180 až 180°.";
    next.checksum=checksum(next); c=next; return nullptr;
}
inline void pointJson(JsonObject o,const Point &p) {o["kind"]=kindName(p.kind);o["minute"]=p.minute;o["offset"]=p.offset;}
inline void json(JsonObject o,const Config &c) {
    o["enabled"]=c.enabled;o["one_percent_off"]=c.onePercentOff;
    o["latitude"]=c.latitude;o["longitude"]=c.longitude;o["timezone"]=Zones[c.zone].name;
    auto a=o.createNestedArray("windows");
    for (auto &w:c.windows) {auto v=a.createNestedObject();v["enabled"]=w.enabled;v["overnight"]=w.overnight;pointJson(v.createNestedObject("start"),w.start);pointJson(v.createNestedObject("end"),w.end);}
}
// Patch one interval without requiring the caller to read/replace the other one.
inline const char *patchWindow(JsonObjectConst o,unsigned index,Config &c) {
    if (index>1 || o.isNull() || !o.size()) return "Neplatná změna intervalu.";
    auto next=c;auto &w=next.windows[index];
    for (JsonPairConst pair:o) {
        const char *key=pair.key().c_str();auto v=pair.value();
        if (!strcmp(key,"enabled") || !strcmp(key,"overnight")) {
            if (!v.is<bool>()) return "Přepínač intervalu musí být true/false.";
            if (!strcmp(key,"enabled")) w.enabled=v;else w.overnight=v;
        } else if (!strcmp(key,"start") || !strcmp(key,"end")) {
            if (!v.is<JsonObjectConst>() || !v.size()) return "Očekávám změnu začátku nebo konce.";
            Point &p=!strcmp(key,"start")?w.start:w.end;
            StaticJsonDocument<256> doc;pointJson(doc.to<JsonObject>(),p);
            for (JsonPairConst field:v.as<JsonObjectConst>()) {
                const char *k=field.key().c_str();
                if (strcmp(k,"kind") && strcmp(k,"minute") && strcmp(k,"offset")) return "Neznámé pole času.";
                doc[k]=field.value();
            }
            if (!pointParse(doc.as<JsonVariantConst>(),p)) return "Neplatný čas nebo posun intervalu.";
        } else return "Neznámé pole intervalu.";
    }
    next.checksum=checksum(next);c=next;return nullptr;
}
inline tm local(time_t t) {
    tm d{};
#ifdef _WIN32
    localtime_s(&d,&t);
#else
    localtime_r(&t,&d);
#endif
    return d;
}
inline tm utc(time_t t) {
    tm d{};
#ifdef _WIN32
    gmtime_s(&d,&t);
#else
    gmtime_r(&t,&d);
#endif
    return d;
}
inline bool clockValid(time_t t) {return t>=1704067200; /* 2024-01-01 */}
inline bool leap(int year) {return year%4==0 && (year%100!=0 || year%400==0);}
inline int dayKey(const tm &d) {return (d.tm_year+1900)*10000+(d.tm_mon+1)*100+d.tm_mday;}
inline tm dateShift(tm d,int days) {
    d.tm_hour=12; d.tm_min=d.tm_sec=0; d.tm_mday+=days; d.tm_isdst=-1;
    return local(mktime(&d)); // Noon avoids DST gaps and repeated hours at midnight.
}
inline time_t utcMidnight(const tm &d) {
    int days=0;
    for (int y=1970;y<d.tm_year+1900;++y) days+=leap(y)?366:365;
    return time_t(int64_t(days+d.tm_yday)*86400);
}
// NOAA fractional-year approximation, 90.833° zenith. Returns UTC, never a
// timezone-adjusted pseudo-epoch. Missing polar events are represented by zero.
// https://gml.noaa.gov/grad/solcalc/solareqns.PDF
inline time_t sun(const tm &date,float latitude,float longitude,bool rise) {
    constexpr double Pi=3.14159265358979323846, Rad=Pi/180;
    double minutes=720-4*longitude;
    for (int iteration=0;iteration<3;++iteration) {
        double g=2*Pi/(leap(date.tm_year+1900)?366:365)*(date.tm_yday+(minutes/60-12)/24);
        double eq=229.18*(.000075+.001868*cos(g)-.032077*sin(g)-.014615*cos(2*g)-.040849*sin(2*g));
        double decl=.006918-.399912*cos(g)+.070257*sin(g)-.006758*cos(2*g)+.000907*sin(2*g)-.002697*cos(3*g)+.00148*sin(3*g);
        double c=(cos(90.833*Rad)-sin(latitude*Rad)*sin(decl))/(cos(latitude*Rad)*cos(decl));
        if (!std::isfinite(c) || c < -1 || c > 1) return 0;
        double ha=acos(c)/Rad;
        minutes=720-4*(longitude+(rise?ha:-ha))-eq;
    }
    return utcMidnight(date)+time_t(lround(minutes*60));
}
inline time_t event(const Config &c,const tm &date,const Point &p) {
    if (p.kind==Fixed) {
        tm d=date;d.tm_hour=p.minute/60;d.tm_min=p.minute%60;d.tm_sec=0;d.tm_isdst=-1;
        return mktime(&d); // libc handles local DST gaps/repeated hours consistently.
    }
    // Near the date line the event's UTC date can differ from the local date.
    for (int shift=-1;shift<=1;++shift) {
        time_t at=sun(dateShift(date,shift),c.latitude,c.longitude,p.kind==Sunrise);
        if (at && dayKey(local(at))==dayKey(date)) return at+p.offset*60;
    }
    return 0;
}
struct Span {time_t start,end;constexpr Span(time_t a=0,time_t b=0):start(a),end(b) {}};
inline Span interval(const Config &c,const tm &date,const Window &w) {
    if (!w.enabled) return {};
    time_t a=event(c,date,w.start), b=event(c,date,w.end);
    if (!a || !b) return {};
    if (b<=a) {if (!w.overnight) return {};b=event(c,dateShift(date,1),w.end);}
    return b>a ? Span{a,b} : Span{};
}
// Only the evening -> next morning gap; never the daytime gap. Select the
// next morning start even if its interval is invalid, so a skipped sunrise
// cannot accidentally join nights across several days.
inline Span nightInterval(const Config &c,const tm &date) {
    if (!c.windows[0].enabled || !c.windows[1].enabled) return {};
    auto evening=interval(c,date,c.windows[0]);if (!evening.start) return {};
    time_t first=0;Span morning;
    auto base=local(evening.start);
    for (int day=-1;day<=2;++day) {
        auto d=dateShift(base,day);time_t start=event(c,d,c.windows[1].start);
        if (start>=evening.start && (!first || start<first)) {first=start;morning=interval(c,d,c.windows[1]);}
    }
    return morning.start>evening.end ? Span{evening.end,morning.start} : Span{};
}
enum Phase : uint8_t { Off, Evening, Morning, Night };
inline const char *phaseName(Phase p) {return p==Evening?"evening":p==Morning?"morning":p==Night?"night":"off";}
struct Plan {
    Span spans[12]; unsigned count=0; int date=0;
    Phase owners[12];
    Span nights[6];unsigned nightCount=0;
    time_t sunrise=0,sunset=0,horizon=0;
    void build(const Config &c,time_t now) {
        count=0;nightCount=0;tm d=local(now);date=dayKey(d);
        horizon=event(c,dateShift(d,3),{Fixed,0,0});
        sunrise=event(c,d,{Sunrise,0,0}); sunset=event(c,d,{Sunset,0,0});
        for (int day=-2;day<=3;++day) for (unsigned i=0;i<2;++i) {
            Span s=interval(c,dateShift(d,day),c.windows[i]);
            if (s.start) {owners[count]=i?Morning:Evening;spans[count++]=s;}
        }
        for (int day=-2;day<=3;++day) {
            auto gap=nightInterval(c,dateShift(d,day));if (gap.start) nights[nightCount++]=gap;
        }
    }
    bool active(time_t now) const {
        for (unsigned i=0;i<count;++i) if (now>=spans[i].start && now<spans[i].end) return true;
        return false;
    }
    Phase phase(time_t now,bool nightEnabled) const {
        // Preserve window identity across ON -> ON edges. Morning wins overlaps.
        Phase result=Off;
        for (unsigned i=0;i<count;++i) if (now>=spans[i].start && now<spans[i].end) {
            if (owners[i]==Morning) return Morning;
            result=Evening;
        }
        if (result!=Off) return result;
        if (nightEnabled) for (unsigned i=0;i<nightCount;++i) if (now>=nights[i].start && now<nights[i].end) return Night;
        return Off;
    }
    time_t next(time_t now,bool nightEnabled=false) const {
        time_t result=0;
        auto consider=[&](time_t t) {
            if (t>now && t<horizon && (!result || t<result) && phase(t,nightEnabled)!=phase(t-1,nightEnabled)) result=t;
        };
        for (unsigned i=0;i<count;++i) {consider(spans[i].start);consider(spans[i].end);}
        if (nightEnabled) for (unsigned i=0;i<nightCount;++i) {consider(nights[i].start);consider(nights[i].end);}
        return result;
    }
};
}
