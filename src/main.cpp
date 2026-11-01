#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <driver/ledc.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include <esp_adc_cal.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <esp_netif.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <soc/ledc_struct.h>
#include <atomic>
#include "light.h"
#include "config.h"
#include "protection.h"
#include "pwm_sense.h"
#include "schedule.h"
#include "reliability.h"
#include "pwm_driver.h"
#if __has_include("bootstrap.h")
#include "bootstrap.h"
#endif
#ifndef CONFIG_IDF_TARGET_ESP32
#error This hardware profile is for the original ESP32, not C3/S3.
#endif

#ifdef RELIABILITY_TEST
constexpr char Version[] = "0.5.2-test";
#else
constexpr char Version[] = "0.5.2";
#endif
#if defined(ARDUINO_LILYGO_T_DISPLAY)
constexpr char BoardId[] = "lilygo-t-display";
#elif defined(ARDUINO_ESP32_DEV)
constexpr char BoardId[] = "esp32-wroom-32";
#else
#error Select the tdisplay or esp32wroom32 PlatformIO environment.
#endif
// Both original ESP32 boards use the same DRV8833 IN/IN pins.
static Settings settings;
static lights::State light;
static lights::Preset onPreset;
static lights::Preset nightPreset{0};
static lights::WindowPresets windowPresets;
static schedule::Config scheduleConfig;
static schedule::Plan schedulePlan;
static std::atomic<time_t> lastTimeSync{0};
static bool manualOverride=false;
static time_t overrideUntil=0;
static void scheduleTick(bool force=false);
static void manualScheduleOverride();
static Preferences prefs;
static SemaphoreHandle_t mutex;
struct Guard { Guard() { xSemaphoreTake(mutex, portMAX_DELAY); } ~Guard() { xSemaphoreGive(mutex); } };
static String deviceId, hostname, baseTopic, apName;
static std::atomic<bool> mqttOnline{false}, statePending{true}, updating{false};
static std::atomic<int> mqttError{-1};
static std::atomic<bool> configPending{true}, calibrationPending{true};
static std::atomic<uint32_t> rebootAt{0};
static bool apActive = false, dirty = false;
static uint32_t changedAt = 0, effectAt = 0;
static lights::Output previousOutput{UINT32_MAX, UINT32_MAX};
static bool storageError = false;
static protection::Config protectionConfig;
static protection::Limits protectionLimits;
static protection::Timer faultTimer;
static uint8_t faultCode = 0;
static sensing::Calibration pwmCalibration;
static sensing::Model pwmModel;
static sensing::Fit measurement;
static sensing::Result measured;
static lights::Output modelOutput{UINT32_MAX, UINT32_MAX};
static uint32_t windowAt = 0;
static uint16_t probeMv = 0, probeEarlyMv = 0;
static uint32_t probeAt = 0;
static esp_adc_cal_characteristics_t adcCalibration;
static std::atomic<uint32_t> controlHeartbeat{0};
static std::atomic<bool> monitorFailed{false}, monitorArmed{false};
static std::atomic<uint32_t> loopHeartbeat{0}, networkHeartbeat{0}, mqttHeartbeat{0}, httpHeartbeat{0};
static std::atomic<uint32_t> disconnects{0}, lastDisconnectAt{0}, reconnects{0}, stationResets{0}, radioResets{0};
static std::atomic<uint32_t> fallbackStarts{0};
static std::atomic<unsigned> disconnectReason{0};
static std::atomic<bool> httpProbePending{false};
static httpd_handle_t httpServer=nullptr;
static RTC_NOINIT_ATTR reliability::Resume resumeSlots[2];
static uint32_t resumeSequence=0;
static bool resumeRestored=false;
static RTC_NOINIT_ATTR uint32_t recoveryMarker;
static uint32_t bootCount=0, lastRecovery=0;
enum Recovery : uint32_t { ControlRecovery=0x52454301, HttpRecovery=0x52454302 };
static uint32_t age(const std::atomic<uint32_t> &stamp) {
    const uint32_t at=stamp.load();return uint32_t(millis()-at);
}
#ifdef RELIABILITY_TEST
static std::atomic<int> testStall{0};
static std::atomic<uint32_t> testOfflineUntil{0};
static std::atomic<int> testNetworkReset{0};
static void injectStall(int task) {
    if (testStall==task) vTaskDelay(pdMS_TO_TICKS(75000));
}
#endif
static void checkpoint() {
    reliability::Resume next{};next.sequence=++resumeSequence;next.light=light;next.manual=manualOverride;next.until=overrideUntil;
    next.checksum=reliability::checksum(next);
    // Keep the previous complete checkpoint if a watchdog resets mid-copy.
    memcpy(&resumeSlots[next.sequence%2],&next,sizeof(next));
}
static void recover(uint32_t reason) {
    // Never take the shared lock or write NVS on a possibly deadlocked system.
    pwm::emergencyStop();recoveryMarker=reason;ESP.restart();
}
static const char *resetName() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:return "power_on";
        case ESP_RST_SW:return "software";
        case ESP_RST_PANIC:return "panic";
        case ESP_RST_INT_WDT:return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:return "task_watchdog";
        case ESP_RST_WDT:return "watchdog";
        case ESP_RST_BROWNOUT:return "brownout";
        default:return "other";
    }
}
static struct {
    bool active = false;
    int step = -2;
    uint32_t nextAt = 0, sampleAt = 0;
    sensing::Fit fit, repeat;
    sensing::Wave wave;
    protection::Limits limits;
    lights::State restore;
    protection::Config result;
    sensing::Calibration pwm;
    lights::Output output{0, 0};
    const char *message = "Nejdřív spusť automatickou kalibraci.";
} calibration;
static const char *faultName() {
    switch (faultCode) {
        case 1: return "Pokles napětí / podezření na zkrat";
        case 2: return "Neplatný nebo nestabilní signál snímače";
        case 3: return "Výpadek měřicí úlohy";
        case 4: return "Kalibrace selhala";
        case 5: return "Chyba uložení ochrany";
        default: return "";
    }
}
static bool protectionBlocks() {
    return faultCode || monitorFailed || (protectionConfig.enabled &&
        !protectionConfig.calibrated);
}
static bool saveLight();

