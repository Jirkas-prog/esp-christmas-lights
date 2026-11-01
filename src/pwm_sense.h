#pragma once
#include <cmath>
#include <stdint.h>
#include <stddef.h>
#include <algorithm>

// Passive observer only. No GPIO, timer or LEDC writes belong in this file.
namespace sensing {
constexpr unsigned Period = 1024, Bins = 64;
struct Model {
    float a[Period], b[Period];
    void configure(unsigned dutyA, unsigned dutyB, float tau, float ratio) {
        const float on = expf(-1 / tau), off = expf(-1 / (tau * ratio));
        float wa = 0, wb = 0, decay = 1;
        for (unsigned t = 0; t < Period; ++t) {
            const bool pa = t < dutyA, pb = t >= 512 && t < 512 + dutyB;
            const float e = pa || pb ? on : off;
            wa = e * wa + (1 - e) * pa; wb = e * wb + (1 - e) * pb; decay *= e;
        }
        wa /= 1 - decay; wb /= 1 - decay;
        for (unsigned t = 0; t < Period; ++t) {
            a[t] = wa; b[t] = wb;
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
inline bool reliable(const Result &r, bool a, bool b, float relativeMargin) {
    return r.valid && (!a || (r.a>0 && r.errorA<=r.a*relativeMargin)) &&
        (!b || (r.b>0 && r.errorB<=r.b*relativeMargin));
}
struct Calibration {
    uint32_t magic = 0x50574d31;
    uint16_t tau = 120, zero = 0;
    int16_t shift = 16;
    uint16_t references[2][104] = {}; // 1..100 %, followed by duties 1..4.
    uint32_t signature = 0, checksum = 0;
    float ratio() const { return 3 - 2 * zero / 3300.f; }
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
    if (c.magic != 0x50574d31 || c.tau < 16 || c.tau > 512 || abs(c.shift) > 64 ||
        c.zero < 160 || c.zero > 600 || c.checksum != checksum(c)) return false;
    for (auto &bank : c.references) for (unsigned ref : bank)
        if (ref < c.zero + 100 || ref > 3000) return false;
    return true;
}
struct Wave {
    uint32_t sum[Bins] = {}, count[Bins] = {};
    void add(unsigned phase, unsigned mv) { sum[phase/16] += mv; ++count[phase/16]; }
};
inline bool identify(const Wave &wave, Calibration &cal, Model &model) {
    // Use an ordinary 100 % first-bank PWM waveform, not a DC test pulse.
    float bestError = 1e30f; unsigned bestTau = 0; int bestShift = 0;
    for (unsigned tau=16; tau<=512; tau+=4) {
        model.configure(500, 0, tau, cal.ratio());
        for (int shift=-48; shift<=48; shift+=4) {
            float aa=0, ay=0, yy=0; unsigned n=0;
            for (unsigned i=0; i<Bins; ++i) if (wave.count[i]) {
                float a=model.a[(i*16+8+shift)&1023], y=float(wave.sum[i])/wave.count[i]-cal.zero;
                aa+=a*a; ay+=a*y; yy+=y*y; ++n;
            }
            if (n < 60 || aa < .01) continue;
            float gain=ay/aa, error=std::max(0.f, (yy-ay*ay/aa)/n);
            if (gain < 100 || gain+cal.zero > 2450) continue;
            if (error < bestError) { bestError=error; bestTau=tau; bestShift=shift; }
        }
    }
    if (!bestTau || bestError > 25*25) return false;
    cal.tau=bestTau; cal.shift=bestShift; return true;
}
}
