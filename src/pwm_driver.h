#pragma once
#include <Arduino.h>
#include <driver/ledc.h>
#include <driver/pcnt.h>
#include <esp_rom_gpio.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <hal/gpio_ll.h>
#include <soc/pcnt_periph.h>
#include <atomic>
#include "light.h"

// One owner of LEDC. The carrier runs in hardware; this task only changes duty.
// No application mutex, ADC, allocation, NVS or network calls in the output task.
namespace pwm {
constexpr int InA=25, InB=26, Enable=27;
constexpr auto Speed=LEDC_HIGH_SPEED_MODE;
constexpr unsigned CounterLimit=30000, TaskPriority=6;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
struct Request {
    lights::State light;
    uint32_t epoch=0;
    bool fixed=true;
    lights::Output output{0,0};
};
struct Frame { lights::Output output{0,0}; uint32_t revision=0, changedUs=0; };
struct Stats {
    uint32_t writes=0, bridgeSwitches=0, frames=0, maxGapUs=0;
    uint32_t windows=0, edgeErrors=0;
    unsigned pulsesA=0, pulsesB=0, windowUs=0;
    unsigned errorWindowUs=0, errorA=0, errorB=0;
};
static Request requested;
static uint32_t requestRevision=0;
static Frame frame;
static Stats stats;
static bool enabled=false, inhibited=false;
static std::atomic<uint32_t> revision{0}, heartbeat{0};
static std::atomic<bool> changing{false};
inline bool same(lights::Output a,lights::Output b) {return a.a==b.a && a.b==b.b;}
inline bool active(const Request &r) {return r.fixed?(r.output.a||r.output.b):(r.light.on && r.light.brightness);}
// All gate writes use this tiny lock. An old PWM frame can never undo a safety OFF.
static void gate(bool on) {
    if (on==enabled) return;
    gpio_ll_set_level(&GPIO,GPIO_NUM_27,on);enabled=on;++stats.bridgeSwitches;
}
static void report(lights::Output out) {
    if (same(frame.output,out)) return;
    frame.output=out;frame.changedUs=uint32_t(esp_timer_get_time());
    frame.revision=revision.fetch_add(1)+1;
}
static Frame read() {
    portENTER_CRITICAL(&lock);auto copy=frame;portEXIT_CRITICAL(&lock);return copy;
}
static Stats diagnostics() {
    portENTER_CRITICAL(&lock);auto copy=stats;portEXIT_CRITICAL(&lock);return copy;
}
static bool stable(const Frame &f) {return !changing.load() && revision.load()==f.revision;}
static void submit(const Request &next) {
    portENTER_CRITICAL(&lock);
    if (next.fixed!=requested.fixed || !same(next.output,requested.output) || next.epoch!=requested.epoch ||
        next.light.on!=requested.light.on || next.light.mode!=requested.light.mode ||
        next.light.brightness!=requested.light.brightness || next.light.intervalMs!=requested.light.intervalMs) ++requestRevision;
    requested=next;
    if (!active(next)) {gate(false);report({0,0});}
    portEXIT_CRITICAL(&lock);
}
static void stop() {Request off;submit(off);}
static void emergencyStop() {
    portENTER_CRITICAL(&lock);
    inhibited=true;gate(false);report({0,0});
    portEXIT_CRITICAL(&lock);
}

static void countPulses(uint32_t nowUs) {
    static uint32_t at=0, oldRevision=0;
    static int16_t previous[2]={};
    if (uint32_t(nowUs-at)<100000) return;
    // Timestamp and counter reads must be adjacent. A flash/cache pause between
    // the task's earlier timestamp and these reads otherwise invents lost edges.
    portENTER_CRITICAL(&lock);
    nowUs=uint32_t(esp_timer_get_time());
    int16_t counts[2];pcnt_get_counter_value(PCNT_UNIT_0,&counts[0]);pcnt_get_counter_value(PCNT_UNIT_1,&counts[1]);
    const unsigned elapsed=nowUs-at;
    unsigned delta[2];
    for (unsigned i=0;i<2;++i) delta[i]=(counts[i]+CounterLimit-previous[i])%CounterLimit;
    // Ignore intentionally changing duty and intervals long enough to wrap twice.
    // Two edges of tolerance cover asynchronous boundary reads, not missing bursts.
    if (at && elapsed<2000000 && oldRevision==frame.revision) {
        const unsigned duties[]={frame.output.a,frame.output.b};
        ++stats.windows;
        for (unsigned i=0;i<2;++i) {
            const unsigned expected=duties[i]?(elapsed+500)/1000:0;
            if (abs(int(delta[i])-int(expected))>(duties[i]?2:0)) {
                ++stats.edgeErrors;stats.errorWindowUs=elapsed;stats.errorA=delta[0];stats.errorB=delta[1];
            }
        }
    }
    stats.pulsesA=delta[0];stats.pulsesB=delta[1];stats.windowUs=elapsed;
    oldRevision=frame.revision;
    portEXIT_CRITICAL(&lock);
    previous[0]=counts[0];previous[1]=counts[1];at=nowUs;
}
static void task(void *) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    lights::Output hardware{0,0};
    TickType_t wake=xTaskGetTickCount();uint32_t previousUs=uint32_t(esp_timer_get_time());
    for (;;) {
        portENTER_CRITICAL(&lock);auto r=requested;const auto version=requestRevision;portEXIT_CRITICAL(&lock);
        const auto out=r.fixed?r.output:lights::output(r.light,lights::phaseTime(millis(),r.epoch,r.light));
        if (!same(out,hardware)) {
            changing=true;
            if (!out.a && !out.b) {
                portENTER_CRITICAL(&lock);gate(false);portEXIT_CRITICAL(&lock);
                ESP_ERROR_CHECK(ledc_stop(Speed,LEDC_CHANNEL_0,0));
                ESP_ERROR_CHECK(ledc_stop(Speed,LEDC_CHANNEL_1,0));
            } else {
                // Fixed opposite half-periods: old/new duties cannot overlap.
                ESP_ERROR_CHECK(ledc_set_duty_with_hpoint(Speed,LEDC_CHANNEL_0,out.a,0));
                ESP_ERROR_CHECK(ledc_set_duty_with_hpoint(Speed,LEDC_CHANNEL_1,out.b,lights::Half));
                ESP_ERROR_CHECK(ledc_update_duty(Speed,LEDC_CHANNEL_0));
                ESP_ERROR_CHECK(ledc_update_duty(Speed,LEDC_CHANNEL_1));
            }
            hardware=out;
            portENTER_CRITICAL(&lock);
            gate(!inhibited && (out.a||out.b) && active(requested) && (enabled || version==requestRevision));
            report(enabled?out:lights::Output{0,0});++stats.writes;
            portEXIT_CRITICAL(&lock);
            changing=false;
        } else {
            // OFF and a subsequent ON can arrive between two task iterations.
            portENTER_CRITICAL(&lock);
            gate(!inhibited && (out.a||out.b) && active(requested) && (enabled || version==requestRevision));report(enabled?out:lights::Output{0,0});
            portEXIT_CRITICAL(&lock);
        }
        const uint32_t nowUs=uint32_t(esp_timer_get_time());
        portENTER_CRITICAL(&lock);
        ++stats.frames;stats.maxGapUs=std::max(stats.maxGapUs,uint32_t(nowUs-previousUs));
        portEXIT_CRITICAL(&lock);
        previousUs=nowUs;heartbeat=millis();esp_task_wdt_reset();countPulses(nowUs);
        // Absolute cadence avoids adding ADC/network processing time to each frame.
        if (uint32_t(xTaskGetTickCount()-wake)>pdMS_TO_TICKS(20)) wake=xTaskGetTickCount();
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(5));
    }
}
static void init() {
    pinMode(Enable,OUTPUT);digitalWrite(Enable,LOW);
    pinMode(InA,OUTPUT);digitalWrite(InA,LOW);pinMode(InB,OUTPUT);digitalWrite(InB,LOW);
    ledc_timer_config_t timer{};
    timer.speed_mode=Speed;timer.timer_num=LEDC_TIMER_0;
    timer.duty_resolution=LEDC_TIMER_10_BIT;timer.freq_hz=1000;timer.clk_cfg=LEDC_USE_APB_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    for (int i=0;i<2;++i) {
        ledc_channel_config_t channel{};
        channel.gpio_num=i?InB:InA;channel.speed_mode=Speed;channel.channel=ledc_channel_t(i);
        channel.timer_sel=LEDC_TIMER_0;channel.hpoint=i?lights::Half:0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
        // Route the actual output pad back to a hardware counter without changing
        // GPIO direction or pulls (pcnt_set_pin would disable the LEDC output).
        gpio_ll_input_enable(&GPIO,gpio_num_t(channel.gpio_num));
        pcnt_config_t counter{};counter.pulse_gpio_num=-1;counter.ctrl_gpio_num=-1;
        counter.unit=pcnt_unit_t(i);counter.channel=PCNT_CHANNEL_0;
        counter.pos_mode=PCNT_COUNT_INC;counter.neg_mode=PCNT_COUNT_DIS;
        counter.hctrl_mode=counter.lctrl_mode=PCNT_MODE_KEEP;
        counter.counter_h_lim=CounterLimit;counter.counter_l_lim=-1;
        ESP_ERROR_CHECK(pcnt_unit_config(&counter));
        esp_rom_gpio_connect_in_signal(channel.gpio_num,pcnt_periph_signals.groups[0].units[i].channels[0].pulse_sig,false);
        ESP_ERROR_CHECK(pcnt_counter_clear(counter.unit));ESP_ERROR_CHECK(pcnt_counter_resume(counter.unit));
    }
}
static void start() {
    heartbeat=millis();
    configASSERT(xTaskCreatePinnedToCore(task,"pwm-output",4096,nullptr,TaskPriority,nullptr,1)==pdPASS);
}
}
