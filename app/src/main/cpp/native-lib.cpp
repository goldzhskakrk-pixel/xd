#include <jni.h>
#include <cstdint>
#include <oboe/Oboe.h>
#include <atomic>
#include <array>
#include <cmath>
#include <memory>
#include <vector>
#include <algorithm>
#include "dsp.h"

namespace {

using dsp::Chain;
using dsp::kParamCount;
using dsp::kSampleRate;

std::array<std::atomic<float>, kParamCount> gParams;
std::atomic<float> gInPeak{0.f}, gOutPeak{0.f};  // son blokun tepe degeri (0..1)


dsp::ParamSnapshot snapshot() {
    dsp::ParamSnapshot s;
    for (int i = 0; i < kParamCount; ++i) s[i] = gParams[i].load(std::memory_order_relaxed);
    return s;
}

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
        // Shared: Exclusive giris baska uygulamalarin mikrofonunu kesebilir. Unprocessed: platformun
        // AGC/gurultu bastirmasi sesi inceltip yapaylastirmasin (API 28 alti VoiceRecognition'a duser).
        in.setDirection(oboe::Direction::Input)
          ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
          ->setSharingMode(oboe::SharingMode::Shared)
          ->setFormat(oboe::AudioFormat::Float)
          ->setChannelCount(1)
          ->setSampleRate(kSampleRate)
          ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
          ->setInputPreset(oboe::InputPreset::Unprocessed)
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
        const dsp::ParamSnapshot snap = snapshot();
        float pk = 0.f;
        for (int32_t i = 0; i < frames; ++i) pk = std::max(pk, std::fabs(in[i]));
        gInPeak.store(pk, std::memory_order_relaxed);
        int32_t done = 0;
        while (done < frames) {
            int32_t n = std::min<int32_t>(Chain::kMaxBlock, frames - done);
            chain_.process(in + done, tmp, n, snap);
            ring_.push(tmp, static_cast<uint32_t>(n));
            done += n;
        }
    }

    void onOutput(float* o, int32_t frames) {
        onOutputRaw(o, frames);
        float pk = 0.f;
        for (int32_t i = 0; i < frames; ++i) pk = std::max(pk, std::fabs(o[i]));
        gOutPeak.store(pk, std::memory_order_relaxed);
        // Kendi sesini dinleme seviyesi (0 = sessiz). Isleme zinciri her durumda calisir.
        float m = std::clamp(gParams[dsp::kMonitor].load(std::memory_order_relaxed) / 100.f, 0.f, 1.f);
        if (m < 1.f) for (int32_t i = 0; i < frames; ++i) o[i] *= m;
    }

    void onOutputRaw(float* o, int32_t frames) {
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

const bool kParamsInit = [] {
    for (int i = 0; i < kParamCount; ++i) gParams[i].store(dsp::kDefaults[i]);
    return true;
}();


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
JNIEXPORT jfloat JNICALL Java_com_example_voicechanger_NativeBridge_level(JNIEnv*, jobject, jint which) {
    float v = (which == 0 ? gInPeak : gOutPeak).load(std::memory_order_relaxed);
    return dsp::toDb(v);
}
JNIEXPORT void JNICALL Java_com_example_voicechanger_NativeBridge_setParam(JNIEnv*, jobject, jint id, jfloat v) {
    if (id >= 0 && id < kParamCount && std::isfinite(v)) gParams[id].store(v);
}
}
