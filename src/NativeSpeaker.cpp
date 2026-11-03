#include "AssetPath.hpp"
#include "NativeInference.hpp"
#include <speech.h>
#include <miniaudio.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

static QString referenceSignature(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo file(path);
    return QString::fromLatin1(QCryptographicHash::hash(
        (file.canonicalFilePath() + ":" + QString::number(file.size()) + ":" +
         QString::number(file.lastModified().toMSecsSinceEpoch())).toUtf8(), QCryptographicHash::Sha256).toHex());
}

static QString voiceDesignReferenceText() {
    QFile file(":/voice-design/voice-design-reference.txt");
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Voice Designの参照生成用テキストを読み込めません。");
    const auto text = QString::fromUtf8(file.readAll()).trimmed();
    if (text.isEmpty()) throw std::runtime_error("Voice Designの参照生成用テキストが空です。");
    return text;
}

static QString designedVoicePath(const QString& model, const QVariantMap& settings, const QString& referenceText) {
    // The official F16/Q8 files share the voice codec. Reuse a voice designed
    // with the installed F16 file instead of designing a different voice on upgrade.
    auto identityModel = model;
    if (QFileInfo(model).fileName() == "Irodori-TTS-866M-MF-v4.1-Q8_0.gguf") {
        const auto original = QFileInfo(model).dir().filePath("../Irodori-TTS-866M-MF-v4.1-F16.gguf");
        if (QFileInfo::exists(original)) identityModel = original;
    }
    const QJsonArray identity{"irodori-voice-design-v2", referenceSignature(identityModel),
        settings.value("ttsCaption").toString().trimmed(), settings.value("ttsDesignRevision").toString(), referenceText};
    const auto key = QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/voice-design/" + QString::fromLatin1(key) + ".wav";
}

static qint64 designedVoiceSeed(const QVariantMap& settings) {
    const auto revision = settings.value("ttsDesignRevision").toString();
    if (revision.isEmpty()) return 42;
    const auto digest = QCryptographicHash::hash(revision.toUtf8(), QCryptographicHash::Sha256);
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(digest.constData()));
}

