#include "ConversationManager.hpp"
#include "SetupManager.hpp"
#include "StartupManager.hpp"
#include "AssetPath.hpp"
#include "NativeRigItem.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QPainter>
#include <QFontDatabase>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <algorithm>

static void saveFrame(QQuickWindow* window, const QString& path) {
    const auto grab = window->contentItem()->grabToImage();
    if (!grab) return;
    QObject::connect(grab.data(), &QQuickItemGrabResult::ready, window, [grab, path, color = window->color()] {
        QImage image(grab->image().size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(color);
        { QPainter painter(&image); painter.drawImage(0, 0, grab->image()); }
        image.save(path);
        QObject::disconnect(grab.data(), nullptr, nullptr, nullptr);
    });
}

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/meiryo.ttc");
#endif
    QCoreApplication::setOrganizationName("CharaShell");
    QCoreApplication::setApplicationName("CharaShell");
    QQuickStyle::setStyle("Material");
    qmlRegisterType<NativeRigItem>("CharaShell.Rig", 1, 0, "NativeRigItem");
    Settings settings;
    ConversationManager conversation(&settings);
    SetupManager setup(&settings);
    StartupManager startup(&setup, &conversation);
    QQmlApplicationEngine engine;
    const bool diagnosticMode = app.arguments().contains("--loading-check") || app.arguments().contains("--portrait-check") ||
        app.arguments().contains("--room-check") || app.arguments().contains("--avatar-check") ||
        app.arguments().contains("--rig-check") || app.arguments().contains("--avatar-preview") ||
        app.arguments().contains("--smoke-test");
    engine.rootContext()->setContextProperty("diagnosticMode", diagnosticMode);
    engine.rootContext()->setContextProperty("appSettings", &settings);
    engine.rootContext()->setContextProperty("conversation", &conversation);
    engine.rootContext()->setContextProperty("setup", &setup);
    engine.rootContext()->setContextProperty("startup", &startup);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("CharaShell", "Main");
    if (app.arguments().contains("--loading-check")) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
        if (!window) return 2;
        window->setProperty("loadingPreview", true);
        QTimer::singleShot(300, &app, [window] {
            saveFrame(window, QCoreApplication::applicationDirPath() + "/loading-check.png");
        });
        QTimer::singleShot(650, &app, [window] {
            saveFrame(window, QCoreApplication::applicationDirPath() + "/loading-check-rotated.png");
        });
        QTimer::singleShot(1000, &app, [&app, window] {
            auto* spinner = window->findChild<QObject*>("modelLoadSpinner");
            app.exit(spinner && spinner->property("angle").toReal() > 0 ? 0 : 2);
        });
    }
    if (app.arguments().contains("--startup-check") || app.arguments().contains("--setup-check")) {
        auto events = std::make_shared<QJsonArray>();
        auto ticks = std::make_shared<int>(0);
        auto postLoadTicks = std::make_shared<int>(0);
        auto avatarReadyTicks = std::make_shared<int>(0);
        auto maxPostLoadGap = std::make_shared<qint64>(0);
        auto clock = std::make_shared<QElapsedTimer>();clock->start();
        auto lastTick = std::make_shared<qint64>(clock->elapsed());
        auto finished = std::make_shared<bool>(false);
        auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        auto* avatar = window ? window->findChild<QObject*>("avatarView") : nullptr;
        auto timer = new QTimer(&app); timer->setInterval(50);
        QObject::connect(timer, &QTimer::timeout, &app, [&, ticks, postLoadTicks, avatarReadyTicks, avatar, maxPostLoadGap, clock, lastTick] {
            const auto now=clock->elapsed();
            if(startup.ready()) {++*postLoadTicks;*maxPostLoadGap=std::max(*maxPostLoadGap,now-*lastTick);}
            if(startup.ready()&&avatar&&avatar->property("ready").toBool())++*avatarReadyTicks;
            *lastTick=now;++*ticks;
        }); timer->start();
        const auto record = [&, events, ticks] {
            events->append(QJsonObject{{"completed", conversation.modelLoadCompleted()}, {"progress", conversation.currentModelProgress()},
                {"status", conversation.modelLoadStatus()}, {"uiTicks", *ticks}});
        };
        QObject::connect(&conversation, &ConversationManager::modelLoadChanged, &app, record); record();
        const auto finish = [&, events, ticks, postLoadTicks, avatarReadyTicks, maxPostLoadGap, finished, window, avatar] {
            if(*finished)return;
            if (!startup.ready() && !startup.failed()) return;
            // Readiness used to end this check before the avatar's constructor
            // blocked the GUI. Cover the transition and continued event delivery.
            const bool avatarReady=avatar&&avatar->property("ready").toBool();
            const bool avatarFailed=avatar&&avatar->property("failed").toBool();
            if(startup.ready()&&!avatarFailed&&(!avatarReady||*avatarReadyTicks<20))return;
            *finished=true;
            const bool success=startup.ready()&&avatarReady&&*maxPostLoadGap<1000;
            QFile result(QCoreApplication::applicationDirPath() + "/startup-check.json");
            if (result.open(QIODevice::WriteOnly)) result.write(QJsonDocument(QJsonObject{{"ready", startup.ready()},
                {"avatarReady",avatarReady},{"postLoadUiTicks",*postLoadTicks},{"avatarReadyUiTicks",*avatarReadyTicks},
                {"maxPostLoadGuiGapMs",*maxPostLoadGap},
                {"error", startup.errorString()}, {"uiTicks", *ticks}, {"events", *events}}).toJson());
            if(window&&avatarReady)saveFrame(window,QCoreApplication::applicationDirPath()+"/startup-ready.png");
            QTimer::singleShot(250, &app, [&app, success] { app.exit(success ? 0 : 2); });
        };
        QObject::connect(&startup, &StartupManager::changed, &app, finish); finish();
        QObject::connect(timer, &QTimer::timeout, &app, finish);
        QTimer::singleShot(200, &app, [&] {
            auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (window) saveFrame(window, QCoreApplication::applicationDirPath() + "/startup-progress.png");
        });
        QTimer::singleShot(180000, &app, [&app] { qCritical() << "Startup timed out"; app.exit(3); });
    }
    if (app.arguments().contains("--portrait-check") || app.arguments().contains("--room-check")) {
        const bool showControls = app.arguments().contains("--show-controls");
        auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        auto* avatar = window ? window->findChild<QObject*>("avatarView") : nullptr;
        auto* poll = new QTimer(&app);poll->setInterval(50);
        auto clock = std::make_shared<QElapsedTimer>();clock->start();
        QObject::connect(poll,&QTimer::timeout,&app,
            [&,window,avatar,poll,clock,showControls,last=qint64(0),maxGap=qint64(0),ticks=0,readyTicks=0]() mutable {
            auto now=clock->elapsed();maxGap=std::max(maxGap,now-last);last=now;++ticks;
            if(!window||!avatar||avatar->property("failed").toBool()) {
                qCritical()<<"Portrait avatar failed";app.exit(2);return;
            }
            if(!avatar->property("ready").toBool())return;
            if(++readyTicks<20)return;
            poll->stop();
            window->setProperty("controlsVisible", showControls);
            saveFrame(window, QCoreApplication::applicationDirPath() + "/portrait-check.png");
            QFile result(QCoreApplication::applicationDirPath()+"/portrait-check.json");
            if(result.open(QIODevice::WriteOnly))result.write(QJsonDocument(QJsonObject{
                {"avatarReady",true},{"uiTicks",ticks},{"avatarReadyUiTicks",readyTicks},{"maxGuiGapMs",maxGap}}).toJson());
            qInfo()<<"Portrait avatar loaded; UI ticks:"<<ticks<<"max GUI gap ms:"<<maxGap;
            QTimer::singleShot(500,&app,[&app,success=maxGap<1000]{app.exit(success?0:3);});
        });
        poll->start();
        QTimer::singleShot(90000,&app,[&app]{qCritical()<<"Portrait check timed out";app.exit(4);});
    }
    if (app.arguments().contains("--rig-check")) {
        QTimer::singleShot(3400, &app, [&] {
            if (!engine.rootObjects().isEmpty())
                if (auto* avatar = engine.rootObjects().first()->findChild<QObject*>("avatarView"))
                    avatar->setProperty("diagnosticBlinkAmount", 1.0);
        });
        QTimer::singleShot(3720, &app, [&] {
            auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            const QString output = QCoreApplication::applicationDirPath() + "/rig-check";
            QDir().mkpath(output);
            if(window)saveFrame(window, output + "/blink.png");
        });
        QTimer::singleShot(4100, &app, [&] {
            if (!engine.rootObjects().isEmpty())
                if (auto* avatar = engine.rootObjects().first()->findChild<QObject*>("avatarView"))
                    avatar->setProperty("diagnosticBlinkAmount", -1.0);
        });
        QTimer::singleShot(5000, &app, [&] {
            auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            auto* avatar = window ? window->findChild<QObject*>("avatarView") : nullptr;
            if (!window || !avatar || !avatar->property("ready").toBool()) {
                qCritical() << "Native PSD rig did not load";
                app.exit(2); return;
            }
            const QString output = QCoreApplication::applicationDirPath() + "/rig-check";
            QDir().mkpath(output);
            window->setProperty("controlsVisible", false);
            saveFrame(window, output + "/idle.png");
            avatar->setProperty("outputLevel", 0.1);
            avatar->setProperty("diagnosticMouthOpen", 1.0);
            {
                QFile levels(output + "/levels.txt");
                if (levels.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    levels.write("outputLevel=" + avatar->property("outputLevel").toString().toUtf8()
                                 + " mouthOpen=" + avatar->property("mouthOpen").toString().toUtf8() + "\n");
            }
            QTimer::singleShot(450, &app, [&, window, avatar, output] {
                QFile levels(output + "/levels.txt");
                if (levels.open(QIODevice::Append))
                    levels.write("rendered-open=" + avatar->property("renderedMouthOpen").toString().toUtf8() + "\n");
                if (avatar->property("renderedMouthOpen").toReal() < 0.7) { app.exit(3); return; }
                saveFrame(window, output + "/mouth-open.png");
                avatar->setProperty("outputLevel", 0.0);
                avatar->setProperty("diagnosticMouthOpen", 0.0);
                QTimer::singleShot(650, &app, [&, window, avatar, output] {
                    QFile levels(output + "/levels.txt");
                    if (levels.open(QIODevice::Append))
                        levels.write("rendered-closed=" + avatar->property("renderedMouthOpen").toString().toUtf8() + "\n");
                    if (avatar->property("renderedMouthOpen").toReal() > 0.05) { app.exit(4); return; }
                    saveFrame(window, output + "/mouth-closed.png");
                    QTimer::singleShot(400, &app, &QCoreApplication::quit);
                });
            });
        });
    }
    if (app.arguments().contains("--avatar-check") && !engine.rootObjects().isEmpty()) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        auto* avatar = engine.rootObjects().first()->findChild<QObject*>("avatarView");
        const auto output = QCoreApplication::applicationDirPath() + "/avatar-check";
        QDir().mkpath(output);
        QTimer::singleShot(2500, &app, [&, window, avatar, output] {
            if (!window || !avatar || !avatar->property("ready").toBool()) {
                qCritical() << "Avatar did not load"; app.exit(2); return;
            }
            saveFrame(window, output + "/closed.png");
            QTimer::singleShot(200, &conversation, &ConversationManager::previewVoice);
            auto* poll = new QTimer(&app);
            poll->setInterval(33);
            QObject::connect(poll, &QTimer::timeout, &app,
                [&, poll, window, avatar, output, captured = false, peak = 0.0f]() mutable {
                    peak = std::max(peak, conversation.outputLevel());
                    if (!captured && avatar->property("mouthOpen").toReal() > 0.45) {
                        saveFrame(window, output + "/speaking.png"); captured = true;
                    }
                    if (!conversation.errorMessage().isEmpty()) {
                        qCritical() << conversation.errorMessage(); poll->stop(); app.exit(3);
                    } else if (conversation.state() == "Idle" && captured &&
                               avatar->property("mouthOpen").toReal() < 0.01) {
                        saveFrame(window, output + "/finished.png");
                        qInfo() << "Avatar playback check passed; peak RMS:" << peak;
                        poll->stop(); QTimer::singleShot(250, &app, &QCoreApplication::quit);
                    }
                });
            poll->start();
        });
        QTimer::singleShot(120000, &app, [&app] { qCritical() << "Avatar check timed out"; app.exit(4); });
    }
    if (app.arguments().contains("--smoke-test")) {
        if (!engine.rootObjects().isEmpty()) {
            if (auto* dialog = engine.rootObjects().first()->findChild<QObject*>("settingsDialog"))
                QMetaObject::invokeMethod(dialog, "open");
        }
        QTimer::singleShot(1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
