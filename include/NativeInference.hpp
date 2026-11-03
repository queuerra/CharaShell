#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <memory>

// Negative progress means that the backend exposes only the current stage.
using ModelLoadProgress = std::function<void(double, const QString&)>;

class NativeThinker {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeThinker();
    ~NativeThinker();
    void load(quint64 turn, const QVariantMap& settings, const std::atomic<quint64>& activeTurn,
        const ModelLoadProgress& progress = {});
    void generate(quint64 turn, const QJsonArray& history, const QVariantMap& settings,
        const std::atomic<quint64>& activeTurn, const std::function<void(QString)>& output,
        const std::function<QJsonObject(QString)>& search = {});
};
class NativeSpeaker {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeSpeaker();
    ~NativeSpeaker();
    void load(const QVariantMap& settings, const std::function<bool()>& cancelled = {},
        const ModelLoadProgress& progress = {});
    void speak(const QString& text, const QVariantMap& settings, const std::function<bool(const float*, unsigned)>& output);
};
