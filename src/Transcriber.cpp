#include "AssetPath.hpp"
#include "Transcriber.hpp"
#include "AudioEngine.hpp"
#include "SpeechSegmenter.hpp"
#include "InferenceScheduler.hpp"
#include <QTimer>
#include <QFileInfo>
#include <QLibrary>
#include <stdexcept>
#ifdef CHARASHELL_WITH_ASR
#include <speech.h>
#define ASR_FUNCTIONS(X) \
    X(speech_api_version_major) X(speech_api_version_minor) X(speech_last_error) \
    X(speech_device_count) X(speech_device_name) X(speech_device_description) X(speech_device_get_kind) \
    X(speech_load_params_new) X(speech_load_params_free) X(speech_load_params_set_device) \
    X(speech_load_params_set_threads) X(speech_model_load) X(speech_model_free) \
    X(speech_model_get_info) X(speech_model_info_free) X(speech_model_info_architecture) \
    X(speech_request_new) X(speech_request_free) X(speech_request_set_audio) X(speech_request_set_progress) \
    X(speech_transcribe) X(speech_request_result) X(speech_result_text)
#endif

struct Transcriber::Impl {
    QTimer* timer = nullptr;
    QString partial;
    SpeechSegmenter segmenter;
    bool suppressed = false;
    QVariantMap modelSettings;
#ifdef CHARASHELL_WITH_ASR
    QLibrary library;
    QString libraryPath;
    speech_model* model = nullptr;
#define DECLARE(name) decltype(&name) name = nullptr;
    ASR_FUNCTIONS(DECLARE)
#undef DECLARE
    void check(speech_status status) const {
        if (status < 0) throw std::runtime_error((QString("Haas-ja: ") + QString::fromUtf8(speech_last_error())).toStdString());
    }
    void releaseModel() {
        if (model) speech_model_free(model);
        model = nullptr; modelSettings.clear();
    }
    ~Impl() { releaseModel(); }
    void loadApi(const QString& path) {
        if (library.isLoaded() && libraryPath == path) return;
        releaseModel();
        if (library.isLoaded()) library.unload();
        libraryPath.clear(); library.setFileName(path);
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        if (!library.load()) throw std::runtime_error(("Haas-ja用speech.cppを読み込めません: " + library.errorString()).toStdString());
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) throw std::runtime_error("Missing ASR API: " #name);
        ASR_FUNCTIONS(RESOLVE)
#undef RESOLVE
        if (speech_api_version_major() != 3 || speech_api_version_minor() < 7)
            throw std::runtime_error("Haas-jaにはspeech.cpp 0.9.4互換のASRライブラリが必要です。");
        libraryPath = path;
    }
    QByteArray device(bool gpu) {
        if (!gpu) return "cpu";
        QByteArray first;
        for (size_t i = 0; i < speech_device_count(); ++i) {
            speech_device_kind kind;
            check(speech_device_get_kind(i, &kind));
            if (kind != SPEECH_DEVICE_GPU) continue;
            const QByteArray name(speech_device_name(i));
            if (first.isEmpty()) first = name;
            if (QString::fromUtf8(speech_device_description(i)).contains("NVIDIA", Qt::CaseInsensitive)) return name;
        }
        if (first.isEmpty()) throw std::runtime_error("Haas-jaのVulkan GPUを利用できません。");
        return first;
    }
    QString recognize(const std::atomic<quint64>& state, quint64 epoch) {
        const auto cancelled = [&] { return state.load() != epoch || (epoch & 1); };
        auto lease = InferenceScheduler::gpu().acquire(false, cancelled);
        if (!lease) return {};
        speech_request* request = nullptr;
        check(speech_request_new(model, &request));
        const auto freeRequest = [this](speech_request* p) { speech_request_free(p); };
        std::unique_ptr<speech_request, decltype(freeRequest)> owned(request, freeRequest);
        const auto& audio = segmenter.audio();
        check(speech_request_set_audio(request, audio.data(), audio.size(), 16000));
        struct Cancellation { const std::atomic<quint64>* state; quint64 epoch; } cancellation{&state, epoch};
        check(speech_request_set_progress(request, [](double, void* data) {
            const auto& cancel = *static_cast<Cancellation*>(data);
            return cancel.state->load() != cancel.epoch ? 1 : 0;
        }, &cancellation));
        const auto status = speech_transcribe(request);
        check(status);
        if (status == SPEECH_CANCELLED || cancelled()) return {};
        const auto* result = speech_request_result(request);
        if (!result) throw std::runtime_error("Haas-jaの認識結果を取得できません。");
        return QString::fromUtf8(speech_result_text(result)).trimmed();
    }
#endif
};

