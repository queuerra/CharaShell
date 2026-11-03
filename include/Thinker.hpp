#pragma once
#include "SseParser.hpp"
#include "ReplyRepetitionGuard.hpp"
#include <QObject>
#include <QVariantMap>
#include <QJsonArray>
#include <QPointer>
#include <atomic>
#include <memory>
class NativeThinker;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class Thinker : public QObject {
    Q_OBJECT
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QPointer<QProcess> cloudProcess_;
    std::unique_ptr<QTemporaryDir> cloudDirectory_;
    QByteArray cloudDiagnostic_;
    QString cloudExecutable_;
    SseParser parser_;
    ReplyRepetitionGuard repetition_;
    bool completed_ = false;
    bool structured_ = false;
    bool nativeBusy_ = false;
    quint64 pendingTurn_ = 0;
    QJsonArray pendingHistory_;
    QVariantMap pendingSettings_;
    std::atomic<quint64> activeTurn_{0};
    void emitReply(quint64 turn, const QString& text);
    void finishReply(quint64 turn, const QJsonArray& history, const QVariantMap& settings);
    void beginReply(const QJsonArray& history);
#ifdef CHARASHELL_NATIVE
    std::unique_ptr<NativeThinker> native_;
#endif
public:
    explicit Thinker(QString cloudExecutable = {});
    ~Thinker();
    // Thread-safe; interrupt before queuing cleanup on the inference worker.
    void setActiveTurn(quint64 turn) { activeTurn_.store(turn); }
    void generate(quint64 turn, const QJsonArray& history, const QVariantMap& settings);
    void cancel();
    void preload(quint64 generation, const QVariantMap& settings);
signals:
    void loadProgress(quint64 generation, double progress, QString status);
    void loaded(quint64 generation);
    void loadFailed(quint64 generation, QString error);
    void searching(quint64 turn, QString query, QString url, bool openBrowser);
    void searchResults(quint64 turn, QJsonArray results);
    void fragment(quint64 turn, QString text);
    void finished(quint64 turn);
    void error(quint64 turn, QString message);
};
