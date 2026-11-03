#pragma once
#include <QObject>
#include <QVariantMap>
#include <QPointer>
#include <QQueue>
#include <atomic>
#include <memory>
class NativeSpeaker;
class AudioEngine;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class Speaker : public QObject {
    Q_OBJECT
    AudioEngine* audio_;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QTimer* playbackPump_ = nullptr;
    QQueue<QString> queue_;
    QVariantMap settings_;
    QByteArray bytes_;
    quint64 turn_ = 0, generatedSamples_ = 0;
    QString currentText_;
    bool sealed_ = false;
    bool nativeBusy_ = false;
    std::atomic<quint64> activeTurn_{0};
#ifdef CHARASHELL_NATIVE
    std::unique_ptr<NativeSpeaker> native_;
#endif
    void next();
    void consume();
    void completeReply();
public:
    explicit Speaker(AudioEngine* audio);
    ~Speaker();
    void setActiveTurn(quint64 turn) { activeTurn_.store(turn); }
    void begin(quint64 turn, const QVariantMap& settings);
    void speak(quint64 turn, const QString& text);
    void seal(quint64 turn);
    void cancel();
    void preload(quint64 generation, const QVariantMap& settings);
signals:
    void loadProgress(quint64 generation, double progress, QString status);
    void loaded(quint64 generation);
    void loadFailed(quint64 generation, QString error);
    void pcmStarted(quint64 turn);
    void sentenceSynthesized(quint64 turn, QString text, quint64 endSample);
    void finished(quint64 turn);
    void error(quint64 turn, QString message);
};
