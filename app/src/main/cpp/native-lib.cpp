#include <jni.h>
#include <cstdint>
#include <oboe/Oboe.h>
#include <atomic>
#include <array>
#include <cmath>
#include <memory>
#include <vector>
#include <algorithm>
#include "signalsmith-stretch.h"

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kSampleRate = 48000;

enum Param { kPitch = 0, kLowShelf, kPresence, kDeessThr, kCompThr, kGain, kGate, kParamCount };

// Kalibre edilmemiş başlangıç değerleri (Settings.kt ile aynı).
std::array<std::atomic<float>, kParamCount> gParams = {{ -3.f, 8.f, 5.f, -30.f, -18.f, 8.f, -50.f }};

// ---- Saf DSP bloklari (cihazsiz test edilebilir) ----
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

// Ses kalinlastirma: Signalsmith Stretch (MIT) ile yuksek kaliteli perde kaydirma (~43 ms gecikme).
class Chain {
public:
    Chain() {
        hp_.highpass(kSampleRate, 90.f, 0.707f);
        sideHp_.highpass(kSampleRate, 5500.f, 0.707f);
        stretch_.configure(1, 2048, 512);
    }

    // Blok isler (n <= kMaxBlock).
    void process(const float* in, float* out, int n) {
        refresh();
        for (int i = 0; i < n; ++i) pre_[i] = pre(std::isfinite(in[i]) ? in[i] : 0.f);
        float* ip[1] = { pre_.data() };
        float* op[1] = { shifted_.data() };
        stretch_.process(ip, n, op, n);
        for (int i = 0; i < n; ++i) out[i] = post(shifted_[i]);
    }

    static constexpr int kMaxBlock = 256;

private:
    // Perde kaymasindan once: yuksek geciren, gurultu kapisi, giris kazanci.
    float pre(float x) {
        x = hp_.process(x);
        float a = std::fabs(x);
        gateEnv_ += (a - gateEnv_) * (a > gateEnv_ ? 0.05f : 0.0005f);
        float gTarget = (20.f * std::log10(gateEnv_ + 1e-9f)) > gateThr_ ? 1.f : 0.f;
        gateGain_ += (gTarget - gateGain_) * (gTarget > gateGain_ ? 0.01f : 0.002f);
        return x * gateGain_ * gain_;
    }

    // Perde kaymasindan sonra: kalinlik, netlik, de-esser, kompresor, limiter.
    float post(float x) {
        x = low_.process(x);
        x = pres_.process(x);
        float hi = sideHp_.process(x), lo = x - hi;
        float hiLevel = 20.f * std::log10(std::fabs(hi) * 4.f + 1e-9f);
        float target = hiLevel > deessThr_ ? std::pow(10.f, -(hiLevel - deessThr_) * 0.5f / 20.f) : 1.f;
        deessGain_ += (target - deessGain_) * (target < deessGain_ ? 0.05f : 0.002f);
        x = lo + hi * deessGain_;
        float lvl = 20.f * std::log10(std::fabs(x) + 1e-9f);
        float tg = lvl > compThr_ ? std::pow(10.f, -(lvl - compThr_) * (1.f - 1.f / 3.f) / 20.f) : 1.f;
        compGain_ += (tg - compGain_) * (tg < compGain_ ? 0.01f : 0.0004f);
        x *= compGain_ * kMakeup;
        return std::tanh(x);  // yumusak limiter
    }

    void refresh() {
        float p = gParams[kPitch], ls = gParams[kLowShelf], pr = gParams[kPresence];
        float g = gParams[kGain];
        gateThr_ = gParams[kGate];
        deessThr_ = gParams[kDeessThr];
        compThr_ = gParams[kCompThr];
        if (p != lastPitch_) { lastPitch_ = p; stretch_.setTransposeSemitones(p); }
        if (ls != lastLs_) { lastLs_ = ls; low_.lowShelf(kSampleRate, 200.f, ls); }
        if (pr != lastPr_) { lastPr_ = pr; pres_.peaking(kSampleRate, 3000.f, 1.f, pr); }
        if (g != lastGain_) { lastGain_ = g; gain_ = std::pow(10.f, g / 20.f); }
    }

    static constexpr float kMakeup = 1.5f;  // ~+3.5 dB kompresor sonrasi
    Biquad hp_, sideHp_, low_, pres_;
    signalsmith::stretch::SignalsmithStretch<float> stretch_;
    std::array<float, kMaxBlock> pre_{}, shifted_{};
    float deessGain_ = 1.f, compGain_ = 1.f, gain_ = 1.f, gateEnv_ = 0.f, gateGain_ = 0.f;
    float gateThr_ = -50.f, deessThr_ = -30.f, compThr_ = -18.f;
    float lastPitch_ = 1e9f, lastLs_ = 1e9f, lastPr_ = 1e9f, lastGain_ = 1e9f;
};

