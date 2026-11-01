#include "../src/light.h"
#include "../src/config.h"
#include "../src/protection.h"
#include "../src/pwm_sense.h"
#include <cassert>
#include <iostream>
#include "schedule_tests.h"
#include "../src/reliability.h"

static bool apply(lights::State &s, const char *json) {
    StaticJsonDocument<1024> doc;
    return !deserializeJson(doc, json) && !lights::parse(doc.as<JsonObjectConst>(), s);
}
static bool configure(Settings &s, const char *json) {
    StaticJsonDocument<1024> doc;
    return !deserializeJson(doc, json) && !parseSettings(doc.as<JsonObjectConst>(), s);
}
int main() {
    {
        using A=reliability::NetworkAction;reliability::NetworkRecovery n;
        assert(n.tick(29999,false,true)==A::None);
        assert(n.tick(30000,false,true)==A::Retry);
        assert(n.tick(120000,false,true)==A::Station);
        assert(n.tick(600000,false,true)==A::Radio);
        assert(n.tick(600001,false,true)==A::None);
        assert(n.tick(1200000,false,true,true)==A::None); // OTA never resets radio
        assert(n.tick(1200000,false,false)==A::None); // unconfigured AP remains usable
        assert(n.tick(1200000,true,true)==A::None);
        assert(n.tick(1200001,false,true)==A::None); // fresh grace after a connection
        const uint32_t at=UINT32_MAX-999;
        n.tick(at,true,true);
        assert(n.tick(at+29999,false,true)==A::None);
        assert(n.tick(at+30000,false,true)==A::Retry);
        assert(n.tick(at+120000,false,true)==A::Station);
        assert(n.tick(at+600000,false,true)==A::Radio);
        // A long router outage keeps retrying without rebooting the controller.
        unsigned radios=0;
        n={};for (uint32_t t=0;t<=3u*24*60*60*1000;t+=15000) if(n.tick(t,false,true)==A::Radio) ++radios;
        assert(radios==432);
        reliability::Resume r{};r.light.on=false;r.manual=true;r.until=1793228400;r.checksum=reliability::checksum(r);
        assert(reliability::valid(r));r.light.on=true;assert(!reliability::valid(r));
        reliability::Resume slots[2]{};assert(!reliability::latest(slots));
        slots[0].sequence=UINT32_MAX;slots[0].checksum=reliability::checksum(slots[0]);
        slots[1].sequence=0;slots[1].checksum=reliability::checksum(slots[1]);
        assert(reliability::latest(slots)==&slots[1]);
        slots[1].checksum^=1;assert(reliability::latest(slots)==&slots[0]);
        std::cout<<"PASS: network escalation, 3-day offline retry, OTA pause, timer wrap and RTC checkpoint integrity\n";
    }
    scheduleTests();
    {
        lights::State s;lights::Preset preset;
        StaticJsonDocument<256> doc;
        auto control=[&](const char *text,bool oneOff=true){assert(!deserializeJson(doc,text));return lights::control(doc.as<JsonObjectConst>(),s,preset,oneOff);};
        assert(!control("{\"brightness\":1}"));assert(!s.on && s.brightness==1);
        auto off=lights::output(s,0);assert(!off.a && !off.b);
        assert(!control("{\"state\":\"ON\"}"));assert(s.on && s.brightness==100 && s.mode==lights::Mode::All);
        preset={37,lights::Mode::Second};
        assert(!control("{\"brightness\":1}"));assert(!s.on);
        assert(!control("{\"brightness\":2}"));assert(s.on && s.brightness==2 && s.mode==lights::Mode::All);
        assert(!control("{\"state\":\"ON\"}"));assert(s.on && s.brightness==37 && s.mode==lights::Mode::Second);
        assert(!control("{\"state\":\"ON\",\"brightness\":12,\"effect\":\"first\"}"));assert(s.on && s.brightness==12 && s.mode==lights::Mode::First);
        assert(!control("{\"state\":\"ON\",\"brightness\":20}"));assert(s.on && s.brightness==20 && s.mode==lights::Mode::First);
        assert(!control("{\"state\":\"ON\",\"effect\":\"all\"}"));assert(s.on && s.brightness==20 && s.mode==lights::Mode::All);
        assert(!control("{\"state\":\"OFF\",\"brightness\":50}"));assert(!s.on && s.brightness==50);
        assert(!control("{\"brightness\":0}"));assert(!s.on && !s.brightness);
        assert(!control("{\"brightness\":1}",false));assert(s.on && s.brightness==1);
        auto saved=s;
        for (auto bad:{"{\"state\":\"ON\",\"brightness\":101}","{\"brightness\":2,\"wrong\":0}","{\"state\":true}"}) {
            assert(control(bad));assert(s.on==saved.on && s.brightness==saved.brightness && s.mode==saved.mode);
        }
        for (auto bad:{"{}","{\"brightness\":1}","{\"brightness\":101}","{\"brightness\":30,\"state\":\"ON\"}","{\"effect\":\"wrong\"}"}) {
            assert(!deserializeJson(doc,bad));assert(lights::parsePreset(doc.as<JsonObjectConst>(),preset));assert(preset.brightness==37 && preset.mode==lights::Mode::Second);
        }
        assert(!deserializeJson(doc,"{\"brightness\":70}"));assert(!lights::parsePreset(doc.as<JsonObjectConst>(),preset));assert(preset.brightness==70 && preset.mode==lights::Mode::Second);
        lights::WindowPresets windows;assert(lights::valid(windows));
        doc.clear();assert(!deserializeJson(doc,R"({"evening":{"brightness":73,"effect":"first"},"morning":{"brightness":21,"effect":"second"}})"));
        assert(!lights::parsePresets(doc.as<JsonObjectConst>(),windows));
        assert(windows.evening.brightness==73 && windows.evening.mode==lights::Mode::First && windows.morning.brightness==21 && windows.morning.mode==lights::Mode::Second);
        for (auto bad:{R"({})",R"({"night":{}})",R"({"morning":null})",R"({"morning":[]})",R"({"evening":{"brightness":14},"morning":{"effect":"bad"}})",R"({"evening":{"brightness":true}})",R"({"evening":{"brightness":101}})",R"({"morning":{"brightness":-1}})",R"({"evening":{"brightness":2.5}})",R"({"evening":{"brightness":14,"unknown":0}})"}) {
            doc.clear();assert(!deserializeJson(doc,bad));assert(lights::parsePresets(doc.as<JsonObjectConst>(),windows));
            assert(windows.evening.brightness==73 && windows.evening.mode==lights::Mode::First && windows.morning.brightness==21 && windows.morning.mode==lights::Mode::Second);
        }
        for (auto value:{0,1,2,100}) {
            doc.clear();doc["morning"]["brightness"]=value;assert(!lights::parsePresets(doc.as<JsonObjectConst>(),windows));
            assert(windows.morning.brightness==value && windows.morning.mode==lights::Mode::Second && windows.evening.brightness==73);
        }
        doc.clear();lights::json(doc.to<JsonObject>(),windows);lights::WindowPresets copy;
        assert(!lights::parsePresets(doc.as<JsonObjectConst>(),copy));assert(copy.evening.brightness==73 && copy.morning.mode==lights::Mode::Second);
        lights::Preset night{0};assert(lights::valid(night,0) && !lights::valid(night));
        for (auto value:{0,1,2,100}) {doc.clear();doc["brightness"]=value;assert(!lights::parsePreset(doc.as<JsonObjectConst>(),night,0));assert(night.brightness==value);}
        for (auto bad:{"{}","{\"brightness\":-1}","{\"brightness\":101}","{\"brightness\":true}","{\"effect\":\"bad\"}","{\"state\":\"OFF\"}"}) {
            assert(!deserializeJson(doc,bad));assert(lights::parsePreset(doc.as<JsonObjectConst>(),night,0));assert(night.brightness==100 && night.mode==lights::Mode::All);
        }
        std::cout<<"PASS: physical OFF at 0/optional 1 percent, slider auto ON, plain ON preset, explicit fields and atomic preset validation\n";
    }
    // Observe ordinary PWM without modifying the requested output. Synthetic
    // equal-mean waveforms must still distinguish short/open/healthy circuits.
    sensing::Model model;
    for (unsigned a : {0,1,3,5,50,250,500}) for (unsigned b : {0,1,3,5,50,250,500}) {
        if (!a && !b) continue;
        model.configure(a,b,120,2.856f);
        for (bool shorted : {false,true}) {
            sensing::Fit fit;
            for (unsigned i=0;i<1024;++i) {
                float xa=model.a[i]*(1620-238), xb=model.b[i]*(1620-238);
                float y=shorted ? model.a[i]*(80-238)+model.b[i]*(3160-238) : xa+xb;
                y+=(int(i*17%7)-3)*.25f;
                fit.add(xa,xb,y,false,i);
            }
            auto r=fit.result();
            if (!r.valid || fit.healthy(r,80)==shorted) return 30;
        }
    }
    // Regression: 80 mV raw ADC noise previously tripped the fixed 45 mV
    // residual limit, although the phase-averaged waveform was healthy.
    model.configure(25,25,120,2.845f);
    sensing::Fit noisy; uint32_t random=123456789;
    for (unsigned i=0;i<8192;++i) {
        random=random*1664525u+1013904223u; unsigned phase=(random>>16)&1023;
        float a=model.a[phase]*900,b=model.b[phase]*1750;
        random=random*1664525u+1013904223u;
        float noise=(int(random>>16)-32768)*(280.f/65536);
        noisy.add(a,b,a+b+noise,false,phase);
    }
    auto noiseResult=noisy.result();
    if (!noisy.healthy(noiseResult,80) || noiseResult.noise<60 || noiseResult.rms>20) return 37;
    protection::Limits limits;
    limits.checksum=protection::checksum(limits);
    if (!protection::valid(limits) || protection::maxIgnored(0)!=79 || protection::maxIgnored(2)!=39 ||
        protection::needsMonitoring(395,0) || !protection::needsMonitoring(396,0)) return 38;
    StaticJsonDocument<512> limitsDoc;
    deserializeJson(limitsDoc,"{\"first\":85,\"second\":15,\"all\":45}");
    if (protection::parseLimits(limitsDoc.as<JsonObjectConst>(),limits) ||
        protection::fadeIgnored(limits)!=90 || protection::shortPowerMw(450,0)<=2000) return 39;
    for (const char *bad : {"{}","{\"first\":101,\"second\":0,\"all\":0}",
        "{\"first\":-1,\"second\":0,\"all\":0}","{\"automatic\":false}",
        "{\"first\":1.5,\"second\":0,\"all\":0}","{\"first\":true,\"second\":0,\"all\":0}"}) {
        deserializeJson(limitsDoc,bad);
        if (!protection::parseLimits(limitsDoc.as<JsonObjectConst>(),limits) || limits.ignored[0]!=85) return 40;
    }
    deserializeJson(limitsDoc,"{\"second\":20}");
    assert(!protection::parseLimits(limitsDoc.as<JsonObjectConst>(),limits));
    assert(limits.ignored[0]==85 && limits.ignored[1]==20 && limits.ignored[2]==45);
    deserializeJson(limitsDoc,"{\"unknown\":1,\"first\":1}");
    assert(protection::parseLimits(limitsDoc.as<JsonObjectConst>(),limits) && limits.ignored[0]==85);
    limits.automatic[0]=4;limits.automatic[1]=2;limits.automatic[2]=3;
    deserializeJson(limitsDoc,"{\"automatic\":true}");
    if (protection::parseLimits(limitsDoc.as<JsonObjectConst>(),limits) ||
        limits.ignored[0]!=4 || protection::fadeIgnored(limits)!=6 || !protection::valid(limits)) return 41;
    std::cout << "PASS: noisy ADC phase averaging, thermal calculations, manual overrides above 2 W, calibrated limits and fade bounds\n";
    sensing::Calibration pwm; pwm.zero=238;
    for (auto &bank:pwm.references) for (auto &v:bank) v=1620;
    pwm.checksum=sensing::checksum(pwm);
    if (!sensing::valid(pwm) || pwm.reference(0,0)!=238 || pwm.reference(0,3)!=1620 || pwm.reference(1,499)!=1620) return 31;
    pwm.references[0][0]=1600; pwm.references[0][1]=1500;
    if (pwm.reference(0,6)!=1580 || pwm.reference(0,9)!=1520) return 34;
    pwm.references[0][0]=pwm.references[0][1]=1620;
    sensing::Wave wave; model.configure(500,0,120,pwm.ratio());
    for (unsigned i=0;i<1024;++i) wave.add(i,lroundf(238+model.a[(i+16)&1023]*1382));
    if (!sensing::identify(wave,pwm,model) || abs(int(pwm.tau)-120)>4 || abs(pwm.shift-16)>4) return 32;
    sensing::Fit blind, saturated;
    for (unsigned i=0;i<200;++i) { blind.add(0,0,0,false,i*5%1024); saturated.add(100,0,3000,true,i*5%1024); }
    if (blind.result().valid || saturated.result().valid) return 33;
    // Independent one-input circuit: integrate physical resistor currents into
    // a capacitor. The waveform is not generated by the observer under test.
    const double rg=3000,rt=3000,rp=rg*(3300./238-1),cap=120e-9,dt=1e-3/1024;
    const double goff=1/rg+1/rp,gon=goff+1/rt;
    for (double forwardV:{1.7,3.6,5.0}) for (double reverseV:{1.7,3.6,5.0}) {
        sensing::Calibration cal;cal.zero=238;cal.tau=lround(cap/gon/dt);cal.shift=0;
        const double forward=(forwardV/rt+3.3/rp)/gon*1000;
        const double reverse=((5-reverseV)/rt+3.3/rp)/gon*1000;
        for (auto &v:cal.references[0]) v=lround(forward);
        for (auto &v:cal.references[1]) v=lround(reverse);
        cal.checksum=sensing::checksum(cal);assert(sensing::valid(cal));
        assert(cal.threshold(0,500,80)<forward && cal.threshold(1,500,80)>reverse);
        for (unsigned a:{0,3,5,25,250,500}) for (unsigned b:{0,3,5,25,250,500}) {
            if (!a && !b) continue;
            model.configure(a,b,cap/gon/dt,gon/goff);
            for (unsigned fault:{0,1,2,3}) { // short either polarity or both
                if ((fault==1&&!a)||(fault==2&&!b)) continue;
                sensing::Fit circuit;double volts=.238;
                for (unsigned t=0;t<23*1024;++t) {
                    unsigned phase=t%1024;
                    if (t>=21*1024) {
                        const float mv=std::max(150.,std::round(volts*1000));
                        auto x=sensing::Basis(cal,a,b).observe(model,phase,mv);
                        circuit.add(x.a,x.b,x.y,false,phase);
                    }
                    const bool pa=phase<a,pb=phase>=512 && phase<512+b;
                    const double source=pa?((fault&1)?0:forwardV):pb?((fault&2)?5:5-reverseV):0;
                    const double current=3.3/rp+(pa||pb?source/rt:0)-volts*(pa||pb?gon:goff);
                    volts+=current*dt/cap;
                }
                const auto r=circuit.result();
                if (!fault) {assert(r.valid);assert(circuit.healthy(r,80));}
                // The runtime intentionally ignores a polarity with negligible
                // contribution at a fade endpoint; test all measurable faults.
                else if (fault==3 || (fault==1 && circuit.aa>=circuit.bb*.05) ||
                         (fault==2 && circuit.bb>=circuit.aa*.05)) assert(!circuit.healthy(r,80));
            }
        }
    }
    // An open string and a reverse plateau near the OFF level are valid.
    sensing::Calibration open;open.zero=238;
    for (auto &v:open.references[0]) v=lround(open.high());
    for (auto &v:open.references[1]) v=lround(open.low());
    open.checksum=sensing::checksum(open);assert(sensing::valid(open));
    for (auto &v:open.references[1]) v=open.zero;
    open.checksum=sensing::checksum(open);assert(sensing::valid(open));
    model.configure(500,500,open.tau,open.ratio());sensing::Fit nearZero,rawNearZero;
    for (unsigned i=0;i<4096;++i) {
        unsigned t=i%1024;
        float mv=open.zero+model.a[t]*(open.reference(0,500)-open.zero)+model.b[t]*(open.reference(1,500)-open.zero);
        auto x=sensing::Basis(open,500,500).observe(model,t,mv);
        nearZero.add(x.a,x.b,x.y,false,t);
        rawNearZero.add(model.a[t]*1000,model.b[t]*1000,mv-open.zero,false,t);
    }
    assert(nearZero.healthy(nearZero.result(),80));
    auto rawZero=rawNearZero.result();assert(fabsf(rawZero.b)<.001f);
    auto quality=sensing::precision(rawZero,open,open.reference(0,500),open.reference(1,500));
    assert(sensing::reliable(quality,true,true,.1f));
    open.magic=0x50574d31;open.checksum=sensing::checksum(open);assert(!sensing::valid(open));
    std::cout << "PASS: phase-resolved PWM at low duty, equal-mean short waveforms, both polarities, RC identification, blind/saturated rejection\n";
    // Safety boundary tests use return codes instead of an assertion dialog.
    protection::Config protectionConfig;
    protectionConfig.referenceMv = 1200; protectionConfig.zeroMv = 140; protectionConfig.reverseMv = 2100;
    protectionConfig.calibrated = true;
    protectionConfig.checksum = protection::checksum(protectionConfig);
    if (!protection::valid(protectionConfig)) return 10;
    auto corrupt = protectionConfig; corrupt.profile[1][17] = 123;
    if (protection::valid(corrupt)) return 12;
    auto noLoad = protectionConfig; noLoad.reverseMv = noLoad.referenceMv + 2;
    noLoad.checksum = protection::checksum(noLoad);
    if (!protection::valid(noLoad)) return 18;
    auto weakString = protectionConfig; weakString.referenceMv = weakString.zeroMv + 100;
    weakString.reverseMv = weakString.referenceMv; weakString.checksum = protection::checksum(weakString);
    if (!protection::valid(weakString)) return 19;
    --weakString.referenceMv; weakString.checksum = protection::checksum(weakString);
    if (protection::valid(weakString)) return 20;
    if (protectionConfig.latchFault || protection::restartBlocked(protectionConfig, 1)) return 21;
    auto latched = protectionConfig; latched.latchFault = 1;
    for (uint8_t fault = 1; fault <= 5; ++fault)
        if (!protection::restartBlocked(latched, fault) || protection::restartBlocked(protectionConfig, fault)) return 22;
    if (protection::restartBlocked(latched, 0)) return 23;
    // 0.3.1 stored arbitrary padding at the new policy byte. Verify old checksum
    // before migrating; preserve every calibration value, default to just OFF.
    auto legacy = protectionConfig; legacy.magic = protection::LegacyMagic; legacy.latchFault = 0xa5;
    legacy.profile[2][99] = 1317; legacy.checksum = protection::checksum(legacy);
    auto badLegacy = legacy; ++badLegacy.referenceMv;
    if (protection::load(badLegacy)) return 24;
    if (!protection::load(legacy) || legacy.latchFault || legacy.profile[2][99] != 1317 ||
        legacy.referenceMv != protectionConfig.referenceMv || !protection::valid(legacy)) return 25;
    auto legacyLatch=protectionConfig; legacyLatch.magic=protection::LatchingMagic; legacyLatch.latchFault=1;
    legacyLatch.checksum=protection::checksum(legacyLatch);
    if (!protection::load(legacyLatch) || !legacyLatch.latchFault || legacyLatch.calibrated) return 36;
    auto badPolicy = protectionConfig; badPolicy.latchFault = 2; badPolicy.checksum = protection::checksum(badPolicy);
    if (protection::load(badPolicy)) return 26;
    StaticJsonDocument<512> protectionDoc;
    for (const char *bad : {"{}", "{\"enabled\":1}", "{\"delay_ms\":99}", "{\"delay_ms\":10001}",
        "{\"delay_ms\":500.5}", "{\"threshold_percent\":29}", "{\"threshold_percent\":96}",
        "{\"enabled\":false,\"typo\":1}", "{\"latch_fault\":1}", "{\"latch_fault\":\"true\"}",
        "{\"latch_fault\":null}", "{\"latch_fault\":true,\"delay_ms\":99}"}) {
        deserializeJson(protectionDoc, bad);
        if (!protection::parse(protectionDoc.as<JsonObjectConst>(), protectionConfig) ||
            !protectionConfig.enabled || protectionConfig.latchFault || protectionConfig.delayMs != 500) return 13;
    }
    for (bool value : {true, false}) {
        protectionDoc.clear(); protectionDoc["latch_fault"] = value;
        if (protection::parse(protectionDoc.as<JsonObjectConst>(), protectionConfig) ||
            protectionConfig.latchFault != uint8_t(value) || !protection::valid(protectionConfig)) return 27;
    }
    for (uint16_t wait : {100, 500, 5000, 10000}) {
        protection::Timer timer;
        if (timer.observe(false, 100, wait) || timer.observe(false, 100 + wait - 1, wait) ||
            !timer.observe(false, 100 + wait, wait)) return 14;
        timer.observe(true, 20000, wait);
        if (timer.pending || timer.observe(false, 20001, wait)) return 15;
        timer.clear();
        if (timer.observe(false, UINT32_MAX - 49, wait) ||
            !timer.observe(false, wait - 50, wait)) return 16;
    }
    // DC averages hide a short; phase-resolved measurements distinguish it.
    const double normalMean = (3.6/2+1.4/2)/2;
    const double shortMean = (0.0/2+5.0/2)/2;
    if (abs(normalMean - shortMean) > 0.001) return 17;
    std::cout << "PASS: protection threshold/margin, low/high ADC faults, calibration checksum/migration, optional fault latch, atomic validation, delay limits and clock wrap\n";
    char alias[33] = "ledpasek01";
    StaticJsonDocument<256> aliasDoc;
    for (const char *bad : {"{}", "{\"alias\":\"\"}", "{\"alias\":null}", "{\"alias\":42}",
        "{\"alias\":\"Test\"}", "{\"alias\":\"test.local\"}", "{\"alias\":\"-test\"}", "{\"alias\":\"test-\"}",
        "{\"alias\":\"a/b\"}", "{\"alias\":\"a+b\"}", "{\"alias\":\"a#b\"}", "{\"alias\":\"a_b\"}",
        "{\"alias\":\"a b\"}", "{\"alias\":\"a\\u0000b\"}",
        "{\"alias\":\"123456789012345678901234567890123\"}", "{\"alias\":\"test\",\"extra\":1}"}) {
        assert(!deserializeJson(aliasDoc, bad));
        assert(parseAlias(aliasDoc.as<JsonObjectConst>(), alias));
        assert(!strcmp(alias, "ledpasek01"));
    }
    for (const char *good : {"x", "ledpasek01", "led-pasek-02", "12345678901234567890123456789012"}) {
        aliasDoc.clear(); aliasDoc["alias"] = good;
        const char *error = parseAlias(aliasDoc.as<JsonObjectConst>(), alias);
        if (error || strcmp(alias, good)) { std::cerr << "Alias check failed: " << good << " -> " << (error ? error : alias) << "\n"; return 1; }
    }
    static_assert(sizeof(Settings) == 368, "Alias must not change the existing saved-network layout");
    lights::State s;
    static_assert(sizeof(lights::State) == 6, "Keep the 0.1.0 saved-state layout");
    static_assert(unsigned(lights::Mode::Crossfade)==4 && sizeof(lights::Preset)==2,"Existing NVS mode values/layouts must remain unchanged");
    assert(s.on && s.mode == lights::Mode::All && s.brightness == 100);
    for (unsigned m=0;m<lights::ModeCount;++m) {
        auto mode=lights::Mode(m);StaticJsonDocument<256> doc;
        doc["effect"]=lights::modeLabel(mode);assert(!lights::parse(doc.as<JsonObjectConst>(),s) && s.mode==mode);
        doc["effect"]=lights::modeName(mode);assert(!lights::parse(doc.as<JsonObjectConst>(),s) && s.mode==mode);
        lights::Preset p;assert(!lights::parsePreset(doc.as<JsonObjectConst>(),p) && p.mode==mode);
        for (unsigned n=m+1;n<lights::ModeCount;++n) assert(strcmp(lights::modeLabel(mode),lights::modeLabel(lights::Mode(n))));
    }
    for (auto mode:{lights::Mode::SoftCrossfade,lights::Mode::Breathe,lights::Mode::Tide,lights::Mode::Candlelight}) {
        s.mode=mode;s.on=true;
        for (unsigned interval:{100,500,1000,10000}) for (unsigned brightness:{0,1,2,8,20,100}) {
            s.intervalMs=interval;s.brightness=brightness;unsigned duty=5*brightness,cycle=lights::cycleMs(s);
            auto first=lights::output(s,0),previous=first;bool changes=false;
            for (unsigned t=1;t<=cycle;t+=5) {
                auto out=lights::output(s,t);assert(out.a<=duty && out.b<=duty);
                // At the fastest allowed tempo, no one-step flash or jump at cycle wrap.
                assert(std::abs(int(out.a)-int(previous.a))<=9 && std::abs(int(out.b)-int(previous.b))<=9);
                if (mode==lights::Mode::SoftCrossfade) assert(out.a+out.b==duty);
                if (mode==lights::Mode::Tide) assert(out.a+out.b==duty+(duty+3)/4 && out.a>=(duty+3)/4 && out.b>=(duty+3)/4);
                if (mode==lights::Mode::Breathe) assert(out.a==out.b && out.a>=duty/5);
                if (mode==lights::Mode::Candlelight) assert(out.a>=duty/2 && out.b>=duty/2);
                changes|=out.a!=first.a || out.b!=first.b;previous=out;
            }
            if (brightness>=8) assert(changes);
            auto end=lights::output(s,cycle);assert(end.a==first.a && end.b==first.b);
            uint32_t start=UINT32_MAX-749;assert(lights::phaseTime(0,start,s)==750);
            assert(lights::phaseTime(cycle-750,start,s)==0);
            s.on=false;auto off=lights::output(s,cycle/3);assert(!off.a && !off.b);s.on=true;
        }
    }
    s=lights::State{};s.mode=lights::Mode::SoftCrossfade;
    assert(lights::output(s,1000).a==250 && lights::output(s,2000).a==0);
    s.mode=lights::Mode::Breathe;assert(lights::output(s,3000).a==100);
    s.mode=lights::Mode::Tide;assert(lights::output(s,4000).a==125);
    std::cout<<"PASS: bilingual modes/legacy codes, smooth bounded cycles, floors, conserved crossfade/tide light, true OFF and timer wrap\n";
    s=lights::State{};
    // Every brightness/mode has disjoint phases, bounded per-bank duty, and true OFF.
    for (unsigned mode = 0; mode < lights::ModeCount; ++mode) for (unsigned brightness = 0; brightness <= 100; ++brightness) {
        s.mode = lights::Mode(mode); s.brightness = brightness;
        for (uint32_t phase : {0u, 1u, 250u, 500u, 999u, 1000u, 1500u, 1999u, 2000u}) {
            s.on = true; auto out = lights::output(s, phase);
            assert(out.a <= 500 && out.b <= 500);
            for (unsigned tick = 0; tick < lights::Period; ++tick) {
                bool a = tick < out.a;
                bool b = tick >= lights::Half && tick < lights::Half + out.b;
                assert(!(a && b));
            }
            if (mode == 0) assert(out.a == out.b);
            if (mode == 1) assert(out.b == 0);
            if (mode == 2) assert(out.a == 0);
            if (mode == 3) assert(phase % 2000 >= 1000 ? out.a == 0 : out.b == 0);
            if (mode == 4) assert(out.a + out.b == 500 * brightness / 100);
            s.on = false; out = lights::output(s, phase); assert(!out.a && !out.b);
        }
    }
    s = lights::State{}; s.mode = lights::Mode::Crossfade;
    for (uint16_t interval : {100, 137, 1000, 10000}) for (uint8_t brightness : {0, 1, 17, 100}) {
        s.intervalMs = interval; s.brightness = brightness;
        auto start = lights::output(s, 0), middle = lights::output(s, interval), end = lights::output(s, 2u * interval);
        assert(start.a == 500 * brightness / 100 && start.b == 0);
        assert(middle.a == 0 && middle.b == start.a);
        assert(end.a == start.a && end.b == start.b);
        auto previous = start;
        for (unsigned t = 1; t <= 2u * interval; ++t) {
            auto out = lights::output(s, t);
            assert(out.a + out.b == start.a);
            if (t <= interval) assert(out.a <= previous.a && out.b >= previous.b);
            else assert(out.a >= previous.a && out.b <= previous.b);
            previous = out;
        }
    }
    s = lights::State{}; s.mode = lights::Mode::Crossfade;
    auto midpoint = lights::output(s, 500);
    assert(midpoint.a == 250 && midpoint.b == 250);
    uint32_t effectStart = UINT32_MAX - 749;
    assert(lights::phaseTime(0, effectStart, 1000) == 750);
    assert(lights::phaseTime(1250, effectStart, 1000) == 0 && effectStart == 1250);
    assert(lights::phaseTime(18750, effectStart, 1000) == 1500 && effectStart == 17250);
    assert(lights::phaseTime(19250, effectStart, 1000) == 0 && effectStart == 19250);
    s = lights::State{};
    assert(apply(s, "{\"state\":\"OFF\",\"brightness\":17,\"effect\":\"alternate\",\"interval_ms\":250}"));
    assert(!s.on && s.brightness == 17 && s.mode == lights::Mode::Alternate && s.intervalMs == 250);
    for (const char *bad : {"{}", "[]", "{\"state\":true}", "{\"brightness\":-1}", "{\"brightness\":101}",
        "{\"brightness\":2.5}", "{\"brightness\":null}", "{\"effect\":\"unknown\"}",
        "{\"interval_ms\":99}", "{\"interval_ms\":10001}", "{\"state\":\"ON\",\"brightness\":999}",
        "{\"state\":\"ON\",\"typo\":1}", "{\"effect\":\"all\\u0000bad\"}", "{\"state\":\"ON\\u0000bad\"}"}) {
        assert(!apply(s, bad)); assert(!s.on && s.brightness == 17 && s.mode == lights::Mode::Alternate);
    }
    assert(apply(s, "{\"state\":\"ON\"}")); assert(s.on && s.brightness == 17);
    assert(apply(s, "{\"effect\":\"crossfade\",\"interval_ms\":2350}"));
    assert(s.mode == lights::Mode::Crossfade && s.intervalMs == 2350 && s.brightness == 17);
    assert(apply(s, "{\"effect\":\"alternate\"}")); assert(s.intervalMs == 2350);
    assert(apply(s, "{\"interval_ms\":100}")); assert(apply(s, "{\"interval_ms\":10000}"));
    Settings config;
    assert(configure(config,"{\"ssid\":\"Test\",\"wifi_password\":\"password123\",\"mqtt_host\":\"192.0.2.234\",\"mqtt_password\":\"secret\"}"));
    assert(configure(config,"{\"wifi_password\":\"\",\"mqtt_password\":\"\"}"));
    assert(!strcmp(config.wifiPassword,"password123") && !strcmp(config.mqttPassword,"secret"));
    for (const char *bad : {"{\"mqtt_port\":0}","{\"mqtt_port\":65536}","{\"mqtt_enabled\":1}",
        "{\"mqtt_host\":\"http://broker\"}","{\"wifi_password\":\"short\"}",
        "{\"mqtt_host\":\"broker:1883\"}","{\"ssid\":\"new\",\"mqtt_port\":-1}"}) {
        assert(!configure(config,bad)); assert(!strcmp(config.ssid,"Test"));
    }
    assert(configure(config,"{\"clear_mqtt_password\":true,\"clear_wifi_password\":true}"));
    assert(!*config.mqttPassword && !*config.wifiPassword);
    std::cout << "PASS: alias validation/atomic rejection, saved-network layout, 8181 PWM combinations, crossfade/phase-clock wrap, commands and settings\n";
}