static void saveDesignedVoice(const QString& path, const std::vector<float>& samples, int rate) {
    if (samples.size() < size_t(rate / 2) || samples.size() > size_t(rate) * 30)
        throw std::runtime_error("Voice Designの基準音声が短すぎるか、長すぎます。");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Voice Designの声を保存するフォルダを作成できません。");
    const auto dataSize = quint32(samples.size() * sizeof(float));
    QByteArray wave("RIFF");
    auto put16 = [&wave](quint16 value) { value = qToLittleEndian(value); wave.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
    auto put32 = [&wave](quint32 value) { value = qToLittleEndian(value); wave.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
    // IEEE float WAVE preserves the generated reference without quantization.
    put32(48 + dataSize); wave.append("WAVEfmt "); put32(16);
    put16(3); put16(1); put32(quint32(rate)); put32(quint32(rate) * 4); put16(4); put16(32);
    wave.append("fact"); put32(4); put32(quint32(samples.size()));
    wave.append("data"); put32(dataSize);
    for (float sample : samples) {
        sample = std::clamp(sample, -1.f, 1.f);
        quint32 bits; std::memcpy(&bits, &sample, sizeof(bits)); put32(bits);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wave) != wave.size() || !file.commit())
        throw std::runtime_error(("Voice Designの声を保存できません: " + file.errorString()).toStdString());
}

#define SPEECH_FUNCTIONS(X) \
    X(speech_api_version_major) X(speech_api_version_minor) X(speech_last_error) \
    X(speech_device_count) X(speech_device_name) X(speech_device_description) X(speech_device_get_kind) \
    X(speech_load_params_new) X(speech_load_params_free) X(speech_load_params_set_device) \
    X(speech_load_params_set_threads) X(speech_load_params_set_warmup) X(speech_model_load) X(speech_model_free) \
    X(speech_model_get_info) X(speech_model_info_free) X(speech_model_info_architecture) \
    X(speech_model_info_sample_rate) X(speech_model_info_device) X(speech_voice_add) \
    X(speech_request_new) X(speech_request_free) X(speech_request_set_text) \
    X(speech_request_set_string) X(speech_request_set_int) X(speech_request_set_progress) X(speech_synthesize)

struct NativeSpeaker::Impl {
    QLibrary library;
    QString libraryPath, modelPath, referenceKey, deviceName;
    speech_model* model = nullptr;
    bool ready = false, gpu = false;
    int sampleRate = 48000;
#define DECLARE(name) decltype(&name) name = nullptr;
    SPEECH_FUNCTIONS(DECLARE)
#undef DECLARE
    void check(speech_status status) const {
        if (status < 0) throw std::runtime_error((QString("Irodori C++: ") + QString::fromUtf8(speech_last_error())).toStdString());
    }
    void release() {
        if (model) speech_model_free(model);
        model = nullptr; modelPath.clear(); referenceKey.clear();
    }
    ~Impl() { release(); }
    void loadApi(const QString& path) {
        if (ready && path == libraryPath) return;
        release(); ready = false;
        if (library.isLoaded()) library.unload();
        library.setFileName(path);
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        if (!library.load()) throw std::runtime_error(("Irodori C++の推論DLLを読み込めません。モデルを自動取得してください。\n" + library.errorString()).toStdString());
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) throw std::runtime_error("Missing speech.cpp API: " #name);
        SPEECH_FUNCTIONS(RESOLVE)
#undef RESOLVE
        if (speech_api_version_major() != SPEECH_API_VERSION_MAJOR || speech_api_version_minor() < SPEECH_API_VERSION_MINOR)
            throw std::runtime_error("Irodori C++のAPIバージョンが一致しません。モデルを再取得してください。");
        libraryPath = path; ready = true;
    }
    QByteArray selectDevice(bool useGpu) {
        if (!useGpu) return "cpu";
        QByteArray first;
        for (size_t i = 0; i < speech_device_count(); ++i) {
            speech_device_kind kind;
            check(speech_device_get_kind(i, &kind));
            if (kind != SPEECH_DEVICE_GPU) continue;
            const QByteArray name(speech_device_name(i));
            if (first.isEmpty()) first = name;
            if (QString::fromUtf8(speech_device_description(i)).contains("NVIDIA", Qt::CaseInsensitive)) return name;
        }
        if (first.isEmpty()) throw std::runtime_error("IrodoriのVulkan GPUを利用できません。GPUドライバーを確認してください。");
        return first;
    }
    void installReference(const QString& voice) {
        if (!voice.isEmpty()) check(speech_voice_add(model, "reference", voice.toUtf8().constData()));
        referenceKey = referenceSignature(voice);
    }
    void loadModel(const QString& path, const QString& voice, bool useGpu) {
        const auto referenceId = referenceSignature(voice);
        if (model && modelPath == path && gpu == useGpu && referenceKey == referenceId) return;
        release();
        const auto device = selectDevice(useGpu);
        speech_load_params* params = nullptr;
        check(speech_load_params_new(&params));
        const auto freeParams = [this](speech_load_params* p) { speech_load_params_free(p); };
        std::unique_ptr<speech_load_params, decltype(freeParams)> parameters(params, freeParams);
        check(speech_load_params_set_device(params, device.constData()));
        check(speech_load_params_set_threads(params, 4));
        check(speech_load_params_set_warmup(params, 1));
        check(speech_model_load(path.toUtf8().constData(), params, &model));
        try {
            speech_model_info* info = nullptr;
            check(speech_model_get_info(model, &info));
            const auto freeInfo = [this](speech_model_info* p) { speech_model_info_free(p); };
            std::unique_ptr<speech_model_info, decltype(freeInfo)> information(info, freeInfo);
            if (QString::fromUtf8(speech_model_info_architecture(info)) != "irodori-tts")
                throw std::runtime_error("Irodori-TTS-v4.1-Small-MF用のGGUFを指定してください。");
            sampleRate = speech_model_info_sample_rate(info);
            if (sampleRate != 48000) throw std::runtime_error("Irodori 4.1-MFの48 kHzコーデックを含むモデルを指定してください。");
            deviceName = QString::fromUtf8(speech_model_info_device(info));
            installReference(voice);
            modelPath = path; gpu = useGpu;
            qInfo().noquote() << "Irodori C++ model loaded on" << deviceName;
        } catch (...) { release(); throw; }
    }
    speech_status synthesize(const QString& text, const QString& caption, const char* voice,
            speech_audio_callback audio, speech_progress_callback progress, void* data, qint64 seed = 42) {
        speech_request* request = nullptr;
        check(speech_request_new(model, &request));
        const auto freeRequest = [this](speech_request* p) { speech_request_free(p); };
        std::unique_ptr<speech_request, decltype(freeRequest)> owned(request, freeRequest);
        check(speech_request_set_text(request, text.trimmed().toUtf8().constData()));
        check(speech_request_set_string(request, SPEECH_OPT_VOICE, voice));
        check(speech_request_set_string(request, SPEECH_OPT_INSTRUCTIONS, caption.toUtf8().constData()));
        check(speech_request_set_int(request, SPEECH_OPT_STEPS, 4));
        check(speech_request_set_int(request, SPEECH_OPT_SEED, seed));
        check(speech_request_set_progress(request, progress, data));
        return speech_synthesize(request, audio, data);
    }
};

namespace {
struct AudioOutput {
    const std::function<bool(const float*, unsigned)>& output;
    ma_resampler resampler{};
    bool resampling = false;
    bool captureOnly = false;
    std::vector<float> referencePcm;
    std::exception_ptr failure;
    QElapsedTimer timer;
    qint64 firstAudioMs = -1;
    quint64 samples = 0;
    AudioOutput(const std::function<bool(const float*, unsigned)>& callback, int sourceRate, int targetRate, bool capture = false)
        : output(callback), captureOnly(capture) {
        if (targetRate < 8000 || targetRate > 48000) throw std::runtime_error("TTSサンプルレートは8000〜48000 Hzで指定してください。");
        if (sourceRate != targetRate) {
            auto config = ma_resampler_config_init(ma_format_f32, 1, sourceRate, targetRate, ma_resample_algorithm_linear);
            if (ma_resampler_init(&config, nullptr, &resampler) != MA_SUCCESS) throw std::runtime_error("Irodoriの再生レート変換を初期化できません。");
            resampling = true;
        }
        timer.start();
    }
    ~AudioOutput() { if (resampling) ma_resampler_uninit(&resampler, nullptr); }
    bool deliver(const float* pcm, size_t count) {
        float block[2400];
        while (count) {
            const auto n = unsigned(std::min<size_t>(count, 2400));
            for (unsigned i = 0; i < n; ++i) block[i] = std::clamp(pcm[i], -1.f, 1.f);
            if (firstAudioMs < 0) firstAudioMs = timer.elapsed();
            if (!output(block, n)) return false;
            samples += n; pcm += n; count -= n;
        }
        return true;
    }
    static int progress(double, void* data) noexcept {
        auto& context = *static_cast<AudioOutput*>(data);
        try { return context.output(nullptr, 0) ? 0 : 1; }
        catch (...) { context.failure = std::current_exception(); return 1; }
    }
    static int audio(const float* pcm, size_t count, void* data) noexcept {
        auto& context = *static_cast<AudioOutput*>(data);
        try {
            if (context.captureOnly) {
                if (!context.output(nullptr, 0)) return 1;
                if (count > 48000 * 30 - context.referencePcm.size()) throw std::runtime_error("Voice Designの基準音声が長すぎます。");
                context.referencePcm.insert(context.referencePcm.end(), pcm, pcm + count);
                return 0;
            }
            if (!context.resampling) return context.deliver(pcm, count) ? 0 : 1;
            float block[2400];
            while (count) {
                ma_uint64 consumed = count, produced = 2400;
                if (ma_resampler_process_pcm_frames(&context.resampler, pcm, &consumed, block, &produced) != MA_SUCCESS)
                    throw std::runtime_error("Irodoriの音声レート変換に失敗しました。");
                if (!consumed && !produced) throw std::runtime_error("Irodoriの音声レート変換が進みません。");
                if (!context.deliver(block, size_t(produced))) return 1;
                pcm += consumed; count -= size_t(consumed);
            }
            return 0;
        } catch (...) { context.failure = std::current_exception(); return 1; }
    }
};
}

NativeSpeaker::NativeSpeaker() : impl_(std::make_unique<Impl>()) {}
NativeSpeaker::~NativeSpeaker() = default;
void NativeSpeaker::load(const QVariantMap& settings, const std::function<bool()>& cancelled,
        const ModelLoadProgress& progress) {
    const std::function<bool(const float*, unsigned)> output = [&](const float*, unsigned) { return !cancelled || !cancelled(); };
    if (!output(nullptr, 0)) return;
    if (progress) progress(-1, QStringLiteral("音声合成モデルと音声コーデックをロードしています"));
    auto& api = *impl_;
    const auto path = resolveAssetPath(settings.value("ttsPath").toString());
    auto voice = resolveAssetPath(settings.value("voicePath").toString());
    if (!QFileInfo::exists(path) || QFileInfo(path).suffix().compare("gguf", Qt::CaseInsensitive) != 0)
        throw std::runtime_error("Irodori C++用のGGUFモデルがありません。モデルを自動取得してください。");
    if (!voice.isEmpty() && !QFileInfo::exists(voice)) throw std::runtime_error("Irodoriの参照音声が見つかりません。");
    api.loadApi(resolveAssetPath(settings.value("ttsLibraryPath", "assets/irodori/native/speech.dll").toString()));
    const bool useGpu = settings.value("useGpu", true).toBool();
    const auto caption = settings.value("ttsCaption").toString().trimmed();
    if (voice.isEmpty()) {
        const auto referenceText = voiceDesignReferenceText();
        voice = designedVoicePath(path, settings, referenceText);
        if (!QFileInfo::exists(voice)) {
            if (progress) progress(-1, QStringLiteral("読み上げ用の参照音声を準備しています"));
            api.loadModel(path, {}, useGpu);
            if (!output(nullptr, 0)) return;
            AudioOutput design(output, api.sampleRate, api.sampleRate, true);
            // Generate the dedicated script together so Voice Design chooses one voice for the reference.
            const auto status = api.synthesize(referenceText, caption,
                "none", AudioOutput::audio, AudioOutput::progress, &design, designedVoiceSeed(settings));
            if (design.failure) std::rethrow_exception(design.failure);
            api.check(status);
            if (status == SPEECH_CANCELLED || !output(nullptr, 0)) return;
            saveDesignedVoice(voice, design.referencePcm, api.sampleRate);
            api.installReference(voice);
            qInfo().noquote() << "Voice Design reference saved:" << voice;
        } else api.loadModel(path, voice, useGpu);
    } else api.loadModel(path, voice, useGpu);
    if (output(nullptr, 0) && progress) progress(1, QStringLiteral("音声合成モデルのロードが完了しました"));
}
void NativeSpeaker::speak(const QString& text, const QVariantMap& settings,
        const std::function<bool(const float*, unsigned)>& output) {
    if (!output(nullptr, 0)) return;
    load(settings, [&] { return !output(nullptr, 0); });
    auto& api = *impl_;
    const auto caption = settings.value("ttsCaption").toString().trimmed();
    if (!output(nullptr, 0)) return;
    AudioOutput context(output, api.sampleRate, settings.value("sampleRate", 48000).toInt());
    const auto status = api.synthesize(text, caption, "reference", AudioOutput::audio, AudioOutput::progress, &context);
    if (context.failure) std::rethrow_exception(context.failure);
    api.check(status);
    if (status != SPEECH_CANCELLED)
        qInfo() << "Irodori C++ first PCM (ms):" << context.firstAudioMs << "total (ms):" << context.timer.elapsed() << "samples:" << context.samples;
}
