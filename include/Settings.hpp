#pragma once
#include <QObject>
#include <QVariantMap>

class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    QVariantMap values_;
public:
    explicit Settings(QObject* parent = nullptr);
    QVariantMap values() const { return values_; }
    Q_INVOKABLE void save(const QVariantMap& values);
signals:
    void changed();
};
