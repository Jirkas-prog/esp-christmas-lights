#pragma once
#include <cmath>
#include <stdint.h>
#include <stddef.h>
#include <algorithm>

// Passive observer only. No GPIO, timer or LEDC writes belong in this file.
namespace sensing {
constexpr unsigned Period = 1024, Bins = 64;
constexpr uint32_t CalibrationMagic=0x50574d33;
constexpr float SupplyMv=5000, AdcMaxMv=3100;
struct Model {
    float a[Period], b[Period], retention[Period+1];
    void configure(unsigned dutyA, unsigned dutyB, float tau, float ratio) {
        const float on = expf(-1 / tau), off = expf(-1 / (tau * ratio));
        float wa = 0, wb = 0, decay = 1;
        for (unsigned t = 0; t < Period; ++t) {
            const bool pa = t < dutyA, pb = t >= 512 && t < 512 + dutyB;
            const float e = pa || pb ? on : off;
            wa = e * wa + (1 - e) * pa; wb = e * wb + (1 - e) * pb; decay *= e;
        }
        wa /= 1 - decay; wb /= 1 - decay; retention[0]=1;
        for (unsigned t = 0; t < Period; ++t) {
            a[t] = wa; b[t] = wb;
            retention[t+1]=retention[t]*((t<dutyA || (t>=512 && t<512+dutyB))?on:off);
            const bool pa = t < dutyA, pb = t >= 512 && t < 512 + dutyB;
            const float e = pa || pb ? on : off;
            wa = e * wa + (1 - e) * pa; wb = e * wb + (1 - e) * pb;
        }
    }
};
struct Result { bool valid = false; float a = 0, b = 0, rms = 0, noise = 0, errorA=0, errorB=0; unsigned samples = 0; };
struct Fit {
    double aa = 0, ab = 0, bb = 0, ay = 0, by = 0, yy = 0;
    unsigned count = 0, clipped = 0;
    struct Bin { float a=0, b=0, y=0; unsigned count=0; } bins[Bins];
    void add(float a, float b, float y, bool clip = false, unsigned phase = 0) {
        ++count; clipped += clip;
        aa += a*a; ab += a*b; bb += b*b; ay += a*y; by += b*y; yy += y*y;
        auto &bin=bins[phase/16]; bin.a+=a; bin.b+=b; bin.y+=y; ++bin.count;
    }
    Result result(unsigned minimum = 100) const {
        Result r; r.samples = count;
        if (count < minimum || clipped > count/20 || aa + bb < count * 4.) return r;
        const double det = aa * bb - ab * ab;
        if (aa < 1) r.b = by / bb;
        else if (bb < 1) r.a = ay / aa;
        else {
            if (det < aa * bb * .001) return r;
            r.a = (ay * bb - by * ab) / det; r.b = (by * aa - ay * ab) / det;
        }
        double residual = yy - 2*r.a*ay - 2*r.b*by + r.a*r.a*aa + 2*r.a*r.b*ab + r.b*r.b*bb;
        r.noise = sqrt(std::max(0., residual / count));
        if (aa<1) r.errorB=3*r.noise/sqrt(bb);
        else if (bb<1) r.errorA=3*r.noise/sqrt(aa);
        else { r.errorA=3*r.noise*sqrt(bb/det); r.errorB=3*r.noise*sqrt(aa/det); }
        // Compare phase-averaged signal to phase-averaged predictors. Raw ADC
        // noise is not a waveform fault; averaging must never mix PWM phases.
        double error=0; unsigned used=0, covered=0;
        for (const auto &bin:bins) if (bin.count) {
            float e=(bin.y-r.a*bin.a-r.b*bin.b)/bin.count;
            error+=e*e*bin.count; used+=bin.count; ++covered;
        }
        r.rms = used ? sqrt(error/used) : 0;
        r.valid = std::isfinite(r.a) && std::isfinite(r.b) && covered>=60 && r.rms<=45 && r.noise<=200;
        return r;
    }
    bool healthy(const Result &r, unsigned percent) const {
        if (!r.valid) return false;
        const float low = percent / 100.f, high = 2 - low;
        // The stronger polarity still detects a wire-to-wire short during a
        // fade endpoint where the other polarity has almost no measurable energy.
        return (aa < bb * .05 || (r.a >= low && r.a <= high)) &&
               (bb < aa * .05 || (r.b >= low && r.b <= high));
    }
};
// Eliminate the unknown capacitor voltage at the start of each short capture.
// The homogeneous RC response z is a nuisance coefficient, not a fault signal.
// Schur projection preserves both polarity gains while allowing a fading PWM
// envelope to change between captures, without waiting for DC settling.
struct Batch {
    Fit data{};double az=0,bz=0,yz=0,zz=0;float zBins[Bins]={};
    void add(float a,float b,float y,float z,bool clip,unsigned phase) {
        data.add(a,b,y,clip,phase);az+=double(a)*z;bz+=double(b)*z;yz+=double(y)*z;zz+=double(z)*z;
        zBins[phase/16]+=z;
    }
    void merge(Fit &out) const {
        if (data.count<8 || zz<1e-12) return;
        out.aa+=std::max(0.,data.aa-az*az/zz);out.ab+=data.ab-az*bz/zz;
        out.bb+=std::max(0.,data.bb-bz*bz/zz);out.ay+=data.ay-az*yz/zz;
        out.by+=data.by-bz*yz/zz;out.yy+=std::max(0.,data.yy-yz*yz/zz);
        out.count+=data.count-1;out.clipped+=data.clipped;
        for (unsigned i=0;i<Bins;++i) {
            auto &dest=out.bins[i];const auto &source=data.bins[i];
            dest.a+=source.a-zBins[i]*az/zz;dest.b+=source.b-zBins[i]*bz/zz;
            dest.y+=source.y-zBins[i]*yz/zz;dest.count+=source.count;
        }
    }
};
inline bool reliable(const Result &r, bool a, bool b, float relativeMargin) {
    return r.valid && (!a || (r.a>0 && r.errorA<=r.a*relativeMargin)) &&
        (!b || (r.b>0 && r.errorB<=r.b*relativeMargin));
}
struct Calibration {
    uint32_t magic = CalibrationMagic;
    uint16_t tau = 120, zero = 0;
    int16_t shift = 16;
    uint8_t positiveBank=0, reserved=0;
    uint16_t references[2][104] = {}; // 1..100 %, followed by duties 1..4.
    uint32_t signature = 0, checksum = 0;
    // One 3k input resistor and one 3k shunt, including the internal pull-up.
    float ratio() const { return 2 - zero / 3300.f; }
    float low() const { return zero/ratio(); }
    float high() const { return low()+SupplyMv*(1-1/ratio()); }
    bool positive(unsigned bank) const { return bank==positiveBank; }
    float shortLevel(unsigned bank) const { return positive(bank)?low():high(); }
    float distance(unsigned bank,float mv) const { return positive(bank)?mv-low():high()-mv; }
    float contrast(unsigned bank,unsigned duty) const { return distance(bank,reference(bank,duty)); }
    float signedContrast(unsigned bank,unsigned duty) const { return (positive(bank)?1:-1)*contrast(bank,duty); }
    float threshold(unsigned bank,unsigned duty,unsigned percent) const {
        return shortLevel(bank)+signedContrast(bank,duty)*percent/100.f;
    }
    float reference(unsigned bank, unsigned duty) const {
        if (!duty) return zero;
        if (duty < 5) return references[bank][99 + duty];
        unsigned lo = duty / 5 - 1, remainder = duty % 5;
        if (!remainder || lo >= 99) return references[bank][lo];
        return references[bank][lo] + (int(references[bank][lo+1])-int(references[bank][lo])) * float(remainder) / 5.f;
    }
};
inline uint32_t checksum(const Calibration &c) {
    const auto *p = reinterpret_cast<const uint8_t *>(&c); uint32_t hash = 2166136261u;
    for (size_t i=0; i<offsetof(Calibration,checksum); ++i) hash=(hash^p[i])*16777619u;
    return hash;
}
inline bool valid(const Calibration &c) {
    if (c.magic != CalibrationMagic || c.tau < 16 || c.tau > 64000 || abs(c.shift) > 64 ||
        c.zero < 160 || c.zero > 600 || c.positiveBank>1 || c.reserved || c.checksum != checksum(c)) return false;
    for (unsigned bank=0;bank<2;++bank) for (unsigned ref:c.references[bank])
        if (ref>AdcMaxMv || c.distance(bank,ref)<99) return false;
    return true;
}
// Subtract the phase-dependent SHORT waveform, then fit the remaining LED
// voltage. Both gains are 1 when healthy and approach 0 on a short, although
// GPIO32 falls in the positive phase and rises in the negative phase.
struct Sample { float a,b,y; };
struct Basis {
    float zero,shortA,shortB,voltageA,voltageB;
    Basis(const Calibration &cal,unsigned dutyA,unsigned dutyB):zero(cal.zero),
        shortA(cal.shortLevel(0)-cal.zero),shortB(cal.shortLevel(1)-cal.zero),
        voltageA(cal.signedContrast(0,dutyA)),voltageB(cal.signedContrast(1,dutyB)) {}
    Sample observe(const Model &model,unsigned phase,float mv) const {
        const float wa=model.a[phase],wb=model.b[phase];
        return {wa*voltageA,wb*voltageB,mv-zero-wa*shortA-wb*shortB};
    }
};
inline Result precision(const Result &raw,const Calibration &cal,float forward,float reverse) {
    Result out=raw;const float a=cal.distance(0,forward)/1000.f,b=cal.distance(1,reverse)/1000.f;
    out.a=out.b=1;
    out.errorA=a>0?raw.errorA/a:1e9f;out.errorB=b>0?raw.errorB/b:1e9f;
    return out;
}
struct Wave {
    uint32_t sum[Bins] = {}, count[Bins] = {};
    void add(unsigned phase, unsigned mv) { sum[phase/16] += mv; ++count[phase/16]; }
};
inline bool identify(const Wave &wave, Calibration &cal, Model &model) {
    // Ordinary PWM on the configured positive bank; no polarity guessing.
    float values[Bins];unsigned covered=0;
    for (unsigned i=0;i<Bins;++i) {values[i]=wave.count[i]?float(wave.sum[i])/wave.count[i]-cal.zero:0;covered+=wave.count[i]!=0;}
    if (covered<60) return false;
    float bestError=1e30f;unsigned bestTau=0;int bestShift=0;
    for (unsigned tau=16;tau<=64000;tau+=std::max(4u,tau/64)) {
        // Closed-form periodic start, then one scalar recurrence. Reuse model.a
        // as scratch to keep the identification below the control watchdog.
        const double on=exp(-500./tau),off=exp(-524./(tau*cal.ratio()));
        float w=(1-on)*off/(1-on*off),stepOn=expf(-4.f/tau),stepOff=expf(-4.f/(tau*cal.ratio()));
        for (unsigned t=0;t<Period;t+=4) {model.a[t]=w;w=t<500?stepOn*w+1-stepOn:stepOff*w;}
        for (int shift=-48;shift<=48;shift+=4) {
            float aa=0,ay=0;
            for (unsigned i=0;i<Bins;++i) if (wave.count[i]) {
                float a=model.a[(i*16+8+shift-512*cal.positiveBank)&1023];
                aa+=a*a;ay+=a*values[i];
            }
            if (aa<.01) continue;
            float gain=ay/aa,error=0;
            if (gain<100 || gain+cal.zero>AdcMaxMv) continue;
            // Direct residuals avoid cancellation of large DC terms for slow RC.
            for (unsigned i=0;i<Bins;++i) if (wave.count[i]) {
                float e=values[i]-gain*model.a[(i*16+8+shift-512*cal.positiveBank)&1023];
                error+=e*e;
            }
            error/=covered;
            if (error<bestError) {bestError=error;bestTau=tau;bestShift=shift;}
        }
    }
    if (!bestTau || bestError > 25*25) return false;
    cal.tau=bestTau; cal.shift=bestShift; return true;
}
}
