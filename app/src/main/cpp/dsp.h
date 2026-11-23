#pragma once
// Saf DSP: Oboe ve Android'e bagimli degil, host'ta test edilebilir.
#include <algorithm>
#include <array>
#include <cmath>
#include "signalsmith-stretch.h"

namespace dsp {

constexpr float kPi = 3.14159265358979f;
constexpr int kSampleRate = 48000;

enum Param {
    kPitch = 0, kLowShelf, kPresence, kDeessThr, kCompThr, kGain, kGate, kFormant, kMonitor,
    kParamCount
};
using ParamSnapshot = std::array<float, kParamCount>;

// Settings.kt ile ayni sirada ve ayni degerlerde olmali. KALIBRE EDILMEMISTIR.
constexpr ParamSnapshot kDefaults = {{ -1.f, 1.f, 5.f, -30.f, -22.f, 6.f, -55.f, 1.f, 0.f }};

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void highpass(float fs, float f, float q) {
        float w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void lowShelf(float fs, float f, float dB) {
        float A = std::pow(10.f, dB / 40), w = 2 * kPi * f / fs, c = std::cos(w);
        float al = std::sin(w) / 2 * std::sqrt(2.f), t = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c),
            A * ((A + 1) - (A - 1) * c - t),
            (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t);
    }
    void peaking(float fs, float f, float q, float dB) {
        float A = std::pow(10.f, dB / 40), w = 2 * kPi * f / fs, c = std::cos(w);
        float al = std::sin(w) / (2 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
};

inline float toDb(float x) { return 20.f * std::log10(x + 1e-9f); }
inline float fromDb(float d) { return std::pow(10.f, d / 20.f); }

// Tavana kadar dogrusal, ustunde yumusak sikisan limiter (tanh'in tum sinyali bozmasina karsi).
inline float softLimit(float x) {
    constexpr float kKnee = 0.8f, kRoom = 1.f - kKnee;
    float a = std::fabs(x);
    if (a <= kKnee) return x;
    float y = kKnee + kRoom * std::tanh((a - kKnee) / kRoom);
    return x < 0.f ? -y : y;
}

class Chain {
public:
    static constexpr int kMaxBlock = 256;

    Chain() {
        hp_.highpass(kSampleRate, 90.f, 0.707f);
        sideHp_.highpass(kSampleRate, 5500.f, 0.707f);
        stretch_.configure(1, 2048, 512);
    }

    // Blok isler (n <= kMaxBlock). Cikis monitor seviyesi UYGULANMAMIS isilmis sestir.
    void process(const float* in, float* out, int n, const ParamSnapshot& p) {
        refresh(p);
        for (int i = 0; i < n; ++i) pre_[i] = pre(std::isfinite(in[i]) ? in[i] : 0.f);
        float* ip[1] = { pre_.data() };
        float* op[1] = { shifted_.data() };
        stretch_.process(ip, n, op, n);
        for (int i = 0; i < n; ++i) out[i] = post(shifted_[i]);
    }

private:
    // Perde kaymasindan once: yuksek geciren ve gurultu kapisi (kazanc YOK: gurultuyu buyutmeyelim).
    float pre(float x) {
        x = hp_.process(x);
        float a = std::fabs(x);
        gateEnv_ += (a - gateEnv_) * (a > gateEnv_ ? 0.05f : 0.0005f);
        float db = toDb(gateEnv_);
        if (db > gateThr_) { gateOpen_ = true; holdLeft_ = kHoldSamples; }
        else if (gateOpen_ && db < gateThr_ - kHysteresisDb) {
            if (holdLeft_ > 0) --holdLeft_; else gateOpen_ = false;
        }
        float target = gateOpen_ ? 1.f : 0.f;
        gateGain_ += (target - gateGain_) * (target > gateGain_ ? 0.01f : 0.0008f);
        return x * gateGain_;
    }

    // Perde kaymasindan sonra: kalinlik, netlik, hafif de-esser, kompresor, seviye, limiter.
    float post(float x) {
        // Yavas otomatik seviye: kisik mikrofonu hedef seviyeye cikarir (en fazla +18 dB), sadece kapi aciksa.
        float ax0 = std::fabs(x);
        agcEnv_ += (ax0 - agcEnv_) * (ax0 > agcEnv_ ? 0.002f : 0.0005f);
        if (gateOpen_) {
            float want = std::clamp(kAgcTargetDb - toDb(agcEnv_), 0.f, kAgcMaxBoostDb);
            agcDb_ += (want - agcDb_) * (want < agcDb_ ? 0.0008f : 0.00008f);
        }
        x *= fromDb(agcDb_);

        x = low_.process(x);
        x = pres_.process(x);

        // Hafif de-esser: en fazla ~6 dB kisar ki Turkce s/s/c sesleri yutulmasin.
        float hi = sideHp_.process(x), lo = x - hi;
        float a = std::fabs(hi);
        deessEnv_ += (a - deessEnv_) * (a > deessEnv_ ? 0.02f : 0.002f);
        float hiDb = toDb(deessEnv_ * 4.f);
        float tg = hiDb > deessThr_ ? fromDb(-(hiDb - deessThr_) * 0.5f) : 1.f;
        tg = std::max(tg, 0.5f);
        deessGain_ += (tg - deessGain_) * (tg < deessGain_ ? 0.05f : 0.002f);
        x = lo + hi * deessGain_;

        // Kompresor: ornek bazli degil, zarf izleyici (5 ms atak, ~120 ms birakma), oran 2.5:1.
        float ax = std::fabs(x);
        compEnv_ += (ax - compEnv_) * (ax > compEnv_ ? 0.0042f : 0.00017f);
        float lvl = toDb(compEnv_);
        float cg = lvl > compThr_ ? fromDb(-(lvl - compThr_) * (1.f - 1.f / 2.5f)) : 1.f;
        x *= cg * outGain_;
        return softLimit(x);
    }

    void refresh(const ParamSnapshot& p) {
        gateThr_ = p[kGate];
        deessThr_ = p[kDeessThr];
        compThr_ = p[kCompThr];
        if (p[kPitch] != lastPitch_ || p[kFormant] != lastFormant_) {
            lastPitch_ = p[kPitch]; lastFormant_ = p[kFormant];
            stretch_.setTransposeSemitones(lastPitch_);
            // Formantlari perde kadar degil, sadece 'kFormant' kadar kaydir: dogal, insansi kalinlik.
            stretch_.setFormantSemitones(lastFormant_, true);
        }
        if (p[kLowShelf] != lastLs_) { lastLs_ = p[kLowShelf]; low_.lowShelf(kSampleRate, 180.f, lastLs_); }
        if (p[kPresence] != lastPr_) { lastPr_ = p[kPresence]; pres_.peaking(kSampleRate, 3000.f, 1.f, lastPr_); }
        if (p[kGain] != lastGain_) { lastGain_ = p[kGain]; outGain_ = fromDb(lastGain_); }
    }

    static constexpr int kHoldSamples = kSampleRate * 15 / 100;  // 150 ms
    static constexpr float kHysteresisDb = 4.f;
    static constexpr float kAgcTargetDb = -26.f, kAgcMaxBoostDb = 18.f;

    Biquad hp_, sideHp_, low_, pres_;
    signalsmith::stretch::SignalsmithStretch<float> stretch_;
    std::array<float, kMaxBlock> pre_{}, shifted_{};
    float agcEnv_ = 0.f, agcDb_ = 0.f, deessEnv_ = 0.f, deessGain_ = 1.f, compEnv_ = 0.f, outGain_ = 1.f;
    float gateEnv_ = 0.f, gateGain_ = 0.f;
    bool gateOpen_ = false;
    int holdLeft_ = 0;
    float gateThr_ = -55.f, deessThr_ = -30.f, compThr_ = -20.f;
    float lastPitch_ = 1e9f, lastFormant_ = 1e9f, lastLs_ = 1e9f, lastPr_ = 1e9f, lastGain_ = 1e9f;
};

}  // namespace dsp