static void syncOutput() {
    // Copy a small command under the application lock. The output task never
    // takes that lock, including while HTTP, NVS or ADC is busy.
    if ((faultCode && !calibration.active) || monitorFailed || updating || rebootAt ||
        (!calibration.active && protectionBlocks())) {pwm::stop();return;}
    lights::phaseTime(millis(),effectAt,light); // Keep the effect epoch safe across millis wrap.
    pwm::Request next;next.light=light;next.epoch=effectAt;
    next.fixed=calibration.active;next.output=calibration.output;
    pwm::submit(next);
}
static void trip(uint8_t code) {
    faultCode = code; calibration.active = false; calibration.output = {0, 0};
    light.on = false; pwm::stop();
    // OFF and the diagnostic event survive reboot. Ordinary ON acknowledges
    // this event only in the default non-latching mode; never auto-retry.
    const bool latchSaved = prefs.putUChar("fault", code) == 1;
    const bool lightSaved = saveLight();
    storageError = !latchSaved || !lightSaved || code == 5;
    statePending = true;
}
static bool saveProtection(protection::Config &next) {
    next.checksum = protection::checksum(next);
    if (prefs.putBytes("protection", &next, sizeof(next)) != sizeof(next)) {
        storageError = true; trip(5); return false;
    }
    protectionConfig = next; return true;
}
static uint32_t calibrationSignature(const protection::Config &c) {
    uint32_t h=2166136261u;
    for (uint16_t v : {c.referenceMv, c.reverseMv, c.zeroMv}) h=(h^v)*16777619u;
    for (auto &bank : c.profile) for (uint16_t v : bank) h=(h^v)*16777619u;
    return h;
}
static void configureModel(lights::Output out, const sensing::Calibration &cal) {
    pwmModel.configure(out.a, out.b, cal.tau, cal.ratio()); modelOutput=out;
}
static void acquire(sensing::Fit &fit, const sensing::Calibration &cal, lights::Output out,
                    unsigned durationUs, bool calibratedRefs, sensing::Wave *wave=nullptr, sensing::Fit *repeat=nullptr) {
    // Pure observation: LEDC remains free running throughout every conversion.
    const auto frame=pwm::read();
    if (!pwm::same(frame.output,out) || !pwm::stable(frame) ||
        uint32_t(esp_timer_get_time()-frame.changedUs)<2000) return;
    const float scaleA=calibratedRefs ? cal.reference(0,out.a)-cal.zero : 1000;
    const float scaleB=calibratedRefs ? cal.reference(1,out.b)-cal.zero : 1000;
    const int64_t stop=esp_timer_get_time()+durationUs;
    while (esp_timer_get_time()<stop) {
        unsigned begin=LEDC.timer_group[0].timer[0].value.timer_cnt;
        int64_t at=esp_timer_get_time();
        unsigned raw=adc1_get_raw(ADC1_CHANNEL_4);
        unsigned end=LEDC.timer_group[0].timer[0].value.timer_cnt;
        // A fade may advance independently of the ADC task. Never fit a sample
        // against a duty/RC model from before that transition.
        if (!pwm::stable(frame)) break;
        if (esp_timer_get_time()-at>50) continue; // Preempted sample has unknown phase.
        unsigned phase=(begin+((end-begin)&1023)/2)&1023;
        unsigned mv=esp_adc_cal_raw_to_voltage(raw,&adcCalibration);
        if (wave) wave->add(phase,mv);
        unsigned t=(phase+cal.shift)&1023;
        const float a=pwmModel.a[t]*scaleA, b=pwmModel.b[t]*scaleB, y=int(mv)-cal.zero;
        const bool clip=raw==4095 || mv>2450;
        fit.add(a,b,y,clip,phase);
        if (repeat) repeat->add(a,b,y,clip,phase);
        delayMicroseconds(esp_random()%13); // Avoid rational sample-clock aliasing.
    }
}
static void resetMeasurement() {
    measurement={}; measured={}; faultTimer.clear(); windowAt=millis();
    modelOutput={UINT32_MAX,UINT32_MAX};
}
static void cancelCalibration(const char *message) {
    calibration.active=false; calibration.output={0,0}; calibration.message=message;
    light.on=false; pwm::stop(); saveLight(); resetMeasurement(); statePending=true;
}
static void prepareCalibrationPoint() {
    calibration.nextAt=millis()+80; calibration.sampleAt=0;
    calibration.fit={}; calibration.repeat={}; calibration.wave={};
}
static void calibrationTick(uint32_t now) {
    if (int32_t(now-calibration.nextAt)<0) return;
    if (!calibration.sampleAt) {
        calibration.sampleAt=now;
        configureModel(calibration.output,calibration.pwm);
    }
    // Short batches keep HTTP/OTA and the independent watchdog responsive.
    unsigned duration=calibration.step==-2 ? 500 :
        (calibration.output.a+calibration.output.b<25 ? 1000 : 300);
    acquire(calibration.fit,calibration.pwm,calibration.output,8000,false,
            calibration.step<0 ? &calibration.wave : nullptr,
            uint32_t(now-calibration.sampleAt)>=duration/2 ? &calibration.repeat : nullptr);
    if (uint32_t(millis()-calibration.sampleAt)<duration) return;
    if (calibration.step==-2) {
        uint32_t sum=0, count=0;
        for (unsigned i=0;i<sensing::Bins;++i) { sum+=calibration.wave.sum[i]; count+=calibration.wave.count[i]; }
        calibration.pwm.zero=count ? (sum+count/2)/count : 0;
        calibration.result.zeroMv=calibration.pwm.zero;
        if (calibration.pwm.zero<160 || calibration.pwm.zero>600) {
            calibration.message="Snímací vstup nemá platnou klidovou úroveň. Ověř GPIO32 a GND.";
            trip(4); return;
        }
        calibration.output={500,0}; calibration.step=-1; prepareCalibrationPoint();
        calibration.message="Měřím odezvu kondenzátoru při běžném PWM první poloviny.";
        return;
    }
    if (calibration.step==-1) {
        if (!sensing::identify(calibration.wave,calibration.pwm,pwmModel)) {
            calibration.message="Průběh PWM nelze spolehlivě změřit. Kalibrace se neuložila.";
            trip(4); return;
        }
        calibration.step=0; calibration.output={5,0}; prepareCalibrationPoint();
        calibration.message="Měřím normální PWM: první, druhá a obě poloviny, jas 1–100 %.";
        return;
    }
    int step=calibration.step;
    unsigned bank=step<300 ? step/100 : (step-300)/4;
    unsigned index=step<300 ? step%100 : 100+(step-300)%4;
    auto result=calibration.fit.result();
    measured=result; probeAt=millis();
    float va=calibration.pwm.zero+result.a*1000, vb=calibration.pwm.zero+result.b*1000;
    auto repeated=calibration.repeat.result();
    const float reserve=(100-calibration.result.thresholdPercent)/200.f;
    // Judge precision at the runtime window size, not just the long calibration
    // average, and compare an independently timed half of the capture for drift.
    auto precision=result;
    float runtimeScale=sqrtf(std::max(1.f,result.samples/1000.f));
    precision.errorA*=runtimeScale; precision.errorB*=runtimeScale;
    bool okay=sensing::reliable(precision,bank!=1,bank!=0,reserve);
    if (bank!=1) okay=okay && va>=calibration.pwm.zero+100 && va<=2450 &&
        repeated.valid && fabsf(repeated.a-result.a)<=fabsf(result.a)*reserve;
    if (bank!=0) okay=okay && vb>=calibration.pwm.zero+100 && vb<=3000 &&
        repeated.valid && fabsf(repeated.b-result.b)<=fabsf(result.b)*reserve;
    if (bank<2) {
        // Low-SNR values are not trusted by protection below the resulting cutoff.
        float v=bank ? vb : va;
        calibration.pwm.references[bank][index]=std::isfinite(v) ?
            lroundf(std::max(float(calibration.pwm.zero+100),std::min(2450.f,v))) : calibration.pwm.zero+100;
        if (index<100) calibration.result.profile[bank][index]=calibration.pwm.references[bank][index];
    } else {
        float ra=calibration.pwm.references[0][index]-calibration.pwm.zero;
        float rb=calibration.pwm.references[1][index]-calibration.pwm.zero;
        okay=okay && fabsf(result.a*1000/ra-1)<reserve && fabsf(result.b*1000/rb-1)<reserve;
        calibration.result.profile[2][index]=lroundf(std::max(0.f,std::min(3000.f,(va+vb)/2)));
    }
    if (index<100) {
        calibration.limits.noise[bank]=std::max(calibration.limits.noise[bank],uint16_t(std::min(65535.f,result.noise)));
        calibration.limits.error[bank]=std::max(calibration.limits.error[bank],uint16_t(std::min(65535.f,result.rms)));
        if (!okay) calibration.limits.automatic[bank]=index+1;
        if (!okay && index==99) {
            calibration.message="Ani při plném jasu není měření spolehlivé. Předchozí kalibrace zůstala uložená.";
            trip(4); return;
        }
    }
    if (++calibration.step==308) {
        calibration.result.referenceMv=calibration.pwm.references[0][99];
        calibration.result.reverseMv=calibration.pwm.references[1][99];
        calibration.result.calibrated=true; calibration.result.enabled=true;
        calibration.pwm.signature=calibrationSignature(calibration.result);
        calibration.pwm.checksum=sensing::checksum(calibration.pwm);
        if (!sensing::valid(calibration.pwm)) { trip(4); return; }
        calibration.limits.signature=calibration.pwm.signature;
        memcpy(calibration.limits.ignored,calibration.limits.automatic,sizeof(calibration.limits.ignored));
        calibration.limits.checksum=protection::checksum(calibration.limits);
        if (prefs.putBytes("pwmcal",&calibration.pwm,sizeof(calibration.pwm))!=sizeof(calibration.pwm) ||
            prefs.putBytes("pwmlimits",&calibration.limits,sizeof(calibration.limits))!=sizeof(calibration.limits)) { trip(5); return; }
        if (!saveProtection(calibration.result)) return;
        if (prefs.putUChar("fault",0)!=1) { trip(5); return; }
        pwmCalibration=calibration.pwm; protectionLimits=calibration.limits; faultCode=0; calibration.active=false;
        calibration.message="Kalibrace dokončena. Ochrana měří běžné PWM bez testovacích záblesků.";
        light=calibration.restore; resetMeasurement();
        if (!saveLight()) { trip(5); return; }
        effectAt=millis(); statePending=true; configPending=true; calibrationPending=true; return;
    }
    step=calibration.step;
    bank=step<300 ? step/100 : (step-300)/4;
    unsigned duty=step<300 ? (step%100+1)*5 : (step-300)%4+1;
    calibration.output={bank==1 ? 0 : duty, bank==0 ? 0 : duty};
    prepareCalibrationPoint();
}
static unsigned ignoredBrightness() {
    switch (light.mode) {
        case lights::Mode::All:
        case lights::Mode::Tide:
        case lights::Mode::Breathe:
        case lights::Mode::Candlelight: return protectionLimits.ignored[2];
        case lights::Mode::First: return protectionLimits.ignored[0];
        case lights::Mode::Second: return protectionLimits.ignored[1];
        case lights::Mode::Alternate: return std::max(protectionLimits.ignored[0],protectionLimits.ignored[1]);
        case lights::Mode::Crossfade:
        case lights::Mode::SoftCrossfade: return protection::fadeIgnored(protectionLimits);
        default: return 0;
    }
}
static bool measurementRequired() {
    // Breathing/candle energy changes within a cycle. Use actual combined PWM,
    // never skip the bright part just because the low part is below the ADC cutoff.
    return light.on && (lights::varyingPower(light.mode) || light.mode==lights::Mode::Tide ?
        previousOutput.a+previousOutput.b>10u*ignoredBrightness() : light.brightness>ignoredBrightness());
}
static void protectionTick(uint32_t now) {
    // A stalled task is a recoverable software failure, not a latched electrical
    // fault. Keep the bridge off until the supervisor restarts the controller.
    if (monitorFailed) return;
    if (updating || rebootAt || (faultCode && !calibration.active)) { measurement={}; faultTimer.clear(); return; }
    if (calibration.active) { calibrationTick(now); return; }
    if (!protectionConfig.enabled || !protectionConfig.calibrated || !light.on || !light.brightness) {
        resetMeasurement(); return;
    }
    // The calibrated/user-selected cutoff applies to all signal faults, including
    // a real short. Its worst-case thermal power is exposed prominently in the UI.
    if (!measurementRequired()) {
        if (lights::varyingPower(light.mode) && light.brightness>ignoredBrightness()) {
            // A dim phase is not a healthy probe. Preserve a pending fault until
            // the next measurable phase; only a healthy result can clear it.
            measurement={};windowAt=now;
        } else resetMeasurement();
        return;
    }
    if (previousOutput.a!=modelOutput.a || previousOutput.b!=modelOutput.b) configureModel(previousOutput,pwmCalibration);
    acquire(measurement,pwmCalibration,previousOutput,3000,true);
    if (uint32_t(millis()-windowAt)<200) return;
    measured=measurement.result(); probeAt=millis();
    bool good=measurement.healthy(measured,protectionConfig.thresholdPercent);
    probeMv=std::max(0L,std::min(4000L,lroundf(pwmCalibration.zero+measured.a*(pwmCalibration.reference(0,previousOutput.a)-pwmCalibration.zero))));
    probeEarlyMv=std::max(0L,std::min(4000L,lroundf(pwmCalibration.zero+measured.b*(pwmCalibration.reference(1,previousOutput.b)-pwmCalibration.zero))));
    if (faultTimer.observe(good,probeAt,protectionConfig.delayMs)) trip(measured.valid ? 1 : 2);
    measurement={}; windowAt=millis();
}
static void protectionJson(JsonObject obj) {
    obj["enabled"] = protectionConfig.enabled; obj["calibrated"] = protectionConfig.calibrated;
    obj["fault"] = faultCode; obj["reason"] = faultName();
    obj["latch_fault"] = bool(protectionConfig.latchFault);
    obj["latched"] = protection::restartBlocked(protectionConfig, faultCode);
    obj["blocked"] = protection::restartBlocked(protectionConfig, faultCode) || monitorFailed || (protectionConfig.enabled &&
        !protectionConfig.calibrated);
    obj["delay_ms"] = protectionConfig.delayMs; obj["threshold_percent"] = protectionConfig.thresholdPercent;
    obj["threshold_mv"] = protectionConfig.calibrated ? protection::threshold(protectionConfig) : 0;
    obj["reference_mv"] = protectionConfig.referenceMv; obj["zero_mv"] = protectionConfig.zeroMv;
    obj["reverse_mv"] = protectionConfig.reverseMv;
    obj["calibration_forward_mv"] = calibration.result.referenceMv;
    obj["calibration_reverse_mv"] = calibration.result.reverseMv;
    obj["probe_mv"] = probeMv; obj["early_mv"] = probeEarlyMv;
    obj["sampling"]="passive_pwm"; obj["gain_a"]=measured.a; obj["gain_b"]=measured.b;
    obj["fit_error_mv"]=measured.rms; obj["adc_noise_mv"]=measured.noise; obj["samples"]=measured.samples;
    obj["short_power_mw"]=protection::shortPowerMw(previousOutput.a,previousOutput.b);
    obj["thermal_budget_mw"]=protection::ThermalBudgetMw;
    obj["monitoring_required"]=measurementRequired();
    obj["ignored_brightness"]=ignoredBrightness();
    auto limits=obj.createNestedObject("limits");
    const char *names[]={"first","second","all"};
    unsigned maxPower=0;
    for (unsigned i=0;i<3;++i) {
        auto limit=limits.createNestedObject(names[i]);
        limit["automatic"]=protectionLimits.automatic[i]; limit["ignored"]=protectionLimits.ignored[i];
        limit["safe_max"]=protection::maxIgnored(i);
        unsigned power=protection::shortPowerMw(protectionLimits.ignored[i]*5,i==2 ? protectionLimits.ignored[i]*5 : 0);
        limit["short_power_mw"]=power; maxPower=std::max(maxPower,power);
        limit["noise_mv"]=protectionLimits.noise[i]; limit["fit_error_mv"]=protectionLimits.error[i];
    }
    obj["thermal_warning"]=maxPower>protection::ThermalBudgetMw;
    obj["ignored_short_power_mw"]=maxPower;
    const auto actual=pwm::read().output;
    obj["pwm_a"]=actual.a; obj["pwm_b"]=actual.b;
    obj["rc_tau_ticks"]=pwmCalibration.tau; obj["phase_shift_ticks"]=pwmCalibration.shift;
    obj["sample_age_ms"] = uint32_t(millis() - probeAt);
    obj["pending_ms"] = faultTimer.pending ? uint32_t(millis() - faultTimer.since) : 0;
    obj["calibrating"] = calibration.active; obj["progress"] = std::max(0, calibration.step) * 100 / 308;
    obj["message"] = calibration.message;
}
static const char *command(JsonObjectConst doc) {
    Guard guard;
    if (updating || rebootAt) return "Probíhá aktualizace nebo restart.";
    auto next = light; const char *error = lights::control(doc, next, onPreset, scheduleConfig.onePercentOff);
    if (error) return error;
    if (calibration.active) {
        if (next.on) return "Probíhá kalibrace. Tlačítkem Vypnout ji lze přerušit.";
        cancelCalibration("Kalibrace přerušena uživatelem.");
    }
    if (next.on && next.brightness && (protection::restartBlocked(protectionConfig, faultCode) || monitorFailed ||
        (protectionConfig.enabled && !protectionConfig.calibrated)))
        return "Výstup blokuje ochrana. Otevři nastavení ochrany a kalibraci.";
    if (next.on && next.brightness && faultCode) {
        if (prefs.putUChar("fault", 0) != 1) return "Vypnutí ochranou se nepodařilo potvrdit.";
        faultCode = 0; resetMeasurement();
        // Resume only the requested normal PWM. The observer checks its waveform.
    }
    if (next.mode != light.mode || next.intervalMs != light.intervalMs || next.on != light.on) {
        effectAt = millis();
    }
    if (doc.containsKey("state") || doc.containsKey("brightness")) manualScheduleOverride();
    light = next; changedAt = millis(); dirty = true; statePending = true;
    checkpoint();
    syncOutput();
    return nullptr;
}
static bool saveLight() {
    // NVS commits only after 2 s of inactivity, never on every slider movement.
    bool ok = prefs.putBytes("light", &light, sizeof(light)) == sizeof(light);
    storageError = !ok; if (ok) dirty = false;
    return ok;
}
static void setTimezone() {
    setenv("TZ", schedule::Zones[scheduleConfig.zone].rule, 1); tzset();
    schedulePlan.date=0;
}
static void refreshPlan(time_t now) {
    if (schedule::clockValid(now) && schedulePlan.date!=schedule::dayKey(schedule::local(now)))
        schedulePlan.build(scheduleConfig,now);
}
static bool presetEnabled(const lights::Preset &p) {return p.brightness && !(scheduleConfig.onePercentOff && p.brightness==1);}
static bool nightEnabled() {return presetEnabled(nightPreset);}
static const lights::Preset &phasePreset(schedule::Phase phase) {
    return phase==schedule::Night?nightPreset:phase==schedule::Morning?windowPresets.morning:windowPresets.evening;
}
static void manualScheduleOverride() {
    if (!scheduleConfig.enabled) return;
    time_t now=time(nullptr); refreshPlan(now);
    manualOverride=true; overrideUntil=schedule::clockValid(now)?schedulePlan.next(now,nightEnabled()):0;
}
// Runs in the main loop, never in the PWM/ADC timing task. The caller holds mutex.
static void scheduleTick(bool force) {
    static time_t previous=0;
    static schedule::Phase applied=schedule::Off;
    const time_t now=time(nullptr);
    if (!force && now==previous) return;
    previous=now;
    if (!scheduleConfig.enabled || updating || rebootAt || calibration.active) return;
    refreshPlan(now);
    if (manualOverride) {
        if (!overrideUntil && schedule::clockValid(now)) overrideUntil=schedulePlan.next(now,nightEnabled());
        if (!overrideUntil || now<overrideUntil) return;
        manualOverride=false; overrideUntil=0; statePending=true;force=true;
    }
    // Scheduled ON never acknowledges a protection event, even in non-latching mode.
    auto phase=schedule::clockValid(now)?schedulePlan.phase(now,nightEnabled()):schedule::Off;
    bool changed=phase!=applied;applied=phase;if (changed) statePending=true;
    bool on=phase!=schedule::Off && presetEnabled(phasePreset(phase)) && !protectionBlocks();
    if (light.on==on && (!on || (!force && !changed))) return;
    if (on) {const auto &preset=phasePreset(phase);light.brightness=preset.brightness;light.mode=preset.mode;}
    light.on=on; effectAt=millis(); changedAt=millis(); dirty=true; statePending=true;
    syncOutput();
}
static void localText(JsonObject o,const char *key,time_t at) {
    if (!at) {o[key]=nullptr;return;}
    char text[40]; auto date=schedule::local(at);
    strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S %Z",&date);o[key]=text;
}
static void scheduleStatus(JsonObject o) {
    time_t now=time(nullptr);refreshPlan(now);
    bool valid=schedule::clockValid(now);
    o["enabled"]=scheduleConfig.enabled;o["time_valid"]=valid;
    o["synced"]=lastTimeSync.load()!=0;o["epoch"]=valid?now:0;
    o["last_sync"]=lastTimeSync.load();o["timezone"]=schedule::Zones[scheduleConfig.zone].name;
    o["manual_override"]=manualOverride;o["one_percent_off"]=scheduleConfig.onePercentOff;
    auto phase=valid?schedulePlan.phase(now,nightEnabled()):schedule::Off;
    o["phase"]=schedule::phaseName(phase);o["scheduled_on"]=phase!=schedule::Off && presetEnabled(phasePreset(phase));
    lights::json(o.createNestedObject("night_gap"),nightPreset);
    lights::json(o.createNestedObject("window_presets"),windowPresets);
    localText(o,"local_time",valid?now:0);
    localText(o,"sunrise",valid?schedulePlan.sunrise:0);localText(o,"sunset",valid?schedulePlan.sunset:0);
    localText(o,"next_change",valid?schedulePlan.next(now,nightEnabled()):0);
    localText(o,"override_until",overrideUntil);
}
static void fillState(JsonDocument &doc) {
    Guard guard; lights::json(doc, light);
    doc["version"] = Version; doc["id"] = deviceId;
    doc["alias"] = hostname; doc["ap_ssid"] = apName;
    doc["board"] = BoardId; doc["flash_bytes"] = ESP.getFlashChipSize();
    doc["uptime_ms"] = millis(); doc["ota_slot"] = esp_ota_get_running_partition()->label;
    doc["wifi"] = WiFi.status() == WL_CONNECTED;
    doc["wifi_rssi_dbm"] = WiFi.RSSI();doc["heap_free_bytes"] = ESP.getFreeHeap();
    const auto wifiMode = WiFi.getMode();
    doc["wifi_mode"] = int(wifiMode);
    doc["ap_active"] = bool(wifiMode & WIFI_AP);
    doc["ip"] = WiFi.localIP().toString(); doc["hostname"] = hostname;
    const char *dhcpName=nullptr;auto sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_hostname(sta,&dhcpName)==ESP_OK) doc["dhcp_hostname"]=dhcpName;
    doc["mqtt"] = mqttOnline.load(); doc["mqtt_error"] = mqttError.load();
    doc["topic"] = baseTopic; doc["updating"] = updating.load(); doc["storage_error"] = storageError;
    doc["output_active"] = !updating && !rebootAt && !protectionBlocks() && light.on && light.brightness;
    auto health=doc.createNestedObject("health");
    health["reset_reason"]=resetName();health["boot_count"]=bootCount;
    health["last_recovery"]=lastRecovery==ControlRecovery?"control_stall":lastRecovery==HttpRecovery?"http_stall":"none";
    health["watchdog_seconds"]=reliability::WatchdogSeconds;
    health["resume_restored"]=resumeRestored;
    health["wifi_disconnects"]=disconnects.load();health["wifi_disconnect_reason"]=disconnectReason.load();
    health["wifi_last_disconnect_age_ms"]=disconnects?age(lastDisconnectAt):0;
    health["wifi_retries"]=reconnects.load();health["wifi_station_resets"]=stationResets.load();health["wifi_radio_resets"]=radioResets.load();
    health["fallback_starts"]=fallbackStarts.load();
    health["loop_age_ms"]=age(loopHeartbeat);health["control_age_ms"]=age(controlHeartbeat);
    health["network_age_ms"]=age(networkHeartbeat);health["mqtt_age_ms"]=age(mqttHeartbeat);
    health["heap_min_bytes"]=ESP.getMinFreeHeap();health["heap_largest_block_bytes"]=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    auto pwmInfo=doc.createNestedObject("pwm");const auto timing=pwm::diagnostics();
    pwmInfo["task_priority"]=pwm::TaskPriority;pwmInfo["carrier_hz"]=1000;pwmInfo["frame_ms"]=5;pwmInfo["task_age_ms"]=age(pwm::heartbeat);
    pwmInfo["writes"]=timing.writes;pwmInfo["bridge_switches"]=timing.bridgeSwitches;
    pwmInfo["frames"]=timing.frames;pwmInfo["max_frame_gap_us"]=timing.maxGapUs;
    pwmInfo["stable_windows"]=timing.windows;pwmInfo["edge_errors"]=timing.edgeErrors;
    pwmInfo["last_error_window_us"]=timing.errorWindowUs;pwmInfo["last_error_a"]=timing.errorA;pwmInfo["last_error_b"]=timing.errorB;
    pwmInfo["pulses_a"]=timing.pulsesA;pwmInfo["pulses_b"]=timing.pulsesB;pwmInfo["window_us"]=timing.windowUs;
    protectionJson(doc.createNestedObject("protection"));
    lights::json(doc.createNestedObject("on_preset"),onPreset);
    scheduleStatus(doc.createNestedObject("schedule"));
}

