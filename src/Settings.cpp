#include "AssetPath.hpp"
#include "Settings.hpp"
#include <QSettings>
#include <QFileInfo>
#include <QCoreApplication>

namespace {
QString avatarPath(const QString& value) {
    return resolveAssetPath(value.trimmed().isEmpty() ? QStringLiteral("assets/avatar/default.psd") : value);
}
}

Settings::Settings(QObject* parent) : QObject(parent) {
    const auto previousPrompt = QString::fromUtf8("あなたは親しみやすい会話相手です。自然な話し言葉で、基本1〜3文で短く答えてください。不必要な箇条書きを避け、毎回質問で終わらないでください。");
    const auto conversationPrompt = QString::fromUtf8("あなたは親しみやすい日本語の会話相手です。会話の流れを踏まえて、ユーザーの直近の質問や依頼に直接答えてください。自然な日本語の話し言葉で、基本1〜3文で短く答えてください。質問を繰り返したり、関係のない話題に変えたりせず、分からないことは分からないと伝えてください。不必要な箇条書きを避け、毎回質問で終わらないでください。");
    values_ = {{"llmUrl", "http://127.0.0.1:8081"}, {"ttsUrl", "http://127.0.0.1:8088"},
        {"llmModel", "gemma-4-e2b-qat"}, {"ttsModel", "irodori-tts"}, {"voice", "none"},
        {"asrPath", "assets/asr/Haas-616M-ja-v1.0-Q8_0.gguf"}, {"asrLibraryPath", "assets/asr/native/speech.dll"},
        {"language", "ja"}, {"sampleRate", 48000},
        {"backend", "native"}, {"llmPath", "assets/models/gemma-4-e2b-qat/gemma-4-E2B_q4_0-it.gguf"},
        {"ttsPath", "assets/irodori/q8/Irodori-TTS-866M-MF-v4.1-Q8_0.gguf"},
        {"ttsLibraryPath", "assets/irodori/native/speech.dll"},
        {"ttsCaption", QString::fromUtf8("自然で明るい日本語の声。落ち着いた会話調。")},
        {"ttsDesignRevision", ""},
        {"voicePath", ""}, {"speechEnabled", true}, {"contextSize", 4096}, {"gpuLayers", 999}, {"useGpu", true},
        {"llmKvCache", "f16"},
        {"webSearchEnabled", true}, {"searchOpenBrowser", true}, {"advancedWithChatGPT", false},
        {"system2Provider", "sarashina"}, {"sarashinaApiKey", ""},
        {"sarashinaModel", "sarashina3.2-mini-thinking"},
        {"inputDeviceId", ""}, {"outputDeviceId", ""}, {"avatarPsdPath", avatarPath({})},
        {"endpointSilence", 0.4}, {"bargeIn", false}, {"vadThreshold", 0.025},
        {"systemPrompt", conversationPrompt}};
    QSettings settings;
    for (auto i = values_.begin(); i != values_.end(); ++i) i.value() = settings.value(i.key(), i.value());
    // Shorten only the former default; retain deliberately customized endpoints.
    if (!settings.value("fastReplyEndpointMigrated", false).toBool()) {
        if (qFuzzyCompare(values_.value("endpointSilence").toDouble(), 0.7)) {
            values_["endpointSilence"] = 0.4;
            settings.setValue("endpointSilence", 0.4);
        }
        settings.setValue("fastReplyEndpointMigrated", true);
    }
    // Persist the absolute avatar path, including older relative or empty settings.
    values_["avatarPsdPath"] = avatarPath(values_.value("avatarPsdPath").toString());
    if (settings.value("avatarPsdPath").toString() != values_.value("avatarPsdPath").toString())
        settings.setValue("avatarPsdPath", values_.value("avatarPsdPath"));
    if (settings.contains("advancedWithChatGPT") && !settings.contains("system2Provider")) {
        values_["system2Provider"] = "chatgpt";
        settings.setValue("system2Provider", "chatgpt");
    }
    // Restore saved ChatGPT installations to bundled local inference.
    if (values_.value("backend").toString() == "chatgpt") {
        values_["backend"] = "native";
        values_["llmModel"] = "gemma-4-e2b-qat";
        settings.setValue("backend", values_.value("backend"));
        settings.setValue("llmModel", values_.value("llmModel"));
    }
    if (values_.value("systemPrompt").toString() == previousPrompt) {
        values_["systemPrompt"] = conversationPrompt;
        settings.setValue("systemPrompt", conversationPrompt);
    }
    // Switch the previous bundled default to LFM, while keeping explicitly chosen GGUFs.
    if (!settings.value("lfm2p5Migrated", false).toBool()) {
        if (QFileInfo(values_.value("llmPath").toString()).fileName() == "gemma-4-E4B-it-UD-Q4_K_XL.gguf") {
            values_["llmPath"] = "assets/models/lfm2.5-1.2b-jp/LFM2.5-1.2B-JP-Q4_K_M.gguf";
            values_["llmModel"] = "lfm2.5-1.2b-jp";
            settings.setValue("llmPath", values_.value("llmPath"));
            settings.setValue("llmModel", values_.value("llmModel"));
        }
        settings.setValue("lfm2p5Migrated", true);
    }
    // Upgrade the bundled LFM default once, retaining other manually chosen GGUFs.
    if (!settings.value("lfm3Migrated", false).toBool()) {
        if (QFileInfo(values_.value("llmPath").toString()).fileName() == "LFM2.5-1.2B-JP-Q4_K_M.gguf") {
            values_["llmPath"] = "assets/models/LFM3-1.7B-Q8_0.gguf";
            values_["llmModel"] = "lfm3-1.7b";
            settings.setValue("llmPath", values_.value("llmPath"));
            settings.setValue("llmModel", values_.value("llmModel"));
        }
        settings.setValue("lfm3Migrated", true);
    }
    if (!settings.value("gemma4E2bHereticMigrated", false).toBool()) {
        if (QFileInfo(values_.value("llmPath").toString()).fileName() == "LFM3-1.7B-Q8_0.gguf") {
            values_["llmPath"] = "assets/models/gemma-4-e2b-heretic/gemma-4-E2B-it-ultra-uncensored-heretic-Q4_K_M.gguf";
            values_["llmModel"] = "gemma-4-e2b-heretic";
            settings.setValue("llmPath", values_.value("llmPath"));
            settings.setValue("llmModel", values_.value("llmModel"));
        }
        settings.setValue("gemma4E2bHereticMigrated", true);
    }
    if (!settings.value("gemma4E2bQatMigrated", false).toBool()) {
        if (QFileInfo(values_.value("llmPath").toString()).fileName() == "gemma-4-E2B-it-ultra-uncensored-heretic-Q4_K_M.gguf") {
            values_["llmPath"] = "assets/models/gemma-4-e2b-qat/gemma-4-E2B_q4_0-it.gguf";
            values_["llmModel"] = "gemma-4-e2b-qat";
            settings.setValue("llmPath", values_.value("llmPath"));
            settings.setValue("llmModel", values_.value("llmModel"));
        }
        settings.setValue("gemma4E2bQatMigrated", true);
    }
    // Existing CPU installations used zero layers; migrate them to the GPU default once.
    if (!settings.contains("useGpu")) {
        values_["useGpu"] = true;
        if (values_.value("gpuLayers").toInt() == 0) values_["gpuLayers"] = 999;
        settings.setValue("useGpu", true); settings.setValue("gpuLayers", values_.value("gpuLayers"));
    }
    // Migrate CosyVoice model and GGUF reference settings once; retain custom WAV references.
    if (!settings.value("irodoriMfMigrated", false).toBool()) {
        if (QFileInfo(values_.value("ttsPath").toString()).suffix().compare("gguf", Qt::CaseInsensitive) == 0)
            values_["ttsPath"] = "assets/irodori/Irodori-TTS-866M-MF-v4.1-F16.gguf";
        if (QFileInfo(values_.value("voicePath").toString()).suffix().compare("gguf", Qt::CaseInsensitive) == 0)
            values_["voicePath"] = "";
        if (values_.value("ttsModel").toString() == "cosyvoice-3") values_["ttsModel"] = "irodori-tts";
        if (values_.value("voice").toString() == "alloy") values_["voice"] = "none";
        if (values_.value("ttsUrl").toString() == "http://127.0.0.1:8082") values_["ttsUrl"] = "http://127.0.0.1:8088";
        if (values_.value("sampleRate").toInt() == 24000) values_["sampleRate"] = 48000;
        for (const auto* key : {"ttsPath", "voicePath", "ttsModel", "voice", "ttsUrl", "sampleRate"})
            settings.setValue(key, values_.value(key));
        settings.setValue("irodoriMfMigrated", true);
    }
    if (!settings.value("irodoriCppMigrated", false).toBool()) {
        const auto old = values_.value("ttsPath").toString();
        if (QFileInfo(old).suffix().compare("safetensors", Qt::CaseInsensitive) == 0 || old.contains("cosyvoice", Qt::CaseInsensitive))
            values_["ttsPath"] = "assets/irodori/Irodori-TTS-866M-MF-v4.1-F16.gguf";
        settings.setValue("ttsPath", values_.value("ttsPath"));
        settings.setValue("ttsLibraryPath", values_.value("ttsLibraryPath"));
        settings.setValue("irodoriCppMigrated", true);
    }
    if (!settings.value("irodoriQ8Migrated", false).toBool()) {
        if (QFileInfo(values_.value("ttsPath").toString()).fileName() == "Irodori-TTS-866M-MF-v4.1-F16.gguf") {
            values_["ttsPath"] = "assets/irodori/q8/Irodori-TTS-866M-MF-v4.1-Q8_0.gguf";
            settings.setValue("ttsPath", values_.value("ttsPath"));
        }
        settings.setValue("irodoriQ8Migrated", true);
    }
}

void Settings::save(const QVariantMap& values) {
    QSettings settings;
    for (auto i = values.begin(); i != values.end(); ++i) {
        if (values_.contains(i.key())) { const bool asset = i.key() == "asrPath" || i.key() == "asrLibraryPath" || i.key() == "llmPath" || i.key() == "ttsPath" || i.key() == "voicePath" || i.key() == "ttsLibraryPath";
            values_[i.key()] = i.key() == "avatarPsdPath" ? QVariant(avatarPath(i.value().toString())) :
                asset ? QVariant(portableAssetPath(i.value().toString())) : i.value();
            settings.setValue(i.key(), values_[i.key()]); }
    }
    settings.sync(); emit changed();
}
