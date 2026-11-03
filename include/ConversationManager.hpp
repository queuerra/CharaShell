#pragma once
#include "AudioEngine.hpp"
#include "Settings.hpp"
#include "System2Client.hpp"
#include "SpeechCoordinator.hpp"
#include <QThread>
#include <QTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>
#include <QElapsedTimer>
class Transcriber;
class Thinker;
class Speaker;

class ConversationManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString partial READ partial NOTIFY changed)
    Q_PROPERTY(QString response READ response NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)
    Q_PROPERTY(int backgroundTaskCount READ backgroundTaskCount NOTIFY changed)
    Q_PROPERTY(QString taskReport READ taskReport NOTIFY changed)
    Q_PROPERTY(QVariantList taskReportSources READ taskReportSources NOTIFY changed)
    Q_PROPERTY(QVariantList thoughts READ thoughts NOTIFY changed)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY changed)
    Q_PROPERTY(QVariantList searchSources READ searchSources NOTIFY changed)
    Q_PROPERTY(bool listening READ listening NOTIFY changed)
    Q_PROPERTY(bool inputLocked READ inputLocked NOTIFY changed)
    Q_PROPERTY(bool system2Waiting READ system2Waiting NOTIFY changed)
    Q_PROPERTY(QString plan READ plan NOTIFY changed)
    Q_PROPERTY(bool modelsLoading READ modelsLoading NOTIFY modelLoadChanged)
    Q_PROPERTY(bool modelsReady READ modelsReady NOTIFY modelLoadChanged)
    Q_PROPERTY(QString modelLoadStatus READ modelLoadStatus NOTIFY modelLoadChanged)
    Q_PROPERTY(QString modelLoadError READ modelLoadError NOTIFY modelLoadChanged)
    Q_PROPERTY(int modelLoadCompleted READ modelLoadCompleted NOTIFY modelLoadChanged)
    Q_PROPERTY(int modelLoadTotal READ modelLoadTotal CONSTANT)
    Q_PROPERTY(double currentModelProgress READ currentModelProgress NOTIFY modelLoadChanged)
    Q_PROPERTY(QVariantList inputDevices READ inputDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList outputDevices READ outputDevices NOTIFY devicesChanged)
    Q_PROPERTY(QString deviceError READ deviceError NOTIFY devicesChanged)
    Q_PROPERTY(float inputLevel READ inputLevel NOTIFY metersChanged)
    Q_PROPERTY(float outputLevel READ outputLevel NOTIFY metersChanged)
    Q_PROPERTY(qulonglong droppedSamples READ droppedSamples NOTIFY metersChanged)
    Settings* settings_;
    enum class Phase { Idle, Loading, Summarizing, System1, Responding, VoicePreview };
    Phase phase_ = Phase::Idle;
    QString request_, plan_, system1Json_, system1Speech_;
    QString summaryJson_, summary_, historyDirectory_;
    QString cachedSummary_, cachedSummaryKey_, summaryJobKey_;
    quint64 summaryJob_ = 0, summarySequence_ = 0;
    QElapsedTimer replyLatency_;
    bool system1FirstText_ = false;
    bool system2Done_ = true, speechDone_ = false, preview_ = false;
    bool system2AnswerStarted_ = false;
    QJsonArray context_, turns_;
    QString turnId_;
    AudioEngine audio_;
    QThread asrThread_, llmThread_, ttsThread_;
    Transcriber* transcriber_;
    Thinker* thinker_;
    Speaker* speaker_;
    System2Client system2_;
    SpeechCoordinator speech_;
    QTimer meterTimer_, responseTimer_;
    QJsonArray history_;
    QVariantList messages_;
    QVariantList searchSources_;
    QVariantList inputDevices_, outputDevices_;
    QString deviceError_;
    QString state_ = "LISTENING", partial_, response_, error_;
    quint64 turn_ = 0;
    bool listening_ = false, audioStarted_ = false, active_ = false;
    bool asrReady_ = false, captureStarted_ = false;
    QVariantMap currentSettings_;
    QVariantMap loadedSettings_;
    bool modelsManaged_ = false, modelsLoading_ = false, modelsReady_ = false;
    int modelLoadCompleted_ = 0;
    double currentModelProgress_ = -1;
    QString modelLoadStatus_, modelLoadError_;
    void loadNextModel();
    void modelLoaded(quint64 generation);
    void modelLoadFailed(quint64 generation, const QString& error);
    void fail(quint64 turn, const QString& message);
    void updateResponse();
    void saveHistory();
    void saveTurns();
    void setTurnStatus(const QString& status);
    void startCapture();
    void lockInput();
    void unlockInput();
    void acceptSystem1();
    void acceptSummary();
    QString summaryKey(const QJsonArray& history, const QVariantMap& settings) const;
    void prepareSummary(const QJsonArray& history, const QVariantMap& settings);
    void prepareIdleSummary();
    void summaryFailed(const QString& message);
    void startSystem1();
    QString dataPath(const QString& name) const;
    void pruneHistory();
    void cancelWorkers(bool preserveSummary = false);
    void resetTurn(bool preserveSummary);
    void submitRequest(const QString& text);
    void finishUtterance();
    void watchGeneration(quint64 turn);
public:
    explicit ConversationManager(Settings* settings, QString backgroundExecutable = {}, QNetworkAccessManager* system2Network = nullptr,
        QString historyDirectory = {});
    ~ConversationManager();
    QString state() const { return state_; }
    QString partial() const { return partial_; }
    QString response() const { return response_; }
    QString errorMessage() const { return error_; }
    int backgroundTaskCount() const { return !system2Done_ ? 1 : 0; }
    QString taskReport() const { return {}; }
    QVariantList taskReportSources() const { return {}; }
    QVariantList thoughts() const { return turns_.toVariantList(); }
    QVariantList messages() const { return messages_; }
    QVariantList searchSources() const { return searchSources_; }
    bool listening() const { return listening_; }
    bool inputLocked() const { return phase_ != Phase::Idle || (modelsManaged_ && !modelsReady_); }
    bool system2Waiting() const { return active_ && !system2Done_ && !system2AnswerStarted_; }
    QString plan() const { return plan_; }
    QVariantList inputDevices() const { return inputDevices_; }
    QVariantList outputDevices() const { return outputDevices_; }
    QString deviceError() const { return deviceError_; }
    bool modelsLoading() const { return modelsLoading_; }
    bool modelsReady() const { return modelsReady_; }
    QString modelLoadStatus() const { return modelLoadStatus_; }
    QString modelLoadError() const { return modelLoadError_; }
    int modelLoadCompleted() const { return modelLoadCompleted_; }
    int modelLoadTotal() const { return 3; }
    double currentModelProgress() const { return currentModelProgress_; }
    void requireStartupModels() { if (!modelsManaged_) { modelsManaged_ = true; emit changed(); } }
    Q_INVOKABLE virtual void preloadModels();
    Q_INVOKABLE void refreshDevices();
    float inputLevel() const { return audio_.inputLevel(); }
    float outputLevel() const { return audio_.outputLevel(); }
    qulonglong droppedSamples() const { return audio_.droppedSamples(); }
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void submit(const QString& text);
    Q_INVOKABLE void submitAdvanced(const QString& text);
    Q_INVOKABLE void previewVoice();
    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE void cancelBackgroundTasks();
    Q_INVOKABLE void dismissTaskReport() {}
signals:
    void modelLoadChanged();
    void modelsLoaded(bool success);
    void modelsInvalidated();
    void devicesChanged();
    void changed();
    void metersChanged();
};
