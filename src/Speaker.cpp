#include "Speaker.hpp"
#include "AudioEngine.hpp"
#include "LocalRequest.hpp"
#include "InferenceScheduler.hpp"
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#ifdef CHARASHELL_NATIVE
#include "NativeInference.hpp"
#endif

Speaker::Speaker(AudioEngine* audio) : audio_(audio) {
    // Publish completion on the producer thread so even a very short final
    // block is played immediately, including standalone Speaker use.
    connect(this, &Speaker::finished, this, [this](quint64 turn) { audio_->finishPlayback(turn); }, Qt::DirectConnection);
}
Speaker::~Speaker() = default;

void Speaker::preload(quint64 generation, const QVariantMap& settings) {
    if (activeTurn_.load() != generation) return;
    try {
        if (settings.value("backend", "native").toString() == "native" && settings.value("speechEnabled", true).toBool()) {
#ifdef CHARASHELL_NATIVE
            auto lease = InferenceScheduler::gpu().acquire(true, [this, generation] { return activeTurn_.load() != generation; });
            if (!lease) return;
            if (!native_) native_ = std::make_unique<NativeSpeaker>();
            native_->load(settings, [this, generation] { return activeTurn_.load() != generation; },
                [this, generation](double value, const QString& status) {
                    if (activeTurn_.load() == generation) emit loadProgress(generation, value, status);
                });
#else
            throw std::runtime_error("Native inference is not included in this build");
#endif
        }
        if (activeTurn_.load() == generation) emit loaded(generation);
    } catch (const std::exception& e) {
        if (activeTurn_.load() == generation) emit loadFailed(generation, QString::fromUtf8(e.what()));
    }
}