// ---- Tek uretici / tek tuketici halka tampon (giris -> cikis) ----
class Ring {
public:
    void clear() { w_.store(0); r_.store(0); }
    uint32_t available() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    // Uretici (giris callback'i)
    void push(const float* x, uint32_t n) {
        uint32_t w = w_.load(std::memory_order_relaxed);
        uint32_t used = w - r_.load(std::memory_order_acquire);
        uint32_t room = kSize - used;
        if (n > room) n = room;
        for (uint32_t i = 0; i < n; ++i) buf_[(w + i) & kMask] = x[i];
        w_.store(w + n, std::memory_order_release);
    }
    // Tuketici (cikis callback'i)
    void pop(float* o, uint32_t n) {
        uint32_t r = r_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) o[i] = buf_[(r + i) & kMask];
        r_.store(r + n, std::memory_order_release);
    }
    void skip(uint32_t n) { r_.store(r_.load(std::memory_order_relaxed) + n, std::memory_order_release); }
private:
    static constexpr uint32_t kSize = 16384, kMask = kSize - 1;
    std::array<float, kSize> buf_{};
    std::atomic<uint32_t> w_{0}, r_{0};
};

// ---- Oboe motoru: giris callback'i isler ve tampona yazar, cikis callback'i tampondan okur ----
class Engine {
public:
    bool start() {
        ring_.clear();
        primed_ = false;

        oboe::AudioStreamBuilder in;
        in.setDirection(oboe::Direction::Input)
          ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
          ->setSharingMode(oboe::SharingMode::Exclusive)
          ->setFormat(oboe::AudioFormat::Float)
          ->setChannelCount(1)
          ->setSampleRate(kSampleRate)
          ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
          ->setInputPreset(oboe::InputPreset::VoiceCommunication)
          ->setDataCallback(&inCb_);
        if (in.openStream(in_) != oboe::Result::OK) return false;

        oboe::AudioStreamBuilder out;
        out.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(1)
           ->setSampleRate(kSampleRate)
           ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
           ->setUsage(oboe::Usage::Media)
           ->setContentType(oboe::ContentType::Speech)
           ->setDataCallback(&outCb_);
        if (out.openStream(out_) != oboe::Result::OK) { cleanup(); return false; }

        // Tampon hedefi: iki akisin patlama boyutlarina gore, en az ~10 ms.
        uint32_t ib = std::max(in_->getFramesPerBurst(), 1);
        uint32_t ob = std::max(out_->getFramesPerBurst(), 1);
        prime_ = std::max<uint32_t>(480, 2 * ob + ib);
        maxFill_ = prime_ + 4 * std::max(ib, ob);

        if (in_->requestStart() != oboe::Result::OK || out_->requestStart() != oboe::Result::OK) {
            cleanup();
            return false;
        }
        return true;
    }

    void stop() { cleanup(); }

private:
    void onInput(const float* in, int32_t frames) {
        float tmp[Chain::kMaxBlock];
        int32_t done = 0;
        while (done < frames) {
            int32_t n = std::min<int32_t>(Chain::kMaxBlock, frames - done);
            chain_.process(in + done, tmp, n);
            ring_.push(tmp, static_cast<uint32_t>(n));
            done += n;
        }
    }

    void onOutput(float* o, int32_t frames) {
        uint32_t avail = ring_.available();
        if (!primed_) {
            if (avail < prime_) { std::fill(o, o + frames, 0.f); return; }
            primed_ = true;
        }
        if (avail > maxFill_) { ring_.skip(avail - prime_); avail = prime_; }  // gecikme birikmesin
        uint32_t want = static_cast<uint32_t>(frames);
        if (avail < want) {
            ring_.pop(o, avail);
            std::fill(o + avail, o + frames, 0.f);
            primed_ = false;  // tampon bosaldi: yeniden doldur
        } else {
            ring_.pop(o, want);
        }
    }

    struct InCb : oboe::AudioStreamDataCallback {
        Engine* e = nullptr;
        oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* d, int32_t n) override {
            e->onInput(static_cast<const float*>(d), n);
            return oboe::DataCallbackResult::Continue;
        }
    };
    struct OutCb : oboe::AudioStreamDataCallback {
        Engine* e = nullptr;
        oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* d, int32_t n) override {
            e->onOutput(static_cast<float*>(d), n);
            return oboe::DataCallbackResult::Continue;
        }
    };

    void cleanup() {
        if (in_) in_->stop();
        if (out_) out_->stop();
        if (in_) { in_->close(); in_.reset(); }
        if (out_) { out_->close(); out_.reset(); }
    }

public:
    Engine() { inCb_.e = this; outCb_.e = this; }

private:
    std::shared_ptr<oboe::AudioStream> in_, out_;
    InCb inCb_;
    OutCb outCb_;
    Ring ring_;
    Chain chain_;
    bool primed_ = false;
    uint32_t prime_ = 480, maxFill_ = 2400;
};

std::unique_ptr<Engine> gEngine;

}  // namespace

extern "C" {
JNIEXPORT jboolean JNICALL Java_com_example_voicechanger_NativeBridge_start(JNIEnv*, jobject) {
    gEngine = std::make_unique<Engine>();
    if (!gEngine->start()) { gEngine.reset(); return JNI_FALSE; }
    return JNI_TRUE;
}
JNIEXPORT void JNICALL Java_com_example_voicechanger_NativeBridge_stop(JNIEnv*, jobject) {
    if (gEngine) { gEngine->stop(); gEngine.reset(); }
}
JNIEXPORT void JNICALL Java_com_example_voicechanger_NativeBridge_setParam(JNIEnv*, jobject, jint id, jfloat v) {
    if (id >= 0 && id < kParamCount && std::isfinite(v)) gParams[id].store(v);
}
}
