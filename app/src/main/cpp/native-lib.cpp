#include <jni.h>
#include <cstdint>
#include <oboe/Oboe.h>
#include <atomic>
#include <array>
#include <cmath>
#include <memory>
#include <vector>
#include <algorithm>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kSampleRate = 48000;

enum Param { kPitch = 0, kLowShelf, kPresence, kDeessThr, kCompThr, kGain, kParamCount };

// Kalibre edilmemiş başlangıç değerleri (Settings.kt ile aynı).
std::array<std::atomic<float>, kParamCount> gParams = {{ -1.f, 6.f, 4.f, -30.f, -18.f, 12.f }};

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

// Iki uclu gecikme hatti ile zaman-alani perde kaydirici (dusuk gecikme, ~30 ms pencere).
class PitchShifter {
public:
    float process(float x, float ratio) {
        buf_[w_] = x;
        float out = 0;
        for (int k = 0; k < 2; ++k) {
            float p = phase_ + 0.5f * k;
            if (p >= 1.f) p -= 1.f;
            float delay = p * kWindow;
            float g = 0.5f * (1.f - std::cos(2 * kPi * p));
            out += g * read(delay);
        }
        phase_ += (1.f - ratio) / kWindow;
        if (phase_ >= 1.f) phase_ -= 1.f;
        if (phase_ < 0.f) phase_ += 1.f;
        w_ = (w_ + 1) & kMask;
        return out;
    }
private:
    static constexpr int kSize = 4096, kMask = kSize - 1;
    static constexpr float kWindow = 1440.f;
    float read(float delay) const {
        float pos = static_cast<float>(w_) - delay;
        if (pos < 0) pos += kSize;
        int i0 = static_cast<int>(pos), i1 = (i0 + 1) & kMask;
        float fr = pos - i0;
        return buf_[i0 & kMask] * (1 - fr) + buf_[i1] * fr;
    }
    std::array<float, kSize> buf_{};
    int w_ = 0;
    float phase_ = 0.f;
};

class Chain {
public:
    Chain() { hp_.highpass(kSampleRate, 80.f, 0.707f); sideHp_.highpass(kSampleRate, 5500.f, 0.707f); }

    float process(float x) {
        refresh();
        x = hp_.process(x) * gain_;  // giris seviyesi: esikler anlamli olsun diye kompresorden once
        x = pitch_.process(x, ratio_);
        x = low_.process(x);
        x = pres_.process(x);
        // De-esser: 5.5 kHz ustu bandi esik asilinca kis.
        float hi = sideHp_.process(x), lo = x - hi;
        float hiLevel = 20.f * std::log10(std::fabs(hi) * 4.f + 1e-9f);
        float target = hiLevel > deessThr_ ? std::pow(10.f, -(hiLevel - deessThr_) * 0.5f / 20.f) : 1.f;
        deessGain_ += (target - deessGain_) * (target < deessGain_ ? 0.05f : 0.002f);
        x = lo + hi * deessGain_;
        // Kompresor 3:1.
        float lvl = 20.f * std::log10(std::fabs(x) + 1e-9f);
        float tg = lvl > compThr_ ? std::pow(10.f, -(lvl - compThr_) * (1.f - 1.f / 3.f) / 20.f) : 1.f;
        compGain_ += (tg - compGain_) * (tg < compGain_ ? 0.01f : 0.0004f);
        x *= compGain_ * kMakeup;
        return std::tanh(x);  // yumusak limiter
    }

private:
    void refresh() {
        float p = gParams[kPitch], ls = gParams[kLowShelf], pr = gParams[kPresence];
        float dt = gParams[kDeessThr], ct = gParams[kCompThr], g = gParams[kGain];
        if (p != lastPitch_) { lastPitch_ = p; ratio_ = std::pow(2.f, p / 12.f); }
        if (ls != lastLs_) { lastLs_ = ls; low_.lowShelf(kSampleRate, 220.f, ls); }
        if (pr != lastPr_) { lastPr_ = pr; pres_.peaking(kSampleRate, 3000.f, 1.f, pr); }
        if (g != lastGain_) { lastGain_ = g; gain_ = std::pow(10.f, g / 20.f); }
        deessThr_ = dt; compThr_ = ct;
    }
    Biquad hp_, sideHp_, low_, pres_;
    PitchShifter pitch_;
    static constexpr float kMakeup = 2.f;  // ~+6 dB kompresor sonrasi
    float ratio_ = 1.f, deessGain_ = 1.f, compGain_ = 1.f, gain_ = 1.f;
    float deessThr_ = -30.f, compThr_ = -18.f;
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
          ->setInputPreset(oboe::InputPreset::VoicePerformance)
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
        float tmp[256];
        int32_t done = 0;
        while (done < frames) {
            int32_t n = std::min<int32_t>(256, frames - done);
            for (int32_t i = 0; i < n; ++i) {
                float s = in[done + i];
                tmp[i] = std::isfinite(s) ? chain_.process(s) : 0.f;
            }
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