void Speaker::cancel() {
    if (playbackPump_) playbackPump_->stop();
    if (reply_) { auto r = reply_.data(); reply_ = nullptr; r->disconnect(this); r->abort(); r->deleteLater(); }
    queue_.clear(); bytes_.clear(); sealed_ = false;
}
void Speaker::begin(quint64 turn, const QVariantMap& settings) { if (activeTurn_.load() && activeTurn_.load() != turn) return; cancel(); turn_ = turn; settings_ = settings; generatedSamples_ = 0; }
void Speaker::speak(quint64 turn, const QString& text) {
    if (activeTurn_.load() && activeTurn_.load() != turn) return;
    if (!settings_.value("speechEnabled", true).toBool()) return;
    if (turn != turn_ || text.trimmed().isEmpty()) return;
    if (queue_.size() >= 64) { cancel(); emit error(turn, "TTSテキストキューが上限に達しました"); return; }
    queue_.enqueue(text); if (!reply_ && !nativeBusy_) next();
}
void Speaker::seal(quint64 turn) { if (turn == turn_) { sealed_ = true; if (!reply_ && !nativeBusy_ && queue_.isEmpty()) emit finished(turn); } }
void Speaker::next() {
    if (queue_.isEmpty()) { if (sealed_) emit finished(turn_); return; }
#ifdef CHARASHELL_NATIVE
    const auto backend = settings_.value("backend", "native").toString();
    if (backend == "native") {
        const auto turn = turn_;
        if (activeTurn_.load() != turn) { queue_.clear(); return; }
        nativeBusy_ = true;
        try {
            if (!native_) native_ = std::make_unique<NativeSpeaker>();
            const auto text = queue_.dequeue();
            const auto rate = settings_.value("sampleRate", 48000).toInt();
            if (rate < 8000 || rate > 48000) throw std::runtime_error("TTSサンプルレートが不正です");
            // Keep two seconds ahead. Wait BEFORE claiming the GPU, so the LLM
            // can continue while earlier audio plays and the ring cannot fill.
            while (audio_->bufferedSamples() > size_t(rate) * 2 && activeTurn_.load() == turn)
                QThread::msleep(5);
            if (activeTurn_.load() != turn) { nativeBusy_ = false; return; }
            QElapsedTimer timer; timer.start();
            quint64 samples = 0;
            qint64 firstPcmMs = -1, previousPcmMs = -1, maximumPcmGapMs = 0, gpuWaitMs = 0;
            bool started = false;
            auto stream = [this, turn, rate, &samples, &started, &timer, &firstPcmMs, &previousPcmMs, &maximumPcmGapMs](const float* pcm, unsigned count) {
                if (activeTurn_.load() != turn) return false;
                if (samples + count > quint64(rate) * 31)
                    throw std::runtime_error("TTSの文音声がバッファ上限を超えました");
                for (unsigned i = 0; i < count; ++i) {
                    if (activeTurn_.load() != turn) return false;
                    while (!audio_->enqueue(pcm[i], turn)) {
                        if (activeTurn_.load() != turn) return false;
                        QThread::msleep(5);
                    }
                }
                samples += count; generatedSamples_ += count;
                if (count) {
                    const auto now = timer.elapsed();
                    if (firstPcmMs < 0) firstPcmMs = now;
                    if (previousPcmMs >= 0) maximumPcmGapMs = std::max(maximumPcmGapMs, now - previousPcmMs);
                    previousPcmMs = now;
                }
                if (count && !started) {
                    started = true;
                    qInfo() << "TTS first streamed PCM (ms):" << timer.elapsed();
                    emit pcmStarted(turn);
                }
                return true;
            };
            if (settings_.value("useGpu", true).toBool()) {
                auto lease = InferenceScheduler::gpu().acquire(true, [this, turn] { return activeTurn_.load() != turn; });
                gpuWaitMs = timer.elapsed();
                if (lease) native_->speak(text, settings_, stream);
            } else native_->speak(text, settings_, stream);
            if (activeTurn_.load() == turn)
                qInfo() << "TTS synthesis complete (ms):" << timer.elapsed()
                        << "audio (ms):" << samples * 1000 / rate
                        << "gpu wait (ms):" << gpuWaitMs << "first PCM (ms):" << firstPcmMs
                        << "max PCM gap (ms):" << maximumPcmGapMs;
            if (activeTurn_.load() == turn) {
                if (!samples) throw std::runtime_error("TTS returned empty audio");
                audio_->finishSentence(turn);
                emit sentenceSynthesized(turn, text, generatedSamples_);
            }
        } catch (const std::exception& e) {
            nativeBusy_ = false; queue_.clear();
            if (activeTurn_.load() == turn) emit error(turn, QString::fromUtf8(e.what()));
            return;
        }
        nativeBusy_ = false;
        if (activeTurn_.load() == turn) next();
        return;
    }
#else
    if (settings_.value("backend").toString() == "native") { queue_.clear(); emit error(turn_, "Native inference is not included in this build"); return; }
#endif
    if (!network_) {
        network_ = new QNetworkAccessManager(this); network_->setProxy(QNetworkProxy::NoProxy);
        playbackPump_ = new QTimer(this); playbackPump_->setInterval(20);
        connect(playbackPump_, &QTimer::timeout, this, &Speaker::consume);
    }
    try {
        if (settings_.value("sampleRate", 48000).toInt() != 48000)
            throw std::runtime_error("IrodoriのHTTP PCM出力は48 kHzです。TTSサンプルレートを48000に設定してください。");
        auto request = localRequest(settings_.value("ttsUrl").toString(), "/v1/audio/speech");
        const auto text = queue_.dequeue(); currentText_ = text;
        QJsonObject body{{"model", settings_.value("ttsModel").toString()}, {"voice", settings_.value("voice").toString()},
            {"input", text}, {"response_format", "pcm"},
            {"irodori", QJsonObject{{"num_steps", 4}, {"caption", settings_.value("ttsCaption").toString()},
                {"chunking_enabled", false}, {"seed", 42}}}};
        auto r = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        reply_ = r; bytes_.clear(); r->setReadBufferSize(128 * 1024);
        playbackPump_->start();
        connect(r, &QNetworkReply::readyRead, this, &Speaker::consume);
        connect(r, &QNetworkReply::finished, this, &Speaker::consume);
    } catch (const std::exception& e) { cancel(); emit error(turn_, QString::fromUtf8(e.what())); }
}
void Speaker::consume() {
    if (!reply_) return;
    if (activeTurn_.load() && activeTurn_.load() != turn_) { cancel(); return; }
    if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) { completeReply(); return; }
    const bool first = reply_->property("firstPcm").isNull();
    bytes_ += reply_->readAll();
    bool any = false;
    qsizetype consumed = 0;
    while (consumed + 2 <= bytes_.size()) {
        auto sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(bytes_.constData() + consumed));
        if (!audio_->enqueue(float(sample) / 32768.f, turn_)) break;
        consumed += 2; ++generatedSamples_;
        any = true;
    }
    bytes_.remove(0, consumed);
    if (any && first) { reply_->setProperty("firstPcm", true); emit pcmStarted(turn_); }
    completeReply();
}
void Speaker::completeReply() {
    if (!reply_ || !reply_->isFinished()) return;
    const auto turn = turn_;
    if (reply_->error() != QNetworkReply::NoError || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        const auto message = "TTS: " + reply_->errorString(); cancel(); emit error(turn, message);
    } else if (bytes_.size() == 1) {
        cancel(); emit error(turn, "TTSのPCM末尾が不完全です");
    } else if (bytes_.isEmpty()) {
        auto r = reply_.data(); reply_ = nullptr; playbackPump_->stop(); const auto text = currentText_;
        const bool any = r->property("firstPcm").toBool();
        r->deleteLater();
        if (!any) { cancel(); emit error(turn, "TTS returned empty audio"); return; }
        audio_->finishSentence(turn);
        emit sentenceSynthesized(turn, text, generatedSamples_); next();
    }
}
