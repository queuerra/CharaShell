#include "ConversationManager.hpp"
#include "Transcriber.hpp"
#include "Thinker.hpp"
#include "Speaker.hpp"
#include "System1OutputParser.hpp"
#include "ConversationSummary.hpp"
#include "ConversationPrompt.hpp"
#include <QJsonDocument>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QUuid>
#include <QCryptographicHash>
#include <QDebug>

QString ConversationManager::dataPath(const QString& name) const {
    return QDir(historyDirectory_.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : historyDirectory_).filePath(name);
}
static QString instruction(const QString& name) {
    QFile file(":/prompts/" + name + ".txt");
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing bundled dialogue prompt");
    return QString::fromUtf8(file.readAll()).trimmed();
}
ConversationManager::ConversationManager(Settings* settings, QString, QNetworkAccessManager* system2Network, QString historyDirectory)
    : settings_(settings), historyDirectory_(std::move(historyDirectory)), transcriber_(new Transcriber(&audio_)), thinker_(new Thinker),
      speaker_(new Speaker(&audio_)), system2_(this, system2Network), speech_(&audio_, speaker_, this) {
    transcriber_->moveToThread(&asrThread_); thinker_->moveToThread(&llmThread_); speaker_->moveToThread(&ttsThread_);
    connect(&asrThread_, &QThread::finished, transcriber_, &QObject::deleteLater);
    connect(&llmThread_, &QThread::finished, thinker_, &QObject::deleteLater);
    connect(&ttsThread_, &QThread::finished, speaker_, &QObject::deleteLater);
    asrThread_.start(); llmThread_.start(); ttsThread_.start();
    connect(transcriber_, &Transcriber::loaded, this, &ConversationManager::modelLoaded);
    connect(thinker_, &Thinker::loaded, this, &ConversationManager::modelLoaded);
    connect(speaker_, &Speaker::loaded, this, &ConversationManager::modelLoaded);
    connect(transcriber_, &Transcriber::loadFailed, this, &ConversationManager::modelLoadFailed);
    connect(thinker_, &Thinker::loadFailed, this, &ConversationManager::modelLoadFailed);
    connect(speaker_, &Speaker::loadFailed, this, &ConversationManager::modelLoadFailed);
    const auto progress = [this](quint64 generation, double value, const QString& status) {
        if (!modelsLoading_ || generation != turn_) return;
        currentModelProgress_ = value; modelLoadStatus_ = status; emit modelLoadChanged();
    };
    connect(thinker_, &Thinker::loadProgress, this, progress);
    connect(speaker_, &Speaker::loadProgress, this, progress);
    connect(transcriber_, &Transcriber::ready, this, [this] {
        if (!listening_) return;
        asrReady_ = true;
        if (!inputLocked()) unlockInput();
        emit changed();
    });
    connect(transcriber_, &Transcriber::partialText, this, [this](const QString& text) {
        if (listening_ && !inputLocked()) { partial_ = text; emit changed(); }
    });
    connect(transcriber_, &Transcriber::finalText, this, [this](const QString& text, quint64 epoch) {
        if (listening_ && !inputLocked() && epoch == transcriber_->inputEpoch()) submit(text);
    });
    connect(transcriber_, &Transcriber::error, this, [this](const QString& message) {
        if (!listening_) return;
        stop(); error_ = message; state_ = "ERROR"; emit changed();
    });
    connect(thinker_, &Thinker::fragment, this, [this](quint64 turn, const QString& text) {
        if (summaryJob_ && turn == summaryJob_) {
            summaryJson_ += text;
            if (summaryJson_.size() > 8192) summaryFailed(QString::fromUtf8("会話履歴の要約が上限を超えました。"));
            return;
        }
        if (turn != turn_) return;
        if (phase_ == Phase::System1) {
            if (!system1FirstText_) {
                system1FirstText_ = true;
                qInfo() << "Recognition/input to System 1 first token (ms):" << replyLatency_.elapsed();
            }
            system1Json_ += text;
            if (system1Json_.size() > 16384) fail(turn, QString::fromUtf8("System 1の出力が上限を超えました。"));
        }
    });
    connect(thinker_, &Thinker::finished, this, [this](quint64 turn) {
        if (summaryJob_ && turn == summaryJob_) { acceptSummary(); return; }
        if (turn != turn_) return;
        if (phase_ == Phase::System1) acceptSystem1();
    });
    connect(thinker_, &Thinker::error, this, [this](quint64 turn, const QString& message) {
        if (summaryJob_ && turn == summaryJob_) summaryFailed(message);
        else fail(turn, message);
    });
    connect(speaker_, &Speaker::error, this, &ConversationManager::fail);
    connect(&speech_, &SpeechCoordinator::error, this, &ConversationManager::fail);
    connect(&system2_, &System2Client::fragment, this, [this](quint64 turn, const QString& text) {
        if (turn != turn_ || !active_ || system2Done_) return;
        system2AnswerStarted_ = true;
        if (!response_.isEmpty() && response_ == system1Speech_) response_ += "\n";
        response_ += text; updateResponse(); speech_.system2(text); emit changed();
    });
    connect(&system2_, &System2Client::finished, this, [this](quint64 turn) {
        if (turn != turn_ || !active_) return;
        system2Done_ = true;
        if (!turns_.isEmpty()) {
            auto item = turns_.last().toObject(); item["system2Status"] = "completed";
            turns_.replace(turns_.size() - 1, item); saveTurns();
        }
        speech_.seal(); emit changed();
    });
    connect(&system2_, &System2Client::error, this, &ConversationManager::fail);
    connect(&speech_, &SpeechCoordinator::delivered, this,
        [this](quint64 turn, const QString& source, const QString& text, bool audible) {
            if (turn != turn_ || preview_) return;
            if (!history_.isEmpty() && history_.last().toObject().value("role") == "assistant" &&
                    history_.last().toObject().value("turnId") == turnId_ && history_.last().toObject().value("source") == source) {
                auto item = history_.last().toObject(); item["content"] = item.value("content").toString() + text;
                history_.replace(history_.size() - 1, item);
            } else history_.append(QJsonObject{{"role", "assistant"}, {"content", text}, {"source", source},
                {"turnId", turnId_}, {"delivery", audible ? "played" : "displayed"}});
            if (!turns_.isEmpty()) {
                auto item = turns_.last().toObject(); auto speech = item.value("utterances").toArray();
                speech.append(QJsonObject{{"source", source}, {"text", text}, {"delivery", audible ? "played" : "displayed"}});
                item["utterances"] = speech; turns_.replace(turns_.size() - 1, item); saveTurns();
            }
            pruneHistory(); saveHistory();
        });
    connect(&speech_, &SpeechCoordinator::finished, this, [this](quint64 turn) {
        if (turn != turn_ || !active_) return;
        speechDone_ = true; finishUtterance();
    });
    meterTimer_.setInterval(20);
    connect(&meterTimer_, &QTimer::timeout, this, [this] { speech_.poll(); emit metersChanged(); });
    meterTimer_.start();
    responseTimer_.setSingleShot(true);
    connect(&responseTimer_, &QTimer::timeout, this, [this] {
        if (active_) fail(turn_, QString::fromUtf8("応答の生成・再生が時間内に完了しませんでした。"));
    });
    connect(settings_, &Settings::changed, this, [this] {
        const auto latest = settings_->values();
        if (modelsManaged_ && modelsReady_) {
            for (const auto* key : {"backend", "asrPath", "asrLibraryPath", "llmPath", "contextSize", "gpuLayers", "useGpu", "llmKvCache",
                    "ttsPath", "ttsLibraryPath", "voicePath", "ttsCaption", "ttsDesignRevision", "speechEnabled", "language", "endpointSilence"}) {
                if (latest.value(key) != loadedSettings_.value(key)) {
                    modelsReady_ = false; emit modelLoadChanged(); emit modelsInvalidated(); return;
                }
            }
        }
        if (inputLocked()) return;
        if (listening_) {
            for (const auto* key : {"inputDeviceId", "outputDeviceId", "sampleRate", "useGpu", "asrPath", "asrLibraryPath"})
                if (latest.value(key) != currentSettings_.value(key)) { start(); return; }
        } else if (audioStarted_ && (latest.value("sampleRate") != currentSettings_.value("sampleRate") ||
                latest.value("outputDeviceId") != currentSettings_.value("outputDeviceId"))) {
            audio_.stop(); audioStarted_ = false;
        }
    });
    QFile file(dataPath("history.json"));
    if (file.open(QIODevice::ReadOnly) && file.size() <= 1024 * 1024) {
        for (const auto& value : QJsonDocument::fromJson(file.readAll()).array()) {
            const auto item = value.toObject();
            // Older versions saved unplayed drafts. Do not import those as speech.
            if (item.value("role") == "assistant" && item.value("delivery") != "played" && item.value("delivery") != "displayed") continue;
            if (!item.value("content").toString().contains(QChar::ReplacementCharacter)) history_.append(item);
        }
        pruneHistory();
        for (const auto& item : history_) messages_.append(item.toObject().toVariantMap());
    }
    QFile turns(dataPath("dialogue-turns.json"));
    if (turns.open(QIODevice::ReadOnly) && turns.size() <= 4 * 1024 * 1024) {
        turns_ = QJsonDocument::fromJson(turns.readAll()).array();
        for (qsizetype i = 0; i < turns_.size(); ++i) {
            auto item = turns_.at(i).toObject();
            if (item.value("status") == "processing" || item.value("status") == "responding") {
                item["status"] = "interrupted"; turns_.replace(i, item);
            }
        }
        saveTurns();
    }
    QFile summary(dataPath("history-summary.json"));
    if (summary.open(QIODevice::ReadOnly) && summary.size() <= 16384) {
        const auto saved = QJsonDocument::fromJson(summary.readAll()).object();
        const auto parsed = ConversationSummary::parse(QString::fromUtf8(QJsonDocument(
            QJsonObject{{"summary", saved.value("summary")}}).toJson(QJsonDocument::Compact)));
        if (parsed && saved.value("key").toString() == summaryKey(history_, settings_->values())) {
            cachedSummary_ = *parsed; cachedSummaryKey_ = saved.value("key").toString();
        }
    }
}
ConversationManager::~ConversationManager() {
    stop();
    for (auto thread : {&asrThread_, &llmThread_, &ttsThread_}) { thread->quit(); thread->wait(); }
}
void ConversationManager::refreshDevices() {
    deviceError_.clear(); audio_.enumerateDevices(inputDevices_, outputDevices_, deviceError_); emit devicesChanged();
}
void ConversationManager::preloadModels() {
    if (active_ || modelsLoading_) return;
    stop();
    modelsManaged_ = true; modelsLoading_ = true; modelsReady_ = false;
    modelLoadCompleted_ = 0; currentModelProgress_ = -1;
    modelLoadError_.clear(); error_.clear();
    phase_ = Phase::Loading; state_ = "LOADING";
    currentSettings_ = settings_->values();
    loadNextModel(); emit changed();
}
void ConversationManager::loadNextModel() {
    if (!modelsLoading_) return;
    const auto generation = turn_; const auto settings = currentSettings_;
    currentModelProgress_ = -1;
    if (modelLoadCompleted_ == 0) {
        modelLoadStatus_ = QString::fromUtf8("音声認識モデルをロードしています");
        QMetaObject::invokeMethod(transcriber_, [this, generation, settings] { transcriber_->preload(generation, settings); });
    } else if (modelLoadCompleted_ == 1) {
        modelLoadStatus_ = QString::fromUtf8("会話モデルをロードしています");
        QMetaObject::invokeMethod(thinker_, [this, generation, settings] { thinker_->preload(generation, settings); });
    } else if (modelLoadCompleted_ == 2) {
        modelLoadStatus_ = QString::fromUtf8("音声合成モデルをロードしています");
        QMetaObject::invokeMethod(speaker_, [this, generation, settings] { speaker_->preload(generation, settings); });
    } else {
        modelsLoading_ = false; modelsReady_ = true; loadedSettings_ = currentSettings_;
        currentModelProgress_ = 1; modelLoadStatus_ = QString::fromUtf8("モデルのロードが完了しました");
        phase_ = Phase::Idle; state_ = "LISTENING";
        emit modelLoadChanged(); emit changed(); emit modelsLoaded(true);
        QTimer::singleShot(0, this, &ConversationManager::prepareIdleSummary); return;
    }
    emit modelLoadChanged();
}
void ConversationManager::modelLoaded(quint64 generation) {
    if (!modelsLoading_ || generation != turn_) return;
    ++modelLoadCompleted_; loadNextModel();
}
void ConversationManager::modelLoadFailed(quint64 generation, const QString& error) {
    if (!modelsLoading_ || generation != turn_) return;
    modelsLoading_ = false; modelsReady_ = false; modelLoadError_ = error;
    modelLoadStatus_ = QString::fromUtf8("モデルのロードに失敗しました");
    phase_ = Phase::Idle; state_ = "ERROR"; error_ = error;
    emit modelLoadChanged(); emit changed(); emit modelsLoaded(false);
}
void ConversationManager::startCapture() {
    if (!listening_ || !asrReady_ || captureStarted_ || inputLocked()) return;
    QString message;
    if (!audio_.start(currentSettings_.value("sampleRate").toInt(), message, true,
            currentSettings_.value("inputDeviceId").toString(), currentSettings_.value("outputDeviceId").toString())) {
        stop(); error_ = message; state_ = "ERROR"; return;
    }
    audioStarted_ = captureStarted_ = true;
    audio_.setCaptureEnabled(false); // Enable after the worker has reset its ASR stream.
}
void ConversationManager::lockInput() {
    partial_.clear(); audio_.setCaptureEnabled(false);
    transcriber_->invalidateInput();
    QMetaObject::invokeMethod(transcriber_, [this] { transcriber_->setSuppressed(true); });
}
void ConversationManager::unlockInput() {
    startCapture();
    // Reset ASR before enabling capture; invalidate already queued finalText events.
    const auto epoch = transcriber_->inputEpoch();
    QMetaObject::invokeMethod(transcriber_, [this, epoch] {
        if (epoch != transcriber_->inputEpoch()) return;
        if (transcriber_->resumeInput(epoch)) audio_.setCaptureEnabled(true);
    });
}
void ConversationManager::cancelWorkers(bool preserveSummary) {
    system2_.cancel();
    if (!preserveSummary) QMetaObject::invokeMethod(thinker_, [this] { thinker_->cancel(); });
    QMetaObject::invokeMethod(speaker_, [this] { speaker_->cancel(); });
}
void ConversationManager::start() {
    if (inputLocked()) return;
    stop(); error_.clear(); currentSettings_ = settings_->values();
    listening_ = true; state_ = "LISTENING";
    const auto settings = currentSettings_;
    QMetaObject::invokeMethod(transcriber_, [this, settings] { transcriber_->start(settings); });
    QTimer::singleShot(0, this, &ConversationManager::prepareIdleSummary);
    emit changed();
}
void ConversationManager::stop() {
    listening_ = false; asrReady_ = captureStarted_ = false; cancel();
    QMetaObject::invokeMethod(transcriber_, [this] { transcriber_->stop(); });
    audio_.stop(); audioStarted_ = false; partial_.clear(); state_ = "LISTENING"; emit changed();
}
void ConversationManager::cancel() {
    resetTurn(false);
}
void ConversationManager::resetTurn(bool preserveSummary) {
    const bool wasLoading = modelsLoading_;
    if (wasLoading) {
        modelsLoading_ = false; modelsReady_ = false;
        modelLoadError_ = QString::fromUtf8("モデルのロードを中断しました。");
    }
    const bool wasActive = active_;
    if (!preserveSummary) {
        summaryJob_ = 0; summaryJobKey_.clear(); summaryJson_.clear();
        thinker_->setActiveTurn(turn_ + 1);
    }
    speaker_->setActiveTurn(turn_ + 1);
    speech_.cancel();
    if (wasActive && !preview_) setTurnStatus("cancelled");
    ++turn_; audio_.setTurn(turn_); cancelWorkers(preserveSummary); responseTimer_.stop();
    active_ = false; phase_ = Phase::Idle; system2Done_ = true; preview_ = false;
    system2AnswerStarted_ = false;
    partial_.clear(); state_ = "LISTENING"; unlockInput(); emit changed();
    if (wasLoading) { emit modelLoadChanged(); emit modelsLoaded(false); }
}
void ConversationManager::cancelBackgroundTasks() { cancel(); }
void ConversationManager::finishUtterance() {
    if (!active_ || !system2Done_ || !speechDone_) return;
    responseTimer_.stop();
    if (!preview_) setTurnStatus("completed");
    active_ = false; preview_ = false; phase_ = Phase::Idle; state_ = "LISTENING";
    unlockInput(); emit changed();
    QTimer::singleShot(0, this, &ConversationManager::prepareIdleSummary);
}
void ConversationManager::watchGeneration(quint64 turn) {
    if (!responseTimer_.isActive()) responseTimer_.start(300000);
    const auto phase = phase_;
    QTimer::singleShot(120000, this, [this, turn, phase] {
        if (turn == turn_ && active_ && phase_ == phase && (phase == Phase::Summarizing || phase == Phase::System1))
            fail(turn, phase == Phase::Summarizing ? QString::fromUtf8("会話履歴の要約がタイムアウトしました。") :
                QString::fromUtf8("System 1の応答がタイムアウトしました。"));
    });
}
void ConversationManager::submit(const QString& input) { submitRequest(input); }
void ConversationManager::submitAdvanced(const QString& input) { submitRequest(input); }
void ConversationManager::submitRequest(const QString& input) {
    if (inputLocked()) return;
    const auto text = input.trimmed(); if (text.isEmpty()) return;
    if (text.size() > 2000) { error_ = QString::fromUtf8("入力は2000文字以内にしてください。"); emit changed(); return; }
    const auto settings = settings_->values();
    const auto key = summaryKey(history_, settings);
    // An idle summarization may already be almost finished. Do not restart it
    // when speech recognition completes or interrupt it with turn cleanup.
    const bool preserveSummary = cachedSummaryKey_ != key && summaryJob_ && summaryJobKey_ == key;
    resetTurn(preserveSummary); error_.clear(); response_.clear(); system1Json_.clear(); searchSources_.clear(); plan_.clear();
    summary_.clear(); replyLatency_.start(); system1FirstText_ = false;
    active_ = true; preview_ = false; phase_ = Phase::System1;
    system2Done_ = true; speechDone_ = false; state_ = "PROCESSING"; lockInput();
    if (audioStarted_ && (currentSettings_.value("sampleRate") != settings.value("sampleRate") ||
            currentSettings_.value("outputDeviceId") != settings.value("outputDeviceId"))) {
        audio_.stop(); audioStarted_ = false;
    }
    currentSettings_ = settings; request_ = text; context_ = history_;
    turnId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    turns_.append(QJsonObject{{"id", turnId_}, {"user", text}, {"status", "processing"},
        {"createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"utterances", QJsonArray{}}});
    while (turns_.size() > 64) turns_.removeAt(0);
    saveTurns();
    history_.append(QJsonObject{{"role", "user"}, {"content", text}, {"turnId", turnId_}}); pruneHistory(); saveHistory();
    messages_.append(QVariantMap{{"role", "user"}, {"content", text}});
    messages_.append(QVariantMap{{"role", "assistant"}, {"content", ""}});
    while (messages_.size() > 100) messages_.removeFirst();
    if (!audioStarted_ && settings.value("speechEnabled", true).toBool()) {
        QString message;
        if (!audio_.start(settings.value("sampleRate").toInt(), message, false, {}, settings.value("outputDeviceId").toString())) {
            fail(turn_, message); return;
        }
        audioStarted_ = true;
    }
    if (context_.isEmpty()) { startSystem1(); return; }
    if (cachedSummaryKey_ == key) {
        summary_ = cachedSummary_; context_ = {};
        qInfo() << "Conversation summary: cached; no summarization on response path";
        startSystem1(); return;
    }
    phase_ = Phase::Summarizing;
    if (!preserveSummary) prepareSummary(context_, settings);
    qInfo() << "Conversation summary: waiting for" << (preserveSummary ? "existing idle job" : "uncached history");
    watchGeneration(turn_); emit changed();
}
QString ConversationManager::summaryKey(const QJsonArray& history, const QVariantMap& settings) const {
    QJsonArray records;
    for (const auto& item : history) {
        const auto obj = item.toObject();
        records.append(QJsonObject{{"role", obj.value("role")}, {"content", obj.value("content")}});
    }
    QJsonObject model;
    for (const auto* key : {"backend", "llmPath", "llmUrl", "llmModel", "contextSize", "gpuLayers", "useGpu", "llmKvCache"})
        model[key] = QJsonValue::fromVariant(settings.value(key));
    const auto bytes = QJsonDocument(QJsonObject{{"messages", records}, {"model", model},
        {"prompt", instruction("history-summary")}}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
void ConversationManager::prepareIdleSummary() {
    if (active_ || modelsLoading_ || (modelsManaged_ && !modelsReady_) || history_.isEmpty()) return;
    const auto settings = settings_->values();
    const auto key = summaryKey(history_, settings);
    if (key == cachedSummaryKey_ || (summaryJob_ && key == summaryJobKey_)) return;
    prepareSummary(history_, settings);
}
void ConversationManager::prepareSummary(const QJsonArray& history, const QVariantMap& settings) {
    // Share the resident model, but use a separate generation ID so an idle
    // result cannot be mistaken for System 1 output, a load event or speech.
    summaryJob_ = (quint64(1) << 63) | ++summarySequence_;
    summaryJobKey_ = summaryKey(history, settings); summaryJson_.clear();
    thinker_->setActiveTurn(summaryJob_);
    QMetaObject::invokeMethod(thinker_, [this] { thinker_->cancel(); });
    auto local = settings;
    local["useChatGPT"] = false; local["webSearchEnabled"] = false;
    local["conversationSummary"] = true; local["system1Structured"] = false;
    local["maxOutputTokens"] = 768;
    local["systemPrompt"] = instruction("history-summary");
    QJsonArray records;
    for (const auto& item : history) {
        const auto obj = item.toObject();
        records.append(QJsonObject{{"role", obj.value("role")}, {"content", obj.value("content")}});
    }
    const QJsonArray summaryInput{QJsonObject{{"role", "user"}, {"content", QString::fromUtf8(
        QJsonDocument(QJsonObject{{"messages", records}}).toJson(QJsonDocument::Compact))}}};
    const auto turn = summaryJob_;
    QMetaObject::invokeMethod(thinker_, [this, turn, summaryInput, local] { thinker_->generate(turn, summaryInput, local); });
    QTimer::singleShot(120000, this, [this, turn] {
        if (summaryJob_ == turn) summaryFailed(QString::fromUtf8("会話履歴の要約がタイムアウトしました。"));
    });
}
void ConversationManager::summaryFailed(const QString& message) {
    summaryJob_ = 0; summaryJobKey_.clear(); summaryJson_.clear();
    thinker_->setActiveTurn(turn_);
    QMetaObject::invokeMethod(thinker_, [this] { thinker_->cancel(); });
    if (active_ && phase_ == Phase::Summarizing) fail(turn_, message);
    else qWarning() << "Idle conversation summary failed:" << message;
}
void ConversationManager::acceptSummary() {
    const auto result = ConversationSummary::parse(summaryJson_);
    if (!result) { summaryFailed(QString::fromUtf8("会話履歴を要約できませんでした。")); return; }
    cachedSummary_ = *result; cachedSummaryKey_ = summaryJobKey_;
    summaryJob_ = 0; summaryJobKey_.clear(); summaryJson_.clear();
    QDir().mkpath(QFileInfo(dataPath("history-summary.json")).absolutePath());
    QSaveFile file(dataPath("history-summary.json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{"key", cachedSummaryKey_}, {"summary", cachedSummary_}}).toJson()); file.commit();
    }
    if (active_ && phase_ == Phase::Summarizing && cachedSummaryKey_ == summaryKey(context_, currentSettings_)) {
        summary_ = cachedSummary_; context_ = {}; startSystem1();
    }
}
void ConversationManager::startSystem1() {
    phase_ = Phase::System1;
    thinker_->setActiveTurn(turn_);
    qInfo() << "Recognition/input to System 1 start (ms):" << replyLatency_.elapsed();
    auto local = currentSettings_;
    local["useChatGPT"] = false; local["webSearchEnabled"] = false;
    local["conversationSummary"] = false;
    local["system1Structured"] = true; local["maxOutputTokens"] = 768;
    local["systemPrompt"] = ConversationPrompt::system(currentSettings_.value("systemPrompt").toString(), instruction("system1"));
    const QJsonArray context{QJsonObject{{"role", "user"}, {"content", ConversationPrompt::input(summary_, request_)}}};
    const auto turn = turn_;
    QMetaObject::invokeMethod(thinker_, [this, turn, context, local] { thinker_->generate(turn, context, local); });
    watchGeneration(turn); emit changed();
}
void ConversationManager::acceptSystem1() {
    const auto result = System1OutputParser::parse(system1Json_);
    qInfo() << "Recognition/input to System 1 response (ms), valid:" << replyLatency_.elapsed() << bool(result);
    if (!result) {
        error_ = QString::fromUtf8("System 1の応答形式が不正だったため、確認の応答に切り替えました。");
        response_ = QString::fromUtf8("うまく理解できなかったので、もう一度教えてもらえる？");
    } else response_ = result->speech;
    phase_ = Phase::Responding; state_ = "RESPONDING"; setTurnStatus("responding"); updateResponse();
    const bool think = result && result->action == "THINK";
    if (!turns_.isEmpty()) {
        auto item = turns_.last().toObject();
        item["action"] = result ? result->action : "CLARIFY";
        item["system1Status"] = result ? "completed" : "invalid_output";
        item["system2Status"] = think ? "running" : "not_required";
        if (think) item["question"] = result->question;
        turns_.replace(turns_.size() - 1, item); saveTurns();
    }
    system2Done_ = !think;
    speech_.begin(turn_, currentSettings_);
    system1Speech_ = response_;
    const auto turn = turn_;
    speech_.system1(response_);
    if (!active_ || turn != turn_) return;
    if (think) {
        plan_ = result->question;
        // Start S2 immediately after queuing S1; never rewrite its final answer.
        system2_.start(turn_, summary_, request_, response_, result->question, currentSettings_);
    } else speech_.seal();
    emit changed();
}
void ConversationManager::previewVoice() {
    if (inputLocked()) return;
    cancel(); error_.clear(); currentSettings_ = settings_->values();
    if (!currentSettings_.value("speechEnabled", true).toBool()) {
        error_ = QString::fromUtf8("音声出力を有効にしてください。"); emit changed(); return;
    }
    if (audioStarted_ && !listening_) { audio_.stop(); audioStarted_ = false; }
    if (!audioStarted_) {
        QString error;
        if (!audio_.start(currentSettings_.value("sampleRate").toInt(), error, false, {}, currentSettings_.value("outputDeviceId").toString())) {
            error_ = error; state_ = "ERROR"; emit changed(); return;
        }
        audioStarted_ = true;
    }
    active_ = preview_ = true; phase_ = Phase::VoicePreview;
    system2Done_ = true; speechDone_ = false; state_ = "RESPONDING"; lockInput();
    speech_.begin(turn_, currentSettings_);
    speech_.system1(QString::fromUtf8("こんにちは。声に合わせて、口が動きます。よろしくお願いします。"));
    speech_.seal(); watchGeneration(turn_); emit changed();
}
void ConversationManager::fail(quint64 turn, const QString& message) {
    if (turn != turn_ || !active_) return;
    const bool preview = preview_; cancel();
    if (!preview) setTurnStatus("failed");
    error_ = message; state_ = "ERROR"; emit changed();
}
void ConversationManager::updateResponse() {
    if (messages_.isEmpty()) return;
    auto message = messages_.last().toMap(); message["content"] = response_; messages_.last() = message;
}
void ConversationManager::setTurnStatus(const QString& status) {
    if (turns_.isEmpty() || turns_.last().toObject().value("id") != turnId_) return;
    auto item = turns_.last().toObject(); item["status"] = status;
    if (status == "failed" || status == "cancelled") {
        if (item.value("system2Status") == "running") item["system2Status"] = status;
        if (!item.contains("system1Status")) item["system1Status"] = status;
    }
    item["updatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    turns_.replace(turns_.size() - 1, item); saveTurns();
}
void ConversationManager::pruneHistory() {
    auto length = [this] { qsizetype n = 0; for (const auto& m : history_) n += m.toObject().value("content").toString().size(); return n; };
    while (history_.size() > 24 || length() > 3000) {
        qsizetype next = 1;
        while (next < history_.size() && history_.at(next).toObject().value("role") != "user") ++next;
        if (next == history_.size()) break; // Keep the current user and delivered reply together.
        while (next--) history_.removeAt(0);
    }
}
void ConversationManager::saveHistory() {
    QDir().mkpath(QFileInfo(dataPath("history.json")).absolutePath());
    QSaveFile file(dataPath("history.json"));
    if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(history_).toJson()); file.commit(); }
}
void ConversationManager::saveTurns() {
    QDir().mkpath(QFileInfo(dataPath("dialogue-turns.json")).absolutePath());
    QSaveFile file(dataPath("dialogue-turns.json"));
    if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(turns_).toJson()); file.commit(); }
}
void ConversationManager::clearHistory() {
    cancel(); history_ = {}; turns_ = {}; messages_.clear(); response_.clear();
    context_ = {}; summary_.clear(); summaryJson_.clear();
    cachedSummary_.clear(); cachedSummaryKey_.clear();
    QFile::remove(dataPath("history-summary.json"));
    saveHistory(); saveTurns(); emit changed();
}
