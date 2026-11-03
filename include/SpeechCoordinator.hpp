#pragma once
#include "TextChunker.hpp"
#include <QObject>
#include <QQueue>
#include <QVariantMap>
class AudioEngine;
class Speaker;

// Owns speech delivery order; Speaker remains the sole PCM producer.
class SpeechCoordinator : public QObject {
    Q_OBJECT
    AudioEngine* audio_;
    Speaker* speaker_;
    struct Utterance { QString source, text; quint64 end = 0; };
    QQueue<Utterance> synthesizing_, playback_;
    TextChunker chunker_{12, 60, TextChunker::Boundary::Sentence};
    quint64 turn_ = 0;
    bool active_ = false, enabled_ = true, sealed_ = false, synthesized_ = false;
    void enqueue(const QString& source, const QString& text);
public:
    SpeechCoordinator(AudioEngine* audio, Speaker* speaker, QObject* parent = nullptr);
    void begin(quint64 turn, const QVariantMap& settings);
    void system1(const QString& text);
    void system2(const QString& fragment);
    void seal();
    void cancel();
    void poll();
signals:
    void delivered(quint64 turn, QString source, QString text, bool audible);
    void finished(quint64 turn);
    void error(quint64 turn, QString message);
};
