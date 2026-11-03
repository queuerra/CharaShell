#pragma once
#include "RingBuffer.hpp"
#include <QObject>
#include <QVariantList>
#include <atomic>
#include <memory>
#include <cstdint>

class AudioEngine : public QObject {
    Q_OBJECT
    struct Impl;
    std::unique_ptr<Impl> impl_;
    struct Sample { float value; std::uint64_t turn; };
    RingBuffer<float, 16000 * 4 + 1> capture_;
    // One 30-second TTS sentence plus the preceding audio can be streamed
    // without blocking synthesis on device playback (host memory only).
    RingBuffer<Sample, 48000 * 34 + 1> playback_;
    std::atomic<std::uint64_t> turn_{0}, dropped_{0};
    std::atomic<std::uint64_t> sealedTurn_{0};
    std::atomic<std::uint64_t> playedSamples_{0}, completedTurn_{0}, completedSamples_{0};
    std::atomic<bool> captureEnabled_{true};
    std::atomic<std::size_t> discardBefore_{0}, flushThrough_{0};
    std::atomic<float> inputLevel_{0}, outputLevel_{0};
    // Playback callback only; outputRate_ is assigned while the device is stopped.
    int outputRate_ = 48000;
    std::uint64_t playbackTurn_ = 0, playbackTurnSamples_ = 0;
    unsigned prebufferMs_ = 150;
    bool buffering_ = true;
public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine();
    bool enumerateDevices(QVariantList& inputs, QVariantList& outputs, QString& error);
    bool start(int outputRate, QString& error, bool capture = true,
        const QString& inputDeviceId = {}, const QString& outputDeviceId = {});
    void stop();
    // Consumer: Transcriber worker. Producer: miniaudio capture callback.
    std::size_t takeCaptured(float* samples, std::size_t capacity);
    // Producer: Speaker worker. Consumer: miniaudio playback callback.
    bool enqueue(float sample, std::uint64_t turn);
    void setTurn(std::uint64_t turn);
    void finishPlayback(std::uint64_t turn) { sealedTurn_.store(turn, std::memory_order_release); }
    std::size_t bufferedSamples() const { return playback_.size(); }
    std::uint64_t playedSamples() const { return playedSamples_.load(std::memory_order_relaxed); }
    std::uint64_t completedSamples(std::uint64_t turn) const {
        if (completedTurn_.load(std::memory_order_acquire) != turn) return 0;
        const auto count = completedSamples_.load(std::memory_order_acquire);
        return completedTurn_.load(std::memory_order_acquire) == turn ? count : 0;
    }
    void finishSentence(std::uint64_t turn) {
        if (turn == turn_.load()) flushThrough_.store(playback_.writePosition(), std::memory_order_release);
    }
    void setCaptureEnabled(bool enabled) { captureEnabled_.store(enabled); }
    float inputLevel() const { return inputLevel_.load(); }
    float outputLevel() const { return outputLevel_.load(); }
    bool drained() const { return playback_.size() == 0; }
    std::uint64_t droppedSamples() const { return dropped_.load(); }
    void captureCallback(const float* samples, unsigned count);
    void playbackCallback(float* samples, unsigned count);
};
