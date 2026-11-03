#include <miniaudio.h>
#include "AudioEngine.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

struct AudioEngine::Impl {
    ma_context context{};
    ma_device capture{}, playback{};
    bool contextReady = false, captureReady = false, playbackReady = false;
    std::vector<ma_device_info> inputs, outputs;
    bool initialize(QString& error) {
        if (contextReady) return true;
#ifdef Q_OS_WIN
        // Use the same backend for enumeration and opening saved WASAPI device IDs.
        const ma_backend backend = ma_backend_wasapi;
        const auto result = ma_context_init(&backend, 1, nullptr, &context);
#else
        const auto result = ma_context_init(nullptr, 0, nullptr, &context);
#endif
        if (result != MA_SUCCESS) { error = "音声デバイスの一覧を取得できません"; return false; }
        contextReady = true;
        return true;
    }
    QString deviceKey(const ma_device_info& device) const {
#ifdef Q_OS_WIN
        return QString::fromUtf16(reinterpret_cast<const char16_t*>(device.id.wasapi));
#else
        return QString::number(context.backend) + ":" +
            QByteArray(reinterpret_cast<const char*>(&device.id), sizeof(device.id)).toHex();
#endif
    }
};
AudioEngine::AudioEngine(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { stop(); if (impl_->contextReady) ma_context_uninit(&impl_->context); }
bool AudioEngine::enumerateDevices(QVariantList& inputs, QVariantList& outputs, QString& error) {
    inputs = {QVariantMap{{"id", ""}, {"name", "システムの既定"}}};
    outputs = inputs;
    if (!impl_->initialize(error)) return false;
    ma_device_info *inputInfo = nullptr, *outputInfo = nullptr;
    ma_uint32 inputCount = 0, outputCount = 0;
    if (ma_context_get_devices(&impl_->context, &outputInfo, &outputCount, &inputInfo, &inputCount) != MA_SUCCESS) {
        error = "音声デバイスの一覧を更新できません"; return false;
    }
    impl_->inputs.clear(); impl_->outputs.clear();
    for (ma_uint32 i = 0; i < inputCount; ++i) {
        impl_->inputs.push_back(inputInfo[i]);
        inputs.append(QVariantMap{{"id", impl_->deviceKey(inputInfo[i])}, {"name", QString::fromUtf8(inputInfo[i].name)}});
    }
    for (ma_uint32 i = 0; i < outputCount; ++i) {
        impl_->outputs.push_back(outputInfo[i]);
        outputs.append(QVariantMap{{"id", impl_->deviceKey(outputInfo[i])}, {"name", QString::fromUtf8(outputInfo[i].name)}});
    }
    return true;
}
bool AudioEngine::start(int rate, QString& error, bool capture, const QString& inputDeviceId, const QString& outputDeviceId) {
    stop();
    outputRate_ = rate;
    QVariantList inputs, outputs;
    if (!enumerateDevices(inputs, outputs, error)) return false;
    const auto findDevice = [this](const std::vector<ma_device_info>& devices, const QString& id) -> const ma_device_id* {
        for (const auto& device : devices) if (impl_->deviceKey(device) == id) return &device.id;
        return nullptr;
    };
    auto in = ma_device_config_init(ma_device_type_capture);
    if (capture && !inputDeviceId.isEmpty()) {
        in.capture.pDeviceID = findDevice(impl_->inputs, inputDeviceId);
        if (!in.capture.pDeviceID) { error = "選択した入力デバイスが見つかりません。音声設定で選び直してください。"; return false; }
    }
    in.capture.format = ma_format_f32; in.capture.channels = 1; in.sampleRate = 16000;
    in.periodSizeInMilliseconds = 20; in.pUserData = this;
    in.dataCallback = [](ma_device* d, void*, const void* p, ma_uint32 n) {
        if (p) static_cast<AudioEngine*>(d->pUserData)->captureCallback(static_cast<const float*>(p), n);
    };
    auto out = ma_device_config_init(ma_device_type_playback);
    if (!outputDeviceId.isEmpty()) {
        out.playback.pDeviceID = findDevice(impl_->outputs, outputDeviceId);
        if (!out.playback.pDeviceID) { error = "選択した出力デバイスが見つかりません。音声設定で選び直してください。"; return false; }
    }
    out.playback.format = ma_format_f32; out.playback.channels = 1; out.sampleRate = rate;
    out.periodSizeInMilliseconds = 20; out.pUserData = this;
    out.dataCallback = [](ma_device* d, void* p, const void*, ma_uint32 n) {
        static_cast<AudioEngine*>(d->pUserData)->playbackCallback(static_cast<float*>(p), n);
    };
    if (capture) {
        if (ma_device_init(&impl_->context, &in, &impl_->capture) != MA_SUCCESS) { error = "マイクを初期化できません"; return false; }
        impl_->captureReady = true;
    }
    if (ma_device_init(&impl_->context, &out, &impl_->playback) != MA_SUCCESS) { error = "再生デバイスを初期化できません"; stop(); return false; }
    impl_->playbackReady = true;
    if (ma_device_start(&impl_->playback) != MA_SUCCESS || (capture && ma_device_start(&impl_->capture) != MA_SUCCESS)) {
        error = "音声デバイスを開始できません"; stop(); return false;
    }
    return true;
}
void AudioEngine::stop() {
    if (impl_->captureReady) { ma_device_uninit(&impl_->capture); impl_->captureReady = false; }
    if (impl_->playbackReady) { ma_device_uninit(&impl_->playback); impl_->playbackReady = false; }
    inputLevel_ = 0; outputLevel_ = 0;
}
void AudioEngine::captureCallback(const float* samples, unsigned count) {
    float energy = 0;
    for (unsigned i = 0; i < count; ++i) { energy += samples[i] * samples[i]; if (captureEnabled_.load(std::memory_order_relaxed) && !capture_.push(samples[i])) ++dropped_; }
    inputLevel_ = count ? std::sqrt(energy / count) : 0;
}
void AudioEngine::playbackCallback(float* samples, unsigned count) {
    completedSamples_.store(playbackTurnSamples_, std::memory_order_release);
    completedTurn_.store(playbackTurn_, std::memory_order_release);
    float energy = 0;
    unsigned played = 0;
    std::fill_n(samples, count, 0.f);
    playback_.discardBefore(discardBefore_.load(std::memory_order_acquire));
    auto turn = turn_.load();
    if (playbackTurn_ != turn) {
        playbackTurn_ = turn; playbackTurnSamples_ = 0;
        prebufferMs_ = 150;
        buffering_ = true;
    }
    const bool sealed = sealedTurn_.load(std::memory_order_acquire) == turn;
    if (buffering_) {
        const auto available = playback_.size();
        const auto minimum = std::size_t(outputRate_) * prebufferMs_ / 1000;
        if (available < minimum && !sealed && playback_.readPosition() >= flushThrough_.load(std::memory_order_acquire)) { outputLevel_ = 0; return; }
        buffering_ = false;
    }
    for (unsigned i = 0; i < count; ++i) {
        Sample s{};
        bool found = false;
        while (playback_.pop(s)) {
            if (s.turn != turn) continue;
            samples[i] = s.value; found = true; break;
        }
        if (!found) {
            if (!sealed) {
                // Refill in a single pause instead of repeatedly playing tiny
                // decoder blocks. Only enlarge the cushion after a starvation.
                prebufferMs_ = std::min(prebufferMs_ * 2, 750u);
                buffering_ = true;
            }
            break;
        }
        energy += samples[i] * samples[i];
        ++played;
    }
    playbackTurnSamples_ += played;
    playedSamples_.fetch_add(played, std::memory_order_relaxed);
    outputLevel_ = count ? std::sqrt(energy / count) : 0;
}
std::size_t AudioEngine::takeCaptured(float* samples, std::size_t capacity) {
    std::size_t n = 0; while (n < capacity && capture_.pop(samples[n])) ++n; return n;
}
bool AudioEngine::enqueue(float sample, std::uint64_t turn) {
    if (turn != turn_.load()) return true;
    return playback_.push({sample, turn});
}
void AudioEngine::setTurn(std::uint64_t turn) {
    turn_ = turn;
    discardBefore_.store(playback_.writePosition(), std::memory_order_release);
}
