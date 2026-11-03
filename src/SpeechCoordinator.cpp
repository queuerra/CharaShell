#include "SpeechCoordinator.hpp"
#include "Speaker.hpp"
#include "AudioEngine.hpp"

SpeechCoordinator::SpeechCoordinator(AudioEngine* audio, Speaker* speaker, QObject* parent)
    : QObject(parent), audio_(audio), speaker_(speaker) {
    connect(speaker_, &Speaker::sentenceSynthesized, this,
        [this](quint64 turn, const QString&, quint64 end) {
            if (!active_ || turn != turn_ || synthesizing_.isEmpty()) return;
            auto item = synthesizing_.dequeue(); item.end = end;
            playback_.enqueue(item); poll();
        });
    connect(speaker_, &Speaker::finished, this, [this](quint64 turn) {
        if (active_ && turn == turn_) { synthesized_ = true; poll(); }
    });
}
void SpeechCoordinator::begin(quint64 turn, const QVariantMap& settings) {
    cancel(); turn_ = turn; active_ = true; sealed_ = synthesized_ = false;
    enabled_ = settings.value("speechEnabled", true).toBool();
    QMetaObject::invokeMethod(speaker_, [speaker = speaker_, turn, settings] { speaker->begin(turn, settings); });
}
void SpeechCoordinator::enqueue(const QString& source, const QString& text) {
    if (!active_ || text.trimmed().isEmpty()) return;
    if (!enabled_) { emit delivered(turn_, source, text, false); return; }
    if (synthesizing_.size() + playback_.size() >= 64) {
        emit error(turn_, QString::fromUtf8("音声キューが上限に達しました。")); return;
    }
    synthesizing_.enqueue({source, text, 0});
    QMetaObject::invokeMethod(speaker_, [speaker = speaker_, turn = turn_, text] { speaker->speak(turn, text); });
}
void SpeechCoordinator::system1(const QString& text) {
    // Flush the entire opening before accepting any System 2 sentence.
    const auto bytes = text.toUtf8();
    for (const auto& chunk : chunker_.push(std::string_view(bytes.constData(), bytes.size())))
        enqueue("system1", QString::fromStdString(chunk));
    enqueue("system1", QString::fromStdString(chunker_.flush()));
}
void SpeechCoordinator::system2(const QString& text) {
    const auto bytes = text.toUtf8();
    for (const auto& chunk : chunker_.push(std::string_view(bytes.constData(), bytes.size())))
        enqueue("system2", QString::fromStdString(chunk));
}
void SpeechCoordinator::seal() {
    if (!active_ || sealed_) return;
    enqueue("system2", QString::fromStdString(chunker_.flush()));
    if (!active_) return;
    sealed_ = true;
    QMetaObject::invokeMethod(speaker_, [speaker = speaker_, turn = turn_] { speaker->seal(turn); });
}
void SpeechCoordinator::poll() {
    if (!active_) return;
    // completedSamples acknowledges the preceding device callback, so handing
    // the last PCM block to the device alone does not mark it as spoken.
    while (!playback_.isEmpty() && audio_->completedSamples(turn_) >= playback_.head().end) {
        const auto item = playback_.dequeue();
        emit delivered(turn_, item.source, item.text, true);
    }
    if (sealed_ && synthesized_ && synthesizing_.isEmpty() && playback_.isEmpty() && audio_->drained()) {
        active_ = false; emit finished(turn_);
    }
}
void SpeechCoordinator::cancel() {
    // Retain only sentences known to have completed before cancellation.
    poll(); active_ = false;
    synthesizing_.clear(); playback_.clear(); chunker_.clear();
}
