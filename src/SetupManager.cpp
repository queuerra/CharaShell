#include "AssetPath.hpp"
#include "SetupManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

SetupManager::SetupManager(Settings* settings) : settings_(settings) {
    plan();
    connect(settings_, &Settings::changed, this, [this] { QTimer::singleShot(0, this, &SetupManager::checkModels); });
    process_.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    connect(&process_, &QProcess::readyReadStandardOutput, this, &SetupManager::readOutput);
    connect(&process_, &QProcess::stateChanged, this, &SetupManager::changed);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            status_ = "ダウンロード処理を開始できません: " + process_.errorString(); emit changed(); emit finished(false);
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        readOutput();
        const bool success = code == 0 && exit == QProcess::NormalExit && checkModels();
        if (success) status_ = "Irodori-TTS-v4.1-Small-MFの準備ができました。参照音声は任意でWAVを指定できます。";
        else status_ = "ダウンロードが完了しませんでした。再試行すると途中から再開します。\n" + output_.right(2000);
        emit changed(); emit finished(success);
    });
}
QString SetupManager::root() const { return QCoreApplication::applicationDirPath() + "/assets"; }
void SetupManager::plan() {
    QFile manifest(QCoreApplication::applicationDirPath() + "/scripts/assets.json");
    if (!manifest.open(QIODevice::ReadOnly)) return;
    const auto files = QJsonDocument::fromJson(manifest.readAll()).object().value("files").toArray();
    for (const auto& item : files) {
        const auto asset = item.toObject();
        if (asset.value("kind").toString() != "Models") continue;
        if (!asset.value("architecture").toString().isEmpty() && asset.value("architecture").toString() != "x64") continue;
        plannedBytes_ += qint64(asset.value("size").toDouble() > 0 ? asset.value("size").toDouble() : asset.value("estimatedSize").toDouble());
    }
}
qint64 SetupManager::receivedBytes() const {
    qint64 sum = 0;
    for (auto bytes : fileProgress_) sum += bytes;
    return sum;
}
bool SetupManager::applyModels() {
    QFile file(root() + "/models.json");
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    if (config.value("llmModel").toString() != "gemma-4-e2b-qat") return false;
    const auto asr = resolveAssetPath(config.value("asrPath").toString());
    const auto asrLibrary = resolveAssetPath(config.value("asrLibraryPath").toString());
    if (!QFileInfo(asr).isFile() || !QFileInfo(asrLibrary).isFile()) return false;
    if (!QFileInfo::exists(resolveAssetPath(config.value("llmPath").toString())) || !QFileInfo::exists(resolveAssetPath(config.value("ttsPath").toString()))) return false;
    if (!QFileInfo::exists(root() + "/irodori/native/speech.dll")) return false;
    const auto custom = resolveAssetPath(settings_->values().value("asrPath").toString());
    if (custom.isEmpty() || !QFileInfo(custom).isFile())
        settings_->save({{"asrPath", asr}, {"language", "ja"}});
    QVariantMap paths;
    for (const auto* key : {"llmPath", "ttsPath", "asrLibraryPath"}) {
        if (!QFileInfo::exists(resolveAssetPath(settings_->values().value(key).toString()))) paths[key] = config.value(key).toString();
    }
    if (!QFileInfo::exists(resolveAssetPath(settings_->values().value("ttsLibraryPath").toString())))
        paths["ttsLibraryPath"] = root() + "/irodori/native/speech.dll";
    if (!paths.isEmpty()) settings_->save(paths);
    return true;
}
bool SetupManager::checkModels() {
    const auto custom = resolveAssetPath(settings_->values().value("asrPath").toString());
    const auto values = settings_->values();
    const bool inferenceReady = values.value("backend").toString() == "http" ||
        (QFileInfo::exists(resolveAssetPath(values.value("llmPath").toString())) && QFileInfo::exists(resolveAssetPath(values.value("ttsPath").toString())) &&
         QFileInfo::exists(resolveAssetPath(values.value("ttsLibraryPath").toString())));
    const bool asrReady = QFileInfo(custom).isFile() &&
        QFileInfo::exists(resolveAssetPath(values.value("asrLibraryPath").toString()));
    const bool available = (asrReady && inferenceReady) || applyModels();
    if (ready_ != available) { ready_ = available; emit changed(); }
    return available;
}
bool SetupManager::ensureModels() { return checkModels(); }
void SetupManager::readOutput() {
    const auto bytes = process_.readAllStandardOutput();
    if (bytes.isEmpty()) return;
    pendingOutput_ += bytes;
    while (true) {
        const auto end = pendingOutput_.indexOf('\n');
        if (end < 0) break;
        const auto line = QString::fromUtf8(pendingOutput_.left(end)).trimmed();
        pendingOutput_.remove(0, end + 1);
        output_ += line + '\n'; output_ = output_.right(8000);
        const auto fields = line.split(' ');
        if (fields.size() >= 3 && (fields[0] == "PROGRESS" || fields[0] == "DOWNLOAD" || fields[0] == "CACHED")) {
            const auto fraction = fields[2].split('/');
            if (fraction.size() == 2) {
                currentFile_ = fields[1];
                fileBytes_ = fraction[0].toLongLong(); fileTotal_ = fraction[1].toLongLong();
                fileProgress_[currentFile_] = fileBytes_;
                if (fields.size() > 3) bytesPerSecond_ = fields[3].toLongLong();
            }
        }
        status_ = line;
    }
    emit changed();
}
void SetupManager::download() {
    if (busy()) return;
    fileProgress_.clear(); fileBytes_ = fileTotal_ = bytesPerSecond_ = 0; currentFile_.clear(); pendingOutput_.clear();
#ifdef Q_OS_WIN
    const auto script = QCoreApplication::applicationDirPath() + "/scripts/download.ps1";
    if (!QFileInfo::exists(script)) { status_ = "scripts/download.ps1が見つかりません。build.ps1で配布用フォルダを作成してください。"; emit changed(); emit finished(false); return; }
    output_.clear(); status_ = "モデルとIrodoriのC++ GPU推論ライブラリを準備しています…"; emit changed();
    process_.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
        "-Kind", "Models", "-Backend", settings_->values().value("useGpu", true).toBool() ? "cuda" : "cpu",
        "-AssetRoot", QDir::toNativeSeparators(root())});
#else
    status_ = "画面からの自動取得はWindows版に対応しています。scripts/download.ps1をPowerShellで実行してください。";
    emit changed(); emit finished(false);
#endif
}
void SetupManager::cancel() { if (busy()) process_.kill(); }