static void networkJson(JsonObject o) {
    o["ssid"]=settings.ssid;o["wifi_password_set"]=bool(*settings.wifiPassword);
    o["mqtt_host"]=settings.mqttHost;o["mqtt_port"]=settings.mqttPort;
    o["mqtt_user"]=settings.mqttUser;o["mqtt_password_set"]=bool(*settings.mqttPassword);
    o["mqtt_enabled"]=settings.mqttEnabled;
}
static void fillConfig(JsonDocument &doc) {
    Guard guard;doc["alias"]=hostname;doc["version"]=Version;
    networkJson(doc.createNestedObject("network"));
    lights::json(doc.createNestedObject("on_preset"),onPreset);
    schedule::json(doc.createNestedObject("schedule"),scheduleConfig);
    lights::json(doc.createNestedObject("night_gap"),nightPreset);
    lights::json(doc.createNestedObject("window_presets"),windowPresets);
    auto p=doc.createNestedObject("protection");
    p["enabled"]=protectionConfig.enabled;p["latch_fault"]=bool(protectionConfig.latchFault);
    p["delay_ms"]=protectionConfig.delayMs;p["threshold_percent"]=protectionConfig.thresholdPercent;
    auto limits=p.createNestedObject("limits");const char *names[]={"first","second","all"};
    for (unsigned i=0;i<3;++i) limits[names[i]]=protectionLimits.ignored[i];
}
static void fillCalibration(JsonDocument &doc) {
    Guard guard;
    doc["calibrated"]=protectionConfig.calibrated;doc["signature"]=pwmCalibration.signature;
    doc["reference_mv"]=protectionConfig.referenceMv;doc["reverse_mv"]=protectionConfig.reverseMv;
    doc["zero_mv"]=protectionConfig.zeroMv;
    auto profiles=doc.createNestedArray("profiles_mv");
    for (auto &bank:protectionConfig.profile) {auto a=profiles.createNestedArray();for (auto mv:bank) a.add(mv);}
    auto pwm=doc.createNestedObject("pwm");pwm["zero_mv"]=pwmCalibration.zero;
    pwm["tau_ticks"]=pwmCalibration.tau;pwm["phase_shift_ticks"]=pwmCalibration.shift;
    auto refs=pwm.createNestedArray("references_mv");
    for (auto &bank:pwmCalibration.references) {auto a=refs.createNestedArray();for (auto mv:bank) a.add(mv);}
    auto quality=doc.createNestedObject("quality");const char *names[]={"first","second","all"};
    for (unsigned i=0;i<3;++i) {auto q=quality.createNestedObject(names[i]);
        q["automatic"]=protectionLimits.automatic[i];q["noise_mv"]=protectionLimits.noise[i];q["fit_error_mv"]=protectionLimits.error[i];}
}
static esp_err_t jsonReply(httpd_req_t *r, const JsonDocument &doc) {
    String body; serializeJson(doc, body);
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, body.c_str(), body.length());
}
static esp_err_t errorReply(httpd_req_t *r, const char *message, const char *status = "400 Bad Request") {
    httpd_resp_set_status(r, status);
    StaticJsonDocument<256> doc; doc["error"] = message; return jsonReply(r, doc);
}
static esp_err_t okay(httpd_req_t *r) {
    StaticJsonDocument<32> doc; doc["ok"] = true; return jsonReply(r, doc);
}
static bool validPost(httpd_req_t *r) {
    // Cross-origin forms cannot set this header. No permissive CORS response.
    char requested[24] = {};
    if (httpd_req_get_hdr_value_str(r, "X-Requested-With", requested, sizeof(requested)) != ESP_OK || strcmp(requested, "Svetylka")) {
        errorReply(r, "Chybí hlavička požadavku.", "403 Forbidden"); return false;
    }
    return true;
}
static bool receive(httpd_req_t *r, void *destination, size_t size, int64_t deadline) {
    size_t offset = 0;
    while (offset < size) {
        if (esp_timer_get_time()>=deadline) return false;
        int count = httpd_req_recv(r, static_cast<char *>(destination) + offset, size - offset);
        if (count <= 0) return false;
        offset += count;httpHeartbeat=millis();
    }
    return true;
}
static bool readJson(httpd_req_t *r, JsonDocument &doc) {
    if (!r->content_len || r->content_len > 2048) { errorReply(r, "Požadavek má nesprávnou velikost."); return false; }
    char body[2049];
    if (!receive(r, body, r->content_len,esp_timer_get_time()+15000000)) { errorReply(r, "Přenos nebyl dokončen."); return false; }
    body[r->content_len] = 0;
    if (memchr(body, 0, r->content_len) || deserializeJson(doc, body) || !doc.is<JsonObject>()) {
        errorReply(r, "Neplatný JSON objekt."); return false;
    }
    return true;
}
extern const char htmlStart[] asm("_binary_web_index_html_start");
extern const char cssStart[] asm("_binary_web_style_css_start");
extern const char jsStart[] asm("_binary_web_app_js_start");
static esp_err_t get(httpd_req_t *r) {
    httpHeartbeat=millis();
    String path = r->uri;
    if (path.startsWith("/api/schedule")) {
        // Validate before taking the control lock: even an error response can
        // block on a slow TCP peer and must never stop PWM sensing or the plan.
        int year=0,month=0,day=0;char query[48],text[16],extra;
        if (httpd_req_get_url_query_str(r,query,sizeof(query))==ESP_OK &&
            httpd_query_key_value(query,"date",text,sizeof(text))==ESP_OK) {
            if (sscanf(text,"%d-%d-%d%c",&year,&month,&day,&extra)!=3 || year<2024 || year>2037 || month<1 || month>12 || day<1 || day>31)
                return errorReply(r,"Datum musí být YYYY-MM-DD v letech 2024–2037.");
            tm date{};date.tm_year=year-1900;date.tm_mon=month-1;date.tm_mday=day;date=schedule::dateShift(date,0);
            if (date.tm_year!=year-1900 || date.tm_mon!=month-1 || date.tm_mday!=day) return errorReply(r,"Neplatný den v měsíci.");
        }
        DynamicJsonDocument doc(4096);
        { Guard guard;
          schedule::json(doc.createNestedObject("config"),scheduleConfig);
          lights::json(doc.createNestedObject("night_gap"),nightPreset);
          lights::json(doc.createNestedObject("window_presets"),windowPresets);
          scheduleStatus(doc.createNestedObject("status"));
          auto zones=doc.createNestedArray("timezones");for (auto &z:schedule::Zones) zones.add(z.name);
          time_t now=time(nullptr);
          if (schedule::clockValid(now)) {
              tm date=schedule::local(now);
              if (year) {
                  date={};date.tm_year=year-1900;date.tm_mon=month-1;date.tm_mday=day;date=schedule::dateShift(date,0);
              }
              auto day=doc.createNestedObject("day");day["date"]=schedule::dayKey(date);
              auto rise=schedule::event(scheduleConfig,date,{schedule::Sunrise,0,0});
              auto set=schedule::event(scheduleConfig,date,{schedule::Sunset,0,0});
              day["sunrise_epoch"]=rise;day["sunset_epoch"]=set;
              localText(day,"sunrise",rise);localText(day,"sunset",set);
              auto windows=day.createNestedArray("windows");
              for (auto &w:scheduleConfig.windows) {auto span=schedule::interval(scheduleConfig,date,w);auto o=windows.createNestedObject();
                  o["enabled"]=w.enabled;o["start_epoch"]=span.start;o["end_epoch"]=span.end;
                  localText(o,"start",span.start);localText(o,"end",span.end);
              }
              auto gap=schedule::nightInterval(scheduleConfig,date);auto o=day.createNestedObject("night_gap");
              o["enabled"]=nightEnabled();o["start_epoch"]=gap.start;o["end_epoch"]=gap.end;
              localText(o,"start",gap.start);localText(o,"end",gap.end);
          }
        }
        return jsonReply(r,doc);
    }
    if (path == "/api/scope") {
        // Passive, phase-tagged measurements only: never write to the bridge.
        uint32_t sums[64] = {}, counts[64] = {}; unsigned accepted = 0, rejected = 0, maxUs = 0;
        unsigned highA=0, highB=0, pads=0, pwmFrames=0;
        lights::Output out;
        { Guard guard;
          const auto frame=pwm::read();out=frame.output;
          const unsigned startFrames=pwm::diagnostics().frames;
          const int64_t stop = esp_timer_get_time() + 40000;
          while (esp_timer_get_time() < stop) {
              highA+=gpio_get_level(GPIO_NUM_25); highB+=gpio_get_level(GPIO_NUM_26); ++pads;
              unsigned begin = LEDC.timer_group[0].timer[0].value.timer_cnt;
              int64_t at = esp_timer_get_time();
              unsigned raw = adc1_get_raw(ADC1_CHANNEL_4);
              unsigned end = LEDC.timer_group[0].timer[0].value.timer_cnt;
              unsigned us = esp_timer_get_time() - at;
              maxUs = std::max(maxUs, us);
              if (!pwm::stable(frame)) break;
              if (us > 50) { ++rejected; continue; }
              unsigned phase = (begin + ((end - begin) & 1023) / 2) & 1023;
              sums[phase / 16] += raw; ++counts[phase / 16]; ++accepted;
              delayMicroseconds(esp_random() % 13);
          }
          pwmFrames=pwm::diagnostics().frames-startFrames;
        }
        DynamicJsonDocument doc(6144);
        doc["duty_a"] = out.a; doc["duty_b"] = out.b;
        doc["accepted"] = accepted; doc["rejected"] = rejected; doc["max_us"] = maxUs;
        doc["pwm_frames"]=pwmFrames;
        doc["pad_samples"]=pads; doc["high_a"]=highA; doc["high_b"]=highB;
        auto wave = doc.createNestedArray("mv"); auto n = doc.createNestedArray("counts");
        for (unsigned i=0; i<64; ++i) { wave.add(counts[i] ? esp_adc_cal_raw_to_voltage(sums[i]/counts[i], &adcCalibration) : 0); n.add(counts[i]); }
        return jsonReply(r, doc);
    }
    if (path == "/api/state") {
        DynamicJsonDocument doc(6144); fillState(doc);
        doc["http_stack_free_bytes"] = uxTaskGetStackHighWaterMark(nullptr);
        return jsonReply(r, doc);
    }
    if (path == "/api/protection") {
        DynamicJsonDocument doc(8192);
        { Guard guard;
          protectionJson(doc.to<JsonObject>());
          auto profiles = doc.createNestedArray("profiles_mv");
          for (auto &bank : protectionConfig.profile) {
              auto values = profiles.createNestedArray();
              for (uint16_t mv : bank) values.add(mv);
          }
        }
        return jsonReply(r, doc);
    }
    if (path == "/api/settings") {
        StaticJsonDocument<768> doc;
        doc["alias"] = hostname;
        networkJson(doc.as<JsonObject>());
        return jsonReply(r, doc);
    }
    const char *body = path == "/style.css" ? cssStart : path == "/app.js" ? jsStart : htmlStart;
    httpd_resp_set_type(r, path == "/style.css" ? "text/css" : path == "/app.js" ? "application/javascript" : "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(r, body, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t update(httpd_req_t *r) {
    bool calibrating;{Guard guard;calibrating=calibration.active;}
    if (calibrating) return errorReply(r, "Nejdřív dokonči nebo zruš kalibraci.", "409 Conflict");
    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || r->content_len < 1024 || r->content_len > partition->size) return errorReply(r, "Nesprávná velikost firmware.");
    { Guard guard; checkpoint();updating = true; monitorArmed = false; pwm::stop(); }
    statePending = true;
    esp_ota_handle_t handle = 0;
    esp_err_t result = esp_ota_begin(partition, r->content_len, &handle);
    if (result == ESP_OK) {
        char buffer[2048]; size_t remaining = r->content_len;
        const int64_t deadline=esp_timer_get_time()+180000000;
        while (remaining) {
            size_t size = std::min(remaining, sizeof(buffer));
            if (!receive(r, buffer, size,deadline)) { result = ESP_FAIL; break; }
            result = esp_ota_write(handle, buffer, size);
            if (result != ESP_OK) break;
            remaining -= size;
        }
        if (result == ESP_OK) result = esp_ota_end(handle);
        else esp_ota_abort(handle);
    }
    if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        updating = false; statePending = true;
        return errorReply(r, "Aktualizace selhala. Původní firmware zůstává aktivní.");
    }
    { Guard guard; if (dirty) saveLight(); }
    rebootAt = millis() + 1200; return okay(r);
}
struct CommandResult {
    const char *error, *status;
    CommandResult(const char *e=nullptr,const char *s="400 Bad Request"):error(e),status(s) {}
};
static CommandResult applyCommand(const String &path,const JsonDocument &doc) {
    if (path=="/api/light") return {command(doc.as<JsonObjectConst>())};
    Guard guard;
    if (updating || rebootAt) return {"Probíhá aktualizace nebo restart.","409 Conflict"};
    if (path=="/api/schedule/presets") {
        if (calibration.active) return {"Probíhá kalibrace.","409 Conflict"};
        auto next=windowPresets;
        if (const char *error=lights::parsePresets(doc.as<JsonObjectConst>(),next)) return {error};
        if (next.evening.brightness==windowPresets.evening.brightness && next.evening.mode==windowPresets.evening.mode &&
            next.morning.brightness==windowPresets.morning.brightness && next.morning.mode==windowPresets.morning.mode) return {};
        if (prefs.putBytes("daypresets",&next,sizeof(next))!=sizeof(next))
            return {"Večerní a ranní režim se nepodařilo uložit.","500 Internal Server Error"};
        windowPresets=next;
        scheduleTick(true);statePending=true;configPending=true;return {};
    }
    if (path=="/api/schedule/night") {
        if (calibration.active) return {"Probíhá kalibrace.","409 Conflict"};
        auto next=nightPreset;
        if (const char *error=lights::parsePreset(doc.as<JsonObjectConst>(),next,0)) return {error};
        if (next.brightness==nightPreset.brightness && next.mode==nightPreset.mode) return {};
        if (prefs.putBytes("nightgap",&next,sizeof(next))!=sizeof(next))
            return {"Noční režim se nepodařilo uložit.","500 Internal Server Error"};
        nightPreset=next;
        if (manualOverride) {overrideUntil=0;manualScheduleOverride();}
        scheduleTick(true);statePending=true;configPending=true;return {};
    }
    if (path=="/api/preset") {
        auto next=onPreset;
        if (const char *error=lights::parsePreset(doc.as<JsonObjectConst>(),next)) return {error};
        if (next.brightness==onPreset.brightness && next.mode==onPreset.mode) return {};
        if (prefs.putBytes("onpreset",&next,sizeof(next))!=sizeof(next))
            return {"Předvolbu se nepodařilo uložit.","500 Internal Server Error"};
        onPreset=next;statePending=true;configPending=true;return {};
    }
    if (path=="/api/restart") {
        if (doc.size()) return {"Očekávám prázdný objekt."};
        if (dirty && !saveLight()) return {"Stav se nepodařilo uložit.","500 Internal Server Error"};
        checkpoint();pwm::stop();rebootAt=millis()+1200;return {};
    }
    if (path == "/api/schedule/resume") {
        if (doc.size()) return {"Očekávám prázdný objekt."};
        if (calibration.active) return {"Probíhá kalibrace.","409 Conflict"};
        manualOverride=false;overrideUntil=0;scheduleTick(true);statePending=true;return {};
    }
    if (path == "/api/schedule" || path == "/api/schedule/window/0" || path == "/api/schedule/window/1") {
        if (calibration.active) return {"Probíhá kalibrace.","409 Conflict"};
        auto next=scheduleConfig;
        const char *error=path=="/api/schedule" ? schedule::parse(doc.as<JsonObjectConst>(),next) :
            schedule::patchWindow(doc.as<JsonObjectConst>(),path.endsWith("/1")?1:0,next);
        if (error) return {error};
        if (next.checksum==scheduleConfig.checksum) return {};
        if (prefs.putBytes("schedule",&next,sizeof(next))!=sizeof(next))
            return {"Plán se nepodařilo uložit.","500 Internal Server Error"};
        bool switched=next.enabled!=scheduleConfig.enabled;
        scheduleConfig=next;setTimezone();
        if (switched) {manualOverride=false;overrideUntil=0;}
        else if (manualOverride) {overrideUntil=0;manualScheduleOverride();}
        if (scheduleConfig.onePercentOff && light.brightness==1 && light.on) {
            light.on=false;pwm::stop();changedAt=millis();dirty=true;
            manualScheduleOverride();
        }
        scheduleTick(true);statePending=true;configPending=true;return {};
    }
    if (path == "/api/protection/limits") {
        if (calibration.active) return {"Probíhá kalibrace.","409 Conflict"};
        auto next=protectionLimits;
        const char *error=protection::parseLimits(doc.as<JsonObjectConst>(),next);
        if (error) return {error};
        if (next.checksum==protectionLimits.checksum) return {};
        if (prefs.putBytes("pwmlimits",&next,sizeof(next))!=sizeof(next)) {
            trip(5); return {"Hranice se nepodařilo uložit.","500 Internal Server Error"};
        }
        protectionLimits=next; resetMeasurement(); statePending=true;configPending=true; return {};
    }
    if (path == "/api/protection") {
        if (calibration.active) return { "Probíhá kalibrace.", "409 Conflict"};
        auto next = protectionConfig;
        const char *error = protection::parse(doc.as<JsonObjectConst>(), next);
        if (error) return { error};
        if (next.checksum==protectionConfig.checksum) return {};
        if (!saveProtection(next)) return { "Ochranu se nepodařilo uložit.", "500 Internal Server Error"};
        resetMeasurement(); syncOutput(); statePending = true;configPending=true;
        return {};
    }
    if (path == "/api/protection/action") {
        if (doc.size() != 1 || !doc["action"].is<const char *>()) return { "Neplatná akce ochrany."};
        JsonString action = doc["action"].as<JsonString>();
        if (action.size() != strlen(action.c_str())) return { "Neplatná akce ochrany."};
        if (!strcmp(action.c_str(), "cancel")) {
            if (calibration.active) {cancelCalibration("Kalibrace přerušena uživatelem.");manualScheduleOverride();}
            return {};
        }
        if (calibration.active) return { "Probíhá kalibrace.", "409 Conflict"};
        if (!strcmp(action.c_str(), "clear")) {
            if (light.on) return { "Nejdřív světlo vypni."};
            if (prefs.putUChar("fault", 0) != 1) return { "Poruchu se nepodařilo odblokovat.", "500 Internal Server Error"};
            faultCode = 0; monitorFailed = false; resetMeasurement();
            manualScheduleOverride();
            calibration.message = "Porucha odblokována. Výstup zůstává vypnutý.";
            statePending = true; return {};
        }
        if (strcmp(action.c_str(), "calibrate")) return { "Neznámá akce ochrany."};
        calibration.restore = light;
        light.on = false; pwm::stop();
        if (!saveLight()) return { "Stav se nepodařilo uložit.", "500 Internal Server Error"};
        calibration.result = protectionConfig; calibration.pwm = sensing::Calibration{}; calibration.limits={};
        calibration.active = true; calibration.step = -2; prepareCalibrationPoint();
        calibration.output = {0, 0}; calibration.message = "Kontroluji klidovou úroveň snímače.";
        faultTimer.clear(); statePending = true; return {};
    }
    if (path == "/api/alias") {
        char next[33]; const char *error = parseAlias(doc.as<JsonObjectConst>(), next);
        if (error) return { error};
        if (hostname==next) return {};
        if ((dirty && !saveLight()) || !prefs.putString("alias", next))
            return { "Název se nepodařilo uložit.", "500 Internal Server Error"};
        pwm::stop(); rebootAt = millis() + 1500; return {};
    }
    if (path!="/api/settings") return {"Neznámý příkaz."};
    Settings next=settings;const char *error=parseSettings(doc.as<JsonObjectConst>(),next);
    if (error) return {error};
    if (!memcmp(&next,&settings,sizeof(next))) return {};
    if ((dirty && !saveLight()) || prefs.putBytes("network",&next,sizeof(next))!=sizeof(next))
        return {"Nastavení se nepodařilo uložit.","500 Internal Server Error"};
    pwm::stop();rebootAt=millis()+1500;return {};
}
static esp_err_t post(httpd_req_t *r) {
    httpHeartbeat=millis();
    if (!validPost(r)) return ESP_OK;
    if (updating || rebootAt) return errorReply(r,"Probíhá aktualizace nebo restart.","409 Conflict");
    String path=r->uri;
    if (path=="/api/update") return update(r);
    StaticJsonDocument<2048> doc;
    if (!readJson(r,doc)) return ESP_OK;
#ifdef RELIABILITY_TEST
    // Fault injection exists only in the temporary OTA test binary.
    if (path=="/api/test") {
        if (doc["lock_ms"].is<unsigned>() && doc["lock_ms"].as<unsigned>()<=250) {
            const unsigned duration=doc["lock_ms"];
            const auto before=pwm::diagnostics();
            {Guard guard;vTaskDelay(pdMS_TO_TICKS(duration));}
            const auto after=pwm::diagnostics();StaticJsonDocument<256> result;
            result["frames"]=after.frames-before.frames;result["writes"]=after.writes-before.writes;
            result["bridge_switches"]=after.bridgeSwitches-before.bridgeSwitches;
            result["edge_errors"]=after.edgeErrors-before.edgeErrors;
            result["max_frame_gap_us"]=after.maxGapUs;return jsonReply(r,result);
        }
        if (doc["offline_ms"].is<unsigned>() && doc["offline_ms"].as<unsigned>()<=120000) {
            testOfflineUntil=millis()+doc["offline_ms"].as<unsigned>();return okay(r);
        }
        if (schedule::textEquals(doc["network"],"station")) {testNetworkReset=1;return okay(r);}
        if (schedule::textEquals(doc["network"],"radio")) {testNetworkReset=2;return okay(r);}
        const char *names[]={"control","network","loop","mqtt","http"};
        for (int i=0;i<5;++i) if (schedule::textEquals(doc["stall"],names[i])) {
            testStall=i+1;return okay(r);
        }
        return errorReply(r,"Neplatná testovací akce.");
    }
#endif
    auto result=applyCommand(path,doc);
    if (result.error) return errorReply(r,result.error,result.status);
    if (path=="/api/light") {DynamicJsonDocument state(6144);fillState(state);return jsonReply(r,state);}
    return okay(r);
}
static void initWeb() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    configASSERT(config.task_priority<pwm::TaskPriority);
    // Optimized POST frame uses ~6.7 KB, plus HTTP/JSON/OTA call stacks.
    config.max_uri_handlers = 24; config.stack_size = 18432;
    config.recv_wait_timeout = 5;config.send_wait_timeout=3; config.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&httpServer, &config));
    for (const char *path : {"/", "/style.css", "/app.js", "/api/state", "/api/settings", "/api/protection", "/api/scope", "/api/schedule"}) {
        httpd_uri_t uri{}; uri.uri = path; uri.method = HTTP_GET; uri.handler = get;
        ESP_ERROR_CHECK(httpd_register_uri_handler(httpServer, &uri));
    }
    for (const char *path : {"/api/light", "/api/settings", "/api/alias", "/api/update", "/api/restart", "/api/protection", "/api/protection/action", "/api/protection/limits", "/api/schedule", "/api/schedule/resume", "/api/schedule/window/0", "/api/schedule/window/1", "/api/preset", "/api/schedule/night", "/api/schedule/presets"}) {
        httpd_uri_t uri{}; uri.uri = path; uri.method = HTTP_POST; uri.handler = post;
        ESP_ERROR_CHECK(httpd_register_uri_handler(httpServer, &uri));
    }
