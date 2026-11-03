#pragma once
#include "SseParser.hpp"
#include <QObject>
#include <QJsonArray>
#include <QVariantMap>
#include <QPointer>
#include <QTimer>
class QNetworkAccessManager;
class QNetworkReply;

// A single cancellable Sarashina stream. No jobs, tools or subprocesses.
class System2Client : public QObject {
    Q_OBJECT
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    SseParser parser_;
    QTimer timeout_;
    quint64 turn_ = 0;
    qsizetype bytes_ = 0, characters_ = 0;
    bool completed_ = false;
    void consume();
    void fail(const QString& message);
public:
    explicit System2Client(QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    void start(quint64 turn, const QString& summary, const QString& original,
        const QString& immediate, const QString& question, const QVariantMap& settings);
    void cancel();
signals:
    void fragment(quint64 turn, QString text);
    void finished(quint64 turn);
    void error(quint64 turn, QString message);
};
