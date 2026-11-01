#include "../src/schedule.h"
#include <cstdlib>

static time_t testDate(int y,int m,int d,int h=12,int min=0) {
    tm date{};date.tm_year=y-1900;date.tm_mon=m-1;date.tm_mday=d;date.tm_hour=h;date.tm_min=min;date.tm_isdst=-1;
    return mktime(&date);
}
static void scheduleTests() {
    _putenv_s("TZ","UTC0");_tzset();
    using namespace schedule;
    Config c;assert(valid(c));assert(!c.enabled && !c.onePercentOff);
    assert(c.latitude==0 && c.longitude==0);
    // Public reference site: Royal Observatory Greenwich; never an installation location.
    const auto reference=[] { Config value;value.latitude=51.4779f;value.longitude=0;return value; };
    assert(!clockValid(0) && clockValid(testDate(2026,10,22)));
    // Known solar times, rounded to minutes; NOAA's approximation is not an ephemeris.
    auto date=local(testDate(2026,6,21));
    assert(std::abs(double(sun(date,51.4779f,0,true)-testDate(2026,6,21,3,43)))<240);
    assert(std::abs(double(sun(date,51.4779f,0,false)-testDate(2026,6,21,20,21)))<240);
    assert(sun(date,89,0,true)==0 && sun(date,89,0,false)==0);
    assert(sun(local(testDate(2026,12,21)),89,0,true)==0);
    assert(utcMidnight(local(testDate(2024,3,1)))==testDate(2024,3,1,0));
    assert(dayKey(dateShift(local(testDate(2026,12,31)),1))==20270101);
    // Offset sign, UTC arithmetic and date-line rollover.
    c=reference();date=local(testDate(2026,10,22));
    assert(event(c,date,{Sunset,0,-30})==event(c,date,{Sunset,0,0})-1800);
    assert(event(c,date,{Sunrise,0,30})==event(c,date,{Sunrise,0,0})+1800);
    c.longitude=179;
    assert(dayKey(local(event(c,date,{Sunrise,0,0})))==dayKey(date));
    c.longitude=-179;
    assert(dayKey(local(event(c,date,{Sunset,0,0})))==dayKey(date));
    c=reference();
    // Evening to midnight and morning to sunrise do not illuminate the gap.
    Plan plan;auto now=testDate(2026,10,22);plan.build(c,now);
    assert(!plan.active(testDate(2026,10,22,12)));
    assert(plan.active(testDate(2026,10,22,23,59)));
    assert(!plan.active(testDate(2026,10,23,0)));
    assert(plan.active(testDate(2026,10,22,5)));
    assert(!plan.active(plan.sunrise));
    // Early summer sunrise BEFORE 05:00 must skip the morning, not run all day.
    date=local(testDate(2026,6,21));assert(!interval(c,date,c.windows[1]).start);
    plan.build(c,testDate(2026,6,21));assert(!plan.active(testDate(2026,6,21,12)));
    // Full overnight interval uses tomorrow's solar event, not today's +24h.
    c.windows[0].end={Sunrise,0,0};c.windows[1].enabled=false;
    date=local(testDate(2026,10,22));auto night=interval(c,date,c.windows[0]);
    assert(night.end==event(c,dateShift(date,1),{Sunrise,0,0}));
    // Adjacent ON intervals change presets directly at the shared boundary.
    c.windows[0]={true,{Fixed,60,0},{Fixed,180,0},false};
    c.windows[1]={true,{Fixed,180,0},{Fixed,240,0},false};
    plan.build(c,now);assert(plan.active(testDate(2026,10,22,3)));
    assert(plan.next(testDate(2026,10,22,2))==testDate(2026,10,22,3));
    assert(plan.phase(testDate(2026,10,22,2,59),false)==Evening);
    assert(plan.phase(testDate(2026,10,22,3),false)==Morning);
    assert(plan.next(testDate(2026,10,22,3))==testDate(2026,10,22,4));
    assert(!plan.active(testDate(2026,10,22,4)));
    // Morning wins overlaps, including equal starts. Returning to a still
    // active evening window is another preset boundary, never an OFF pulse.
    c.windows[0].end={Fixed,300};c.windows[1].start={Fixed,120};
    plan.build(c,now);
    assert(plan.next(testDate(2026,10,22,1))==testDate(2026,10,22,2));
    assert(plan.phase(testDate(2026,10,22,2),true)==Morning);
    assert(plan.next(testDate(2026,10,22,2),true)==testDate(2026,10,22,4));
    assert(plan.phase(testDate(2026,10,22,4),true)==Evening);
    assert(plan.next(testDate(2026,10,22,4),true)==testDate(2026,10,22,5));
    c.windows[0].start=c.windows[1].start;plan.build(c,now);
    assert(plan.phase(testDate(2026,10,22,2),false)==Morning);
    c.windows[0]={true,{Fixed,0,0},{Fixed,0,0},true};c.windows[1].enabled=false;
    plan.build(c,now);assert(plan.active(now) && plan.next(now)==0);
    // Night dimming fills only evening -> morning, including midnight. Main
    // windows have priority, and ON -> dim ON -> main ON are real boundaries.
    c=reference();c.windows[0]={true,{Fixed,18*60},{Fixed,23*60},false};
    c.windows[1]={true,{Fixed,5*60},{Fixed,7*60},false};
    date=local(now);auto gap=nightInterval(c,date);
    assert(gap.start==testDate(2026,10,22,23) && gap.end==testDate(2026,10,23,5));
    plan.build(c,now);
    assert(plan.phase(testDate(2026,10,22,22),true)==Evening);
    assert(plan.phase(gap.start,true)==Night && plan.phase(gap.start-1,true)==Evening);
    assert(plan.phase(testDate(2026,10,23,2),false)==Off);
    assert(plan.phase(gap.end,true)==Morning && plan.phase(gap.end-1,true)==Night);
    assert(plan.phase(testDate(2026,10,23,7),true)==Off && plan.phase(now,true)==Off);
    assert(plan.next(testDate(2026,10,22,22),true)==gap.start);
    assert(plan.next(gap.start,true)==gap.end);
    assert(plan.next(gap.end,true)==testDate(2026,10,23,7));
    c.windows[0].end={Fixed,0};c.windows[0].overnight=true;
    assert(nightInterval(c,date).start==testDate(2026,10,23,0));
    c.windows[0].end={Fixed,2*60};assert(nightInterval(c,date).start==testDate(2026,10,23,2));
    for (unsigned end:{5*60,6*60}) {
        c.windows[0].end={Fixed,uint16_t(end)};assert(!nightInterval(c,date).start);
        plan.build(c,now);assert(plan.next(testDate(2026,10,22,23),true)==testDate(2026,10,23,5));
        assert(plan.phase(testDate(2026,10,23,5),true)==Morning);
        assert(plan.next(testDate(2026,10,23,5),true)==testDate(2026,10,23,7));
    }
    c.windows[0].end={Fixed,23*60};c.windows[1].enabled=false;assert(!nightInterval(c,date).start);
    c=reference();assert(!nightInterval(c,local(testDate(2026,6,21))).start); // skipped morning
    c.latitude=89;assert(!nightInterval(c,local(testDate(2026,6,21))).start); // no sun event
    c=reference();c.windows[1] = {true,{Sunrise,0,-30},{Sunrise,0,30},false};
    gap=nightInterval(c,date);assert(gap.end==event(c,dateShift(date,1),{Sunrise,0,-30}));
    // Two intervals may also be on the same civil day (useful for testing).
    c.windows[0]={true,{Fixed,60},{Fixed,120},false};c.windows[1]={true,{Fixed,180},{Fixed,240},false};
    gap=nightInterval(c,date);assert(gap.start==testDate(2026,10,22,2) && gap.end==testDate(2026,10,22,3));
    plan.build(c,now);assert(plan.phase(testDate(2026,10,22,2,30),true)==Night && plan.phase(now,true)==Off);
    // Config validation is atomic; saved data cannot silently accept broken inputs.
    for (auto input:{"{\"latitude\":91}","{\"longitude\":-181}","{\"enabled\":1}",
        "{\"timezone\":\"bad\"}","{\"timezone\":\"UTC\\u0000bad\"}","{\"windows\":[]}","{\"unknown\":true}"}) {
        StaticJsonDocument<1024> doc;assert(!deserializeJson(doc,input));auto before=c;
        assert(parse(doc.as<JsonObjectConst>(),c));assert(!memcmp(&before,&c,sizeof(c)));
    }
    StaticJsonDocument<2048> doc;json(doc.to<JsonObject>(),c);
    Config copy;assert(!parse(doc.as<JsonObjectConst>(),copy));assert(valid(copy));assert(copy.checksum==checksum(copy));
    doc["windows"][1]["end"]["offset"]=721;
    assert(parse(doc.as<JsonObjectConst>(),copy));
    doc.clear();deserializeJson(doc,R"({"start":{"offset":-25},"enabled":false})");
    auto before=copy;assert(!patchWindow(doc.as<JsonObjectConst>(),0,copy));
    assert(copy.windows[0].start.offset==-25 && !copy.windows[0].enabled);
    assert(copy.windows[1].enabled==before.windows[1].enabled && copy.windows[1].start.minute==before.windows[1].start.minute);
    for (auto input:{R"({"start":{"offset":721}})",R"({"end":{"minute":1440}})",R"({"start":{"unknown":1}})",R"({"enabled":1})",R"({"unknown":0})"}) {
        doc.clear();deserializeJson(doc,input);before=copy;
        assert(patchWindow(doc.as<JsonObjectConst>(),0,copy));assert(!memcmp(&before,&copy,sizeof(copy)));
    }
    std::cout<<"Schedule: solar, polar, leap year, midnight, offsets, morning skip, overlap, night phase boundaries and validation passed.\n";
}