#ifdef RELIABILITY_TEST
    httpd_uri_t test{};test.uri="/api/test";test.method=HTTP_POST;test.handler=post;
    ESP_ERROR_CHECK(httpd_register_uri_handler(httpServer,&test));
#endif
}
static bool publishJson(PubSubClient &mqtt, const String &topic, const JsonDocument &doc) {
    mqttHeartbeat=millis();esp_task_wdt_reset();
    if (doc.overflowed()) return false;
    String body; serializeJson(doc, body); return mqtt.publish(topic.c_str(), body.c_str(), true);
}
#include "mqtt_controls.h"
static bool discovery(PubSubClient &mqtt) {
    StaticJsonDocument<2048> doc;
    doc["name"] = hostname; doc["unique_id"] = deviceId;
    doc["schema"] = "json"; doc["command_topic"] = baseTopic + "/set";
    doc["state_topic"] = baseTopic + "/light/state"; doc["availability_topic"] = baseTopic + "/availability";
    doc["brightness"] = true; doc["brightness_scale"] = 100;
    doc["effect"] = true;
    doc["flash"] = false; doc["transition"] = false;
    doc["retain"] = false;
    auto effects = doc.createNestedArray("effect_list");
    for (unsigned i = 0; i < lights::ModeCount; ++i) effects.add(lights::modeLabel(lights::Mode(i)));
    auto device = doc.createNestedObject("device"); device.createNestedArray("identifiers").add(deviceId);
    device["name"] = hostname; device["model"] = "ESP32 + IN/IN H-bridge";
    device["sw_version"] = Version; device["configuration_url"] = "http://" + WiFi.localIP().toString();
    bool ok=publishJson(mqtt, "homeassistant/light/" + deviceId + "/config", doc);
    return discoverControls(mqtt) && ok;
}
static std::atomic<bool> discoverPending{false};
static bool migrateMqttAlias(PubSubClient &mqtt) {
    String previous;
    { Guard guard; previous = prefs.getString("mqtt_base", ("svetylka/" + deviceId).c_str()); }
    if (previous == baseTopic) return true;
    for (const char *suffix:{"/state","/light/state","/availability","/config","/calibration"})
        if (!mqtt.publish((previous+suffix).c_str(),"",true)) return false;
    Guard guard;
    if (prefs.putString("mqtt_base", baseTopic)) return true;
    storageError = true; return false;
}
static String mqttReply; // Used only by the MQTT task; publish after loop() leaves its receive buffer.
static void mqttMessage(char *topic, uint8_t *payload, unsigned length) {
    if (!strcmp(topic,"homeassistant/status")) {
        if (length==6 && !memcmp(payload,"online",6)) discoverPending=true;
        return;
    }
    String prefix=baseTopic+"/";
    if (!String(topic).startsWith(prefix)) return;
    String suffix=String(topic).substring(prefix.length()),path;
    if (suffix=="set") path="/api/light";
    else if (suffix=="preset/set") path="/api/preset";
    else if (suffix=="schedule/set") path="/api/schedule";
    else if (suffix=="schedule/night/set") path="/api/schedule/night";
    else if (suffix=="schedule/presets/set") path="/api/schedule/presets";
    else if (suffix=="schedule/window/0/set") path="/api/schedule/window/0";
    else if (suffix=="schedule/window/1/set") path="/api/schedule/window/1";
    else if (suffix=="protection/set") path="/api/protection";
    else if (suffix=="protection/limits/set") path="/api/protection/limits";
    else if (suffix=="protection/action") path="/api/protection/action";
    else if (suffix=="network/set") path="/api/settings";
    else if (suffix=="alias/set") path="/api/alias";
    else if (suffix!="schedule/action" && suffix!="system/action" && suffix!="get") return;
    DynamicJsonDocument doc(3072);
    CommandResult result;
    if (!length || length>2048 || memchr(payload,0,length) || deserializeJson(doc,payload,length) || !doc.is<JsonObject>())
        result={"Neplatný JSON příkaz (nejvýše 2048 bajtů)."};
    else if (suffix=="get") {
        if (doc.size()) result={"Očekávám prázdný objekt."};
        else {configPending=true;calibrationPending=true;}
    } else {
        if (suffix=="schedule/action" || suffix=="system/action") {
            const char *action=suffix=="schedule/action"?"resume":"restart";
            if (doc.size()!=1 || !schedule::textEquals(doc["action"],action)) result={"Neplatná akce."};
            else {path=suffix=="schedule/action"?"/api/schedule/resume":"/api/restart";doc.clear();doc.to<JsonObject>();}
        }
        if (!result.error) result=applyCommand(path,doc);
    }
    StaticJsonDocument<512> reply;reply["command"]=suffix;reply["ok"]=!result.error;
    if (result.error) reply["error"]=result.error;
    reply["uptime_ms"]=millis();reply["restart_pending"]=bool(rebootAt);
    mqttReply="";serializeJson(reply,mqttReply);statePending=true;
}
static void mqttTask(void *) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    WiFiClient socket; PubSubClient mqtt(socket);
    mqtt.setBufferSize(8192); mqtt.setKeepAlive(15); mqtt.setSocketTimeout(2);
    mqtt.setServer(settings.mqttHost, settings.mqttPort); mqtt.setCallback(mqttMessage);
    uint32_t lastAttempt = millis() - 5000, lastPublish = 0;
    String availability = baseTopic + "/availability";
    for (;;) {
#ifdef RELIABILITY_TEST
        injectStall(4);
#endif
        mqttHeartbeat=millis();esp_task_wdt_reset();
        if (rebootAt) {
            if (mqtt.connected()) { mqtt.publish(availability.c_str(), "offline", true); mqtt.disconnect(); }
            mqttOnline = false;
        } else if (WiFi.status() != WL_CONNECTED || !settings.mqttEnabled || !*settings.mqttHost) {
            mqtt.disconnect(); socket.stop(); mqttOnline = false;
        } else if (!mqtt.connected()) {
            mqttOnline = false;
            if (millis() - lastAttempt >= 5000) {
                lastAttempt = millis(); socket.stop();
                IPAddress ip;
                // Bound TCP connection wait; the PWM and HTTP tasks are independent.
                if (WiFi.hostByName(settings.mqttHost, ip) && socket.connect(ip, settings.mqttPort, 1500)) {
                    if (mqtt.connect(deviceId.c_str(), *settings.mqttUser ? settings.mqttUser : nullptr,
                                     *settings.mqttUser ? settings.mqttPassword : nullptr,
                                     availability.c_str(), 1, true, "offline")) {
                        bool subscribed=mqtt.subscribe("homeassistant/status",1);
                        for (const char *suffix:{"/set","/+/set","/+/action","/get","/protection/limits/set","/schedule/window/+/set","/schedule/night/set","/schedule/presets/set"})
                            subscribed=mqtt.subscribe((baseTopic+suffix).c_str(),1) && subscribed;
                        if (subscribed && migrateMqttAlias(mqtt)) {
                            mqttOnline = true; discoverPending=true;statePending=true;configPending=true;calibrationPending=true;
                            mqtt.publish(availability.c_str(), "online", true);
                        } else mqtt.disconnect();
                    }
                    mqttError = mqtt.state();
                } else mqttError = -2;
            }
        } else {
            mqtt.loop(); mqttOnline = mqtt.connected();
            if (mqttReply.length()) {mqtt.publish((baseTopic+"/result").c_str(),mqttReply.c_str(),false);mqttReply="";}
            if (discoverPending.exchange(false)) { if (!discovery(mqtt)) discoverPending=true;statePending=true;configPending=true;calibrationPending=true; }
            if (configPending.exchange(false)) {DynamicJsonDocument doc(4096);fillConfig(doc);
                if (!publishJson(mqtt,baseTopic+"/config",doc)) configPending=true;}
            if (calibrationPending.exchange(false)) {DynamicJsonDocument doc(12288);fillCalibration(doc);
                if (!publishJson(mqtt,baseTopic+"/calibration",doc)) calibrationPending=true;}
            if (statePending.exchange(false) || millis() - lastPublish >= 15000) {
                DynamicJsonDocument doc(6144); fillState(doc);
                if (!publishJson(mqtt, baseTopic + "/state", doc)) statePending = true;
                StaticJsonDocument<512> ha;
                ha["state"]=doc["state"];ha["brightness"]=doc["brightness"];ha["effect"]=doc["effect_name"];
                if (!publishJson(mqtt,baseTopic+"/light/state",ha)) statePending=true;
                lastPublish = millis();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void startAp() {
    if (apActive || WiFi.status() == WL_CONNECTED) return;
    // softAP enables AP+STA itself; no password means an open fallback network.
    apActive = WiFi.softAP(apName.c_str());
    if (apActive) {++fallbackStarts;Serial.printf("Open fallback Wi-Fi: %s, http://192.168.4.1\n", apName.c_str());}
    else WiFi.enableAP(false);
}
static void beginStation() {
    WiFi.setHostname(hostname.c_str());WiFi.enableSTA(true);
    // One owner retries; avoid racing the framework's automatic reconnect with
    // explicit station/radio recovery after an AP or DHCP failure.
    WiFi.setAutoReconnect(false);WiFi.setSleep(false);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    if (*settings.ssid) WiFi.begin(settings.ssid,settings.wifiPassword);
}
static void httpProbe(void *) {
#ifdef RELIABILITY_TEST
    injectStall(5);
#endif
    httpHeartbeat=millis();httpProbePending=false;
}
static void networkTask(void *) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    reliability::NetworkRecovery recovery;
    uint32_t apRetryAt=millis()-5000,mdnsRetryAt=millis()-5000,probeAt=millis(),probeQueuedAt=millis();
    bool mdns=false,wasConnected=false;
    for (;;) {
#ifdef RELIABILITY_TEST
        injectStall(2);
        const bool testOffline=testOfflineUntil && int32_t(testOfflineUntil.load()-millis())>0;
        if (testOffline && (WiFi.getMode() & WIFI_STA)) WiFi.disconnect(true,false);
#else
        constexpr bool testOffline=false;
#endif
        const uint32_t now=millis();networkHeartbeat=now;esp_task_wdt_reset();
        if (monitorFailed && !updating) recover(ControlRecovery);
        // Queue at most one probe. Incoming requests/OTA chunks also count as
        // progress. Idle HTTP is healthy; an unresponsive HTTP task is not.
        if (uint32_t(now-probeAt)>=5000) {
            probeAt=now;
            if (!httpProbePending.exchange(true)) {
                probeQueuedAt=now;httpd_queue_work(httpServer,httpProbe,nullptr);
            }
        }
        if (httpProbePending && uint32_t(now-probeQueuedAt)>reliability::HttpTimeoutMs && age(httpHeartbeat)>reliability::HttpTimeoutMs)
            recover(HttpRecovery);
        const bool connected=WiFi.status()==WL_CONNECTED && uint32_t(WiFi.localIP())!=0;
        auto action=recovery.tick(now,connected,bool(*settings.ssid),updating || rebootAt || testOffline);
#ifdef RELIABILITY_TEST
        const int force=testNetworkReset.exchange(0);
        if (force) action=force==1?reliability::NetworkAction::Station:reliability::NetworkAction::Radio;
#endif
        if (connected && action==reliability::NetworkAction::None) {
            if ((WiFi.getMode() & WIFI_AP) && WiFi.enableAP(false)) apActive=false;
            if (!wasConnected) {statePending=true;discoverPending=true;mdnsRetryAt=now-5000;}
            if (!mdns && uint32_t(now-mdnsRetryAt)>=5000) {
                mdnsRetryAt=now;mdns=MDNS.begin(hostname.c_str());
                if (mdns) MDNS.addService("http","tcp",80);
            }
        } else {
            if (wasConnected || action!=reliability::NetworkAction::None) {
                if (mdns) {MDNS.end();mdns=false;}
                statePending=true;
            }
            if (action==reliability::NetworkAction::Retry) {
                ++reconnects;if (!WiFi.reconnect()) beginStation();
            } else if (action==reliability::NetworkAction::Station || action==reliability::NetworkAction::Radio) {
                // Keep fallback AP during a station reset; a full radio reset is
                // only needed after a long outage. Neither resets light or time.
                WiFi.setAutoReconnect(false);WiFi.disconnect(true,false);
                if (action==reliability::NetworkAction::Radio) {++radioResets;WiFi.mode(WIFI_OFF);apActive=false;}
                else ++stationResets;
                beginStation();
            }
            if ((!*settings.ssid || uint32_t(now-recovery.offlineAt)>=30000) && !apActive && uint32_t(now-apRetryAt)>=5000 && !updating) {
                apRetryAt=now;startAp();
            }
        }
        wasConnected=connected;vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void controlTask(void *) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    for (;;) {
#ifdef RELIABILITY_TEST
        injectStall(1);
#endif
        { Guard guard;
          previousOutput=pwm::read().output;
          protectionTick(millis());
          syncOutput();
          monitorArmed = !updating && !rebootAt &&
              (calibration.active || (protectionConfig.enabled && light.on && light.brightness));
          controlHeartbeat = millis();
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
static void monitorWatchdog(void *) {
    // Independent of HTTP, MQTT, and the control mutex. No NVS/JSON here.
    if (!updating && !rebootAt && monitorArmed &&
        (age(controlHeartbeat)>500 || age(pwm::heartbeat)>500)) {
        monitorFailed = true; pwm::emergencyStop();
    }
}
void setup() {
#if defined(ARDUINO_LILYGO_T_DISPLAY)
    // No LCD driver: keep the T-Display backlight off and the panel in reset.
    digitalWrite(4, LOW); pinMode(4, OUTPUT);
    digitalWrite(23, LOW); pinMode(23, OUTPUT);
#endif
    Serial.begin(115200); pwm::init(); mutex = xSemaphoreCreateMutex(); configASSERT(mutex);
    // Supervise startup too: Wi-Fi/HTTP initialization must not strand the board
    // before the worker tasks exist. Normal startup is well below this limit.
    ESP_ERROR_CHECK(esp_task_wdt_init(reliability::WatchdogSeconds,true));
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    configASSERT(prefs.begin("svetylka", false));
    bootCount=prefs.getUInt("boot_count",0)+1;prefs.putUInt("boot_count",bootCount);
    const auto reset=esp_reset_reason();
    const bool warm=reset==ESP_RST_SW || reset==ESP_RST_TASK_WDT || reset==ESP_RST_INT_WDT || reset==ESP_RST_WDT || reset==ESP_RST_PANIC;
    lastRecovery=warm?recoveryMarker:0;recoveryMarker=0;
#ifdef BOOTSTRAP_SSID
    strlcpy(settings.ssid, BOOTSTRAP_SSID, sizeof(settings.ssid));
    strlcpy(settings.wifiPassword, BOOTSTRAP_WIFI_PASSWORD, sizeof(settings.wifiPassword));
    strlcpy(settings.mqttHost, BOOTSTRAP_MQTT_HOST, sizeof(settings.mqttHost));
    strlcpy(settings.mqttUser, BOOTSTRAP_MQTT_USER, sizeof(settings.mqttUser));
    strlcpy(settings.mqttPassword, BOOTSTRAP_MQTT_PASSWORD, sizeof(settings.mqttPassword));
    settings.mqttPort = BOOTSTRAP_MQTT_PORT;
#endif
    Settings saved;
    if (prefs.getBytesLength("network") == sizeof(saved) && prefs.getBytes("network", &saved, sizeof(saved)) == sizeof(saved) && validSettings(saved)) settings = saved;
    lights::State savedLight;
    if (prefs.getBytesLength("light") == sizeof(savedLight) && prefs.getBytes("light", &savedLight, sizeof(savedLight)) == sizeof(savedLight) && lights::valid(savedLight)) light = savedLight;
    lights::Preset savedPreset;
    if (prefs.getBytesLength("onpreset")==sizeof(savedPreset) && prefs.getBytes("onpreset",&savedPreset,sizeof(savedPreset))==sizeof(savedPreset) && lights::valid(savedPreset)) onPreset=savedPreset;
    windowPresets.evening=windowPresets.morning=onPreset;
    if (prefs.isKey("daypresets")) {
        lights::WindowPresets savedPresets;
        if (prefs.getBytesLength("daypresets")==sizeof(savedPresets) && prefs.getBytes("daypresets",&savedPresets,sizeof(savedPresets))==sizeof(savedPresets) && lights::valid(savedPresets)) windowPresets=savedPresets;
        else storageError=true;
    } else if (prefs.putBytes("daypresets",&windowPresets,sizeof(windowPresets))!=sizeof(windowPresets)) storageError=true;
    if (prefs.isKey("nightgap")) {
        if (prefs.getBytesLength("nightgap")==sizeof(savedPreset) && prefs.getBytes("nightgap",&savedPreset,sizeof(savedPreset))==sizeof(savedPreset) && lights::valid(savedPreset,0)) nightPreset=savedPreset;
        else storageError=true;
    }
    if (prefs.isKey("schedule")) {
        schedule::Config savedSchedule;
        if (prefs.getBytesLength("schedule")==sizeof(savedSchedule) &&
            prefs.getBytes("schedule",&savedSchedule,sizeof(savedSchedule))==sizeof(savedSchedule) &&
            schedule::valid(savedSchedule) && savedSchedule.checksum==schedule::checksum(savedSchedule)) scheduleConfig=savedSchedule;
        else {storageError=true;light.on=false;}
    }
    setTimezone();
    if (scheduleConfig.enabled || !light.brightness || (scheduleConfig.onePercentOff && light.brightness==1)) light.on=false;
    protection::Config savedProtection;
    if (prefs.isKey("protection")) {
        if (prefs.getBytesLength("protection") == sizeof(savedProtection) &&
            prefs.getBytes("protection", &savedProtection, sizeof(savedProtection)) == sizeof(savedProtection) &&
            protection::load(savedProtection)) protectionConfig = savedProtection;
        else faultCode = 5; // Corrupt calibration must never silently disable protection.
    }
    faultCode = std::max(faultCode, prefs.getUChar("fault", 0));
    if (faultCode) light.on = false;
    if (protectionConfig.calibrated) {
        if (prefs.getBytesLength("pwmcal")!=sizeof(pwmCalibration) ||
            prefs.getBytes("pwmcal",&pwmCalibration,sizeof(pwmCalibration))!=sizeof(pwmCalibration) ||
            !sensing::valid(pwmCalibration) || pwmCalibration.signature!=calibrationSignature(protectionConfig)) {
            protectionConfig.calibrated=false;
        } else calibration.message="Uložená kalibrace pasivního měření PWM načtena.";
    }
    if (prefs.getBytesLength("pwmlimits")!=sizeof(protectionLimits) ||
        prefs.getBytes("pwmlimits",&protectionLimits,sizeof(protectionLimits))!=sizeof(protectionLimits) ||
        !protection::valid(protectionLimits) || protectionLimits.signature!=pwmCalibration.signature) {
        protectionLimits={}; protectionConfig.calibrated=false;
    }
    if (!protectionConfig.calibrated) { light.on=false; calibration.message="Nové měření PWM vyžaduje automatickou kalibraci."; }
    const auto resume=warm?reliability::latest(resumeSlots):nullptr;
    if (resume) resumeSequence=resume->sequence;
    if (resume && !protectionBlocks()) {
        resumeRestored=true;light=resume->light;manualOverride=scheduleConfig.enabled && resume->manual;
        overrideUntil=manualOverride?resume->until:0;
        if (scheduleConfig.onePercentOff && light.brightness==1) light.on=false;
    }
    checkpoint();
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_12));
    adc_power_acquire();
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_12));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &adcCalibration);
    // Bias the existing RC network above the ADC's dead zone at 1 % brightness.
    // A disconnected input rises to saturation and is rejected by the observer.
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(GPIO_NUM_32));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(GPIO_NUM_32));
    // GPIO37 is input-only and physically grounded by the user. Never drive it.
    uint64_t mac = ESP.getEfuseMac(); char id[24];
    snprintf(id, sizeof(id), "svetylka-%012llx", (unsigned long long)(mac & 0xffffffffffffULL));
    deviceId = id; hostname = prefs.getString("alias", DefaultAlias);
    if (!validAlias(hostname.c_str())) hostname = DefaultAlias;
    baseTopic = "wifi2mqtt/" + hostname; apName = hostname;
    // Arduino 2.x copies its hostname into esp_netif when mode() starts STA.
    WiFi.persistent(false);
    WiFi.onEvent([](WiFiEvent_t event,WiFiEventInfo_t info) {
        if (event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            ++disconnects;disconnectReason=info.wifi_sta_disconnected.reason;lastDisconnectAt=millis();statePending=true;
        } else if (event==ARDUINO_EVENT_WIFI_STA_GOT_IP) statePending=true;
    });
    beginStation();if (!*settings.ssid) startAp();
    sntp_set_time_sync_notification_cb([](struct timeval *tv){lastTimeSync=tv->tv_sec;statePending=true;});
    configTzTime(schedule::Zones[scheduleConfig.zone].rule,"pool.ntp.org","time.cloudflare.com","time.google.com");
    initWeb(); effectAt = millis();
    esp_task_wdt_reset();
    loopHeartbeat=networkHeartbeat=mqttHeartbeat=httpHeartbeat=millis();
    configASSERT(xTaskCreate(mqttTask, "mqtt", 8192, nullptr, 1, nullptr) == pdPASS);
    configASSERT(xTaskCreate(networkTask,"network",6144,nullptr,1,nullptr)==pdPASS);
    controlHeartbeat = millis();pwm::start();
    configASSERT(xTaskCreatePinnedToCore(controlTask, "light", 8192, nullptr, 2, nullptr, 1) == pdPASS);
    esp_timer_create_args_t timerArgs{};
    timerArgs.callback = monitorWatchdog; timerArgs.name = "light-watchdog";
    esp_timer_handle_t safetyTimer;
    ESP_ERROR_CHECK(esp_timer_create(&timerArgs, &safetyTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(safetyTimer, 100000));
    Serial.printf("Svetylka %s, MQTT base: %s\n", Version, baseTopic.c_str());
}
void loop() {
#ifdef RELIABILITY_TEST
    injectStall(3);
#endif
    uint32_t now = millis();static uint32_t checkpointAt=now;
    { Guard guard;
      scheduleTick();
      now = millis(); // A command may have reset effectAt while we waited for the lock.
      if (dirty && !updating && !rebootAt && now - changedAt >= 2000) {
          saveLight(); changedAt = now; statePending = true;
      }
      if (uint32_t(now-checkpointAt)>=1000 && !calibration.active && !updating) {checkpoint();checkpointAt=now;}
    }
    loopHeartbeat=millis();esp_task_wdt_reset();
    if (rebootAt && int32_t(now - rebootAt.load()) >= 0) ESP.restart();
    delay(5);
}
