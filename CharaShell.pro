TEMPLATE = app
TARGET = charashell
QT += core gui qml quick quickcontrols2 network concurrent
CONFIG += c++20
CONFIG -= debug_and_release
INCLUDEPATH += $$PWD/include $$PWD/third_party/miniaudio
!exists($$PWD/third_party/miniaudio/miniaudio.h): error(Run scripts/prepare-qtcreator.ps1 to prepare dependencies.)
# The project runtime is the default even when an old generated dependency file
# still points at a previous checkout. Explicit qmake arguments take precedence.
isEmpty(CHARASHELL_RUNTIME_DIR): CHARASHELL_RUNTIME_DIR = $$PWD/build-dist
exists($$PWD/third_party/qtcreator-runtime.pri): include($$PWD/third_party/qtcreator-runtime.pri)

SOURCES += src/main.cpp src/NativeRig.cpp src/NativeRigItem.cpp src/miniaudio.c src/AudioEngine.cpp src/Transcriber.cpp \
    src/Thinker.cpp src/Speaker.cpp src/ConversationManager.cpp src/System2Client.cpp src/SpeechCoordinator.cpp src/Settings.cpp src/SetupManager.cpp src/StartupManager.cpp
HEADERS += $$files($$PWD/include/*.hpp)
RESOURCES += qml/qml.qrc resources/voice-design.qrc resources/native-avatar.qrc resources/icons.qrc resources/prompts.qrc resources/thinking.qrc resources/backgrounds.qrc
DISTFILES += qml/Main.qml qml/qmldir README.md README.en.md LICENSE $$files($$PWD/images/*) \
    scripts/build-qmake.ps1 scripts/prepare-qtcreator.ps1 scripts/deploy-qmake.ps1

!contains(CONFIG, http_only) {
    DEFINES += CHARASHELL_NATIVE=1
    INCLUDEPATH += $$PWD/third_party/native/include
    SOURCES += src/NativeThinker.cpp src/NativeSpeaker.cpp
    !exists($$PWD/third_party/native/include/llama.h): error(Native API headers missing. Run scripts/prepare-qtcreator.ps1.)
    !exists($$PWD/third_party/native/include/speech.h): error(Irodori C++ API header missing. Run scripts/prepare-qtcreator.ps1.)
}
!contains(CONFIG, without_asr) {
    DEFINES += CHARASHELL_WITH_ASR=1
    INCLUDEPATH += $$PWD/third_party/native/include
    !exists($$PWD/third_party/native/include/speech.h): error(speech.cpp API header missing. Run scripts/native.ps1.)
}
win32-msvc* {
    QMAKE_CFLAGS += /utf-8
    QMAKE_CXXFLAGS += /utf-8
}
win32: LIBS += -lole32
unix:!macx: LIBS += -lpthread -ldl -lm

win32 {
    isEmpty(CHARASHELL_RUNTIME_DIR): CHARASHELL_RUNTIME_DIR = $$PWD/build-dist
    CONFIG(debug, debug|release): deploy_mode = Debug
    else: deploy_mode = Release
    QMAKE_POST_LINK += powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \
        $$shell_quote($$shell_path($$PWD/scripts/deploy-qmake.ps1)) \
        -OutputDir $$shell_quote($$shell_path($$OUT_PWD)) \
        -RuntimeDir $$shell_quote($$shell_path($$CHARASHELL_RUNTIME_DIR)) \
        -ProjectRoot $$shell_quote($$shell_path($$PWD)) \
        -QtPrefix $$shell_quote($$shell_path($$[QT_INSTALL_PREFIX])) \
        -Configuration $$deploy_mode
}