Transcriber::Transcriber(AudioEngine* audio) : impl_(std::make_unique<Impl>()), audio_(audio) {}
Transcriber::~Transcriber() { stop(); }
bool Transcriber::ensureModel(const QVariantMap& settings, QString& message) {
#ifdef CHARASHELL_WITH_ASR
    const QVariantMap key{{"asrPath", resolveAssetPath(settings.value("asrPath").toString())},
        {"asrLibraryPath", resolveAssetPath(settings.value("asrLibraryPath").toString())},
        {"useGpu", settings.value("useGpu", true)}};
    if (impl_->model && impl_->modelSettings == key) return true;
    try {
        impl_->releaseModel();
        const auto path = key.value("asrPath").toString();
        if (!QFileInfo::exists(path)) throw std::runtime_error(("Haas-jaモデルが見つかりません: " + path).toStdString());
        impl_->loadApi(key.value("asrLibraryPath").toString());
        speech_load_params* params = nullptr;
        impl_->check(impl_->speech_load_params_new(&params));
        const auto freeParams = [this](speech_load_params* p) { impl_->speech_load_params_free(p); };
        std::unique_ptr<speech_load_params, decltype(freeParams)> owned(params, freeParams);
        const auto device = impl_->device(settings.value("useGpu", true).toBool());
        impl_->check(impl_->speech_load_params_set_device(params, device.constData()));
        impl_->check(impl_->speech_load_params_set_threads(params, 4));
        impl_->check(impl_->speech_model_load(path.toUtf8().constData(), params, &impl_->model));
        speech_model_info* info = nullptr;
        impl_->check(impl_->speech_model_get_info(impl_->model, &info));
        const auto architecture = QString::fromUtf8(impl_->speech_model_info_architecture(info));
        impl_->speech_model_info_free(info);
        if (architecture != "fastconformer") throw std::runtime_error("Haas-jaのFastConformer GGUFモデルを指定してください。");
        impl_->modelSettings = key;
        return true;
    } catch (const std::exception& e) {
        impl_->releaseModel(); message = QString::fromUtf8(e.what()); return false;
    }
#else
    Q_UNUSED(settings);
    message = "このビルドは音声認識を含みません。CHARASHELL_WITH_ASR=ONでビルドしてください。";
    return false;
#endif
}
void Transcriber::preload(quint64 generation, const QVariantMap& settings) {
    stop();
#ifdef CHARASHELL_WITH_ASR
    QString message;
    if (!ensureModel(settings, message)) { emit loadFailed(generation, message); return; }
    setSuppressed(true);
#else
    Q_UNUSED(settings);
#endif
    emit loaded(generation);
}
void Transcriber::start(const QVariantMap& settings) {
    stop();
    impl_->segmenter.configure(settings.value("vadThreshold", .025).toFloat(), settings.value("endpointSilence", .4).toDouble());
    QString message;
    if (!ensureModel(settings, message)) { emit error(message); return; }
    impl_->timer = new QTimer(this); impl_->timer->setInterval(20);
    connect(impl_->timer, &QTimer::timeout, this, &Transcriber::poll);
    impl_->timer->start(); emit ready();
}
void Transcriber::stop() {
    if (impl_->timer) { delete impl_->timer; impl_->timer = nullptr; }
    float discard[3200]; while (audio_->takeCaptured(discard, 3200)) {}
    impl_->partial.clear(); impl_->segmenter.reset(); impl_->suppressed = false;
}
void Transcriber::setSuppressed(bool suppressed) {
    impl_->suppressed = suppressed; impl_->partial.clear(); impl_->segmenter.reset();
    float discard[3200]; while (audio_->takeCaptured(discard, 3200)) {}
    emit partialText({});
}
bool Transcriber::resumeInput(quint64 expected) {
    setSuppressed(false);
    if (inputState_.compare_exchange_strong(expected, (expected + 2) & ~quint64(1))) return true;
    impl_->suppressed = true;
    return false;
}
void Transcriber::poll() {
#ifdef CHARASHELL_WITH_ASR
    try {
        float samples[320];
        for (int batch = 0; batch < 10; ++batch) {
            const auto count = audio_->takeCaptured(samples, 320);
            if (!count) break;
            const auto epoch = inputState_.load();
            if (impl_->suppressed || (epoch & 1)) continue;
            const auto update = impl_->segmenter.push(samples, count);
            if (update.started) emit speechStarted();
            if (!update.partial && !update.final) continue;
            const auto text = impl_->recognize(inputState_, epoch);
            if (inputState_.load() != epoch) { impl_->segmenter.reset(); return; }
            if (text != impl_->partial) { impl_->partial = text; emit partialText(text); }
            if (update.final) {
                if (!text.isEmpty()) emit finalText(text, epoch);
                impl_->segmenter.reset(); impl_->partial.clear(); emit partialText({}); emit utteranceEnded();
            }
        }
    } catch (const std::exception& e) {
        setSuppressed(true); emit error(QString::fromUtf8(e.what()));
    }
#endif
}
