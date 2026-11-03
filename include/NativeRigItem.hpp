#pragma once
#include "NativeRig.hpp"
#include <QQuickItem>
#include <QElapsedTimer>
#include <QTimer>
#include <QFutureWatcher>

class NativeRigItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(qreal mouthOpen READ mouthOpen WRITE setMouthOpen NOTIFY mouthOpenChanged)
    Q_PROPERTY(qreal renderedMouthOpen READ renderedMouthOpen NOTIFY frameChanged)
    Q_PROPERTY(qreal blinkAmount READ blinkAmount WRITE setBlinkAmount NOTIFY blinkAmountChanged)
    Q_PROPERTY(QString psdPath READ psdPath WRITE setPsdPath NOTIFY psdPathChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY statusChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY statusChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY statusChanged)
public:
    explicit NativeRigItem(QQuickItem* parent=nullptr);
    qreal mouthOpen() const {return targetMouth_;}
    qreal renderedMouthOpen() const {return mouth_;}
    qreal blinkAmount() const {return blinkOverride_;}
    QString psdPath() const {return psdPath_;}
    bool ready() const {return error_.isEmpty()&&!model_.layers.isEmpty();}
    bool loading() const {return loading_;}
    QString errorString() const {return error_;}
    void setMouthOpen(qreal value);
    void setBlinkAmount(qreal value);
    void setPsdPath(const QString& path);
signals:
    void mouthOpenChanged();
    void frameChanged();
    void blinkAmountChanged();
    void psdPathChanged();
    void statusChanged();
protected:
    QSGNode* updatePaintNode(QSGNode*,UpdatePaintNodeData*) override;
private:
    NativeRig::Model model_;
    QString error_, psdPath_;
    bool modelChanged_ = false;
    struct LoadResult { NativeRig::Model model; QString error; };
    QFutureWatcher<LoadResult> loadWatcher_;
    quint64 requestedLoad_ = 0, runningLoad_ = 0;
    bool loading_ = true, loadRunning_ = false, loadQueued_ = false;
    void loadModel();
    void startModelLoad();
    QTimer timer_;
    QElapsedTimer clock_;
    double time_=0,last_=0,mouth_=0,targetMouth_=0,blink_=0;
    double spring_=0,velocity_=0,blinkOverride_=-1;
};
