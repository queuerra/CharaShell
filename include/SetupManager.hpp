#pragma once
#include "Settings.hpp"
#include <QObject>
#include <QProcess>
#include <QHash>

class SetupManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString savePath READ savePath CONSTANT)
    Q_PROPERTY(qint64 plannedBytes READ plannedBytes NOTIFY changed)
    Q_PROPERTY(qint64 receivedBytes READ receivedBytes NOTIFY changed)
    Q_PROPERTY(qint64 fileBytes READ fileBytes NOTIFY changed)
    Q_PROPERTY(qint64 fileTotal READ fileTotal NOTIFY changed)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY changed)
    Q_PROPERTY(qint64 bytesPerSecond READ bytesPerSecond NOTIFY changed)
    Settings* settings_;
    QProcess process_;
    QString status_, output_;
    QByteArray pendingOutput_;
    QHash<QString, qint64> fileProgress_;
    qint64 plannedBytes_ = 0, fileBytes_ = 0, fileTotal_ = 0, bytesPerSecond_ = 0;
    QString currentFile_;
    bool ready_ = false;
    QString root() const;
    bool applyModels();
    void readOutput();
    void plan();
public:
    explicit SetupManager(Settings* settings);
    virtual bool busy() const { return process_.state() != QProcess::NotRunning; }
    bool ready() const { return ready_; }
    QString status() const { return status_; }
    QString savePath() const { return root(); }
    qint64 plannedBytes() const { return plannedBytes_; }
    qint64 receivedBytes() const;
    qint64 fileBytes() const { return fileBytes_; }
    qint64 fileTotal() const { return fileTotal_; }
    QString currentFile() const { return currentFile_; }
    qint64 bytesPerSecond() const { return bytesPerSecond_; }
    Q_INVOKABLE virtual bool checkModels();
    // Returns true when models are already available, otherwise starts setup.
    Q_INVOKABLE bool ensureModels();
    Q_INVOKABLE virtual void download();
    Q_INVOKABLE virtual void cancel();
signals:
    void changed();
    void finished(bool success);
};
