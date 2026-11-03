#pragma once
#include <QObject>
#include <QString>
class SetupManager;
class ConversationManager;

// Downloads missing assets, then loads all local models before enabling input.
class StartupManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    Q_PROPERTY(QString errorString READ errorString NOTIFY changed)
public:
    enum class State { Waiting, Downloading, Loading, Ready, Error };
    StartupManager(SetupManager* setup, ConversationManager* conversation, QObject* parent = nullptr);
    State state() const { return state_; }
    bool downloading() const { return state_ == State::Downloading || (failed() && failureStage_ == State::Downloading); }
    bool loading() const { return state_ == State::Loading || (failed() && failureStage_ == State::Loading); }
    bool ready() const { return state_ == State::Ready; }
    bool failed() const { return state_ == State::Error; }
    QString errorString() const { return error_; }
    Q_INVOKABLE void begin();
    Q_INVOKABLE void retry();
signals:
    void changed();
private:
    SetupManager* setup_;
    ConversationManager* conversation_;
    State state_ = State::Waiting, failureStage_ = State::Waiting;
    QString error_;
    void prepare();
    void load();
};
