#pragma once
#include <QObject>
#include <QVariantMap>
#include <memory>
#include <atomic>
class AudioEngine;
class Transcriber : public QObject {
    Q_OBJECT
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AudioEngine* audio_;
    void poll();
    bool ensureModel(const QVariantMap& settings, QString& error);
    // Low bit is the lock, remaining bits identify the input generation.
    std::atomic<quint64> inputState_{0};
public:
    explicit Transcriber(AudioEngine* audio);
    ~Transcriber();
    void start(const QVariantMap& settings);
    void preload(quint64 generation, const QVariantMap& settings);
    void stop();
    void setSuppressed(bool suppressed);
    void invalidateInput() {
        auto state = inputState_.load();
        while (!inputState_.compare_exchange_weak(state, (state + 2) | 1)) {}
    }
    bool resumeInput(quint64 expected);
    quint64 inputEpoch() const { return inputState_.load(); }
signals:
    void loaded(quint64 generation);
    void loadFailed(quint64 generation, QString error);
    void ready();
    void partialText(QString text);
    void finalText(QString text, quint64 epoch);
    void speechStarted();
    void utteranceEnded();
    void error(QString message);
};
