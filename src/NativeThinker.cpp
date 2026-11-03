#include "AssetPath.hpp"
#include "System1OutputParser.hpp"
#include "ConversationSummary.hpp"
#include "NativeInference.hpp"
#include "InferenceScheduler.hpp"
#include "NativeGpuBudget.hpp"
#include "GemmaSearchTool.hpp"
#include "ResponseTextFilter.hpp"
#include <llama.h>
#include <QLibrary>
#include <QCoreApplication>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QStringConverter>
#include <algorithm>
#include <array>
#include <vector>
#include <thread>
#include <stdexcept>

#define LLAMA_FUNCTIONS(X) \
 X(llama_backend_init) X(llama_supports_gpu_offload) X(llama_model_default_params) X(llama_context_default_params) \
 X(llama_model_load_from_file) X(llama_model_free) X(llama_init_from_model) X(llama_free) \
 X(llama_model_get_vocab) X(llama_model_meta_val_str) X(llama_tokenize) X(llama_token_to_piece) \
 X(llama_model_chat_template) X(llama_chat_apply_template) \
 X(llama_batch_get_one) X(llama_decode) X(llama_synchronize) X(llama_get_memory) X(llama_memory_clear) \
 X(llama_memory_seq_rm) X(llama_memory_seq_pos_min) X(llama_memory_seq_pos_max) \
 X(llama_vocab_is_eog) X(llama_vocab_n_tokens) X(llama_sampler_chain_default_params) X(llama_sampler_chain_init) \
 X(llama_sampler_init_grammar) X(llama_sampler_chain_add) X(llama_sampler_init_top_k) X(llama_sampler_init_top_p) \
 X(llama_sampler_init_temp) X(llama_sampler_init_min_p) X(llama_sampler_init_penalties) X(llama_sampler_init_dist) X(llama_sampler_sample) X(llama_sampler_accept) X(llama_sampler_free)

struct NativeThinker::Impl {
    QLibrary library{QCoreApplication::applicationDirPath() + "/llama"};
    QLibrary backend{QCoreApplication::applicationDirPath() + "/ggml-backend-placeholder"};
#define DECLARE(name) decltype(&name) name = nullptr;
    LLAMA_FUNCTIONS(DECLARE)
#undef DECLARE
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    QString path, architecture, kvCache;
    int contextSize = 0, gpuLayers = 0;
    bool speechEnabled = false;
    const std::atomic<quint64>* active = nullptr;
    quint64 turn = 0;
    bool ready = false;
    // Only tokens whose decode completed successfully are represented in KV.
    std::vector<llama_token> decodedTokens;
    bool cancelled() const { return active && active->load() != turn; }
    void loadApi() {
        if (ready) return;
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        if (!library.load()) throw std::runtime_error(library.errorString().toStdString());
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) throw std::runtime_error("Missing llama API: " #name);
        LLAMA_FUNCTIONS(RESOLVE)
#undef RESOLVE
        backend.setFileName(QCoreApplication::applicationDirPath() + "/ggml");
        backend.setLoadHints(QLibrary::PreventUnloadHint);
        if (!backend.load()) throw std::runtime_error(backend.errorString().toStdString());
        auto load = reinterpret_cast<decltype(&ggml_backend_load_all_from_path)>(backend.resolve("ggml_backend_load_all_from_path"));
        if (!load) throw std::runtime_error("Missing GGML backend loader");
        auto directory = QCoreApplication::applicationDirPath().toUtf8(); load(directory.constData());
        llama_backend_init();
        ready = true;
    }
    int decodeTokens(llama_token* tokens, int count) {
        std::vector<llama_pos> positions(count);
        for (int i = 0; i < count; ++i) positions[i] = llama_pos(decodedTokens.size() + i);
        auto batch = llama_batch_get_one(tokens, count);
        batch.pos = positions.data();
        const auto result = llama_decode(context, batch);
        // Complete CUDA work before releasing the scheduler lease to Vulkan TTS.
        llama_synchronize(context);
        if (result == 0) decodedTokens.insert(decodedTokens.end(), tokens, tokens + count);
        else decodedTokens.clear();
        return result;
    }
    void release() { if (context) llama_free(context); if (model) llama_model_free(model); context = nullptr; model = nullptr; architecture.clear(); decodedTokens.clear(); }
    ~Impl() { release(); }
};
NativeThinker::NativeThinker() : impl_(std::make_unique<Impl>()) {}
NativeThinker::~NativeThinker() = default;
void NativeThinker::load(quint64 turn, const QVariantMap& settings, const std::atomic<quint64>& activeTurn,
    const ModelLoadProgress& progress) {
    auto& api = *impl_; api.active = &activeTurn; api.turn = turn;
    if (api.cancelled()) return;
    if (progress) progress(-1, QStringLiteral("会話モデルの実行環境を準備しています"));
    api.loadApi();
    auto path = resolveAssetPath(settings.value("llmPath").toString());
    const auto nctx = std::clamp(settings.value("contextSize", 4096).toInt(), 1024, 8192);
    const bool useGpu = settings.value("useGpu", true).toBool();
    if (useGpu && !api.llama_supports_gpu_offload())
        throw std::runtime_error("LLMのGPUバックエンドが利用できません。CUDA版の依存ライブラリとGPUドライバーを確認してください。");
    const auto gpu = useGpu ? std::clamp(settings.value("gpuLayers", 999).toInt(), 1, 999) : 0;
    const bool speech = settings.value("speechEnabled", true).toBool();
    const auto kvCache = settings.value("llmKvCache", "f16").toString();
    if (kvCache != "q8_0" && kvCache != "f16") throw std::runtime_error("Unsupported LLM conversation cache type");
    if (!api.model || !api.context || path != api.path || nctx != api.contextSize || gpu != api.gpuLayers || speech != api.speechEnabled || kvCache != api.kvCache) {
        auto lease = InferenceScheduler::gpu().acquire(false, [&] { return api.cancelled(); });
        if (!lease) return;
        api.release();
        auto params = api.llama_model_default_params();
        params.n_gpu_layers = gpu && speech ? speechGpuLayers(path, nctx, gpu) : gpu;
        struct Loading { Impl* api; const ModelLoadProgress* progress; } loading{&api, &progress};
        params.progress_callback = [](float value, void* data) {
            auto& loading = *static_cast<Loading*>(data);
            if (loading.api->cancelled()) return false;
            if (*loading.progress) (*loading.progress)(std::clamp(double(value), 0., 1.), QStringLiteral("会話モデルの重みをロードしています"));
            return true;
        };
        params.progress_callback_user_data = &loading;
        const auto filename = path.toUtf8(); api.model = api.llama_model_load_from_file(filename.constData(), params);
        if (api.cancelled()) return;
        if (!api.model) throw std::runtime_error("LLM GGUF model could not be loaded");
        char architecture[64]{};
        api.llama_model_meta_val_str(api.model, "general.architecture", architecture, sizeof(architecture));
        api.architecture = QString::fromLatin1(architecture);
        if (progress) progress(-1, QStringLiteral("会話モデルの実行コンテキストを初期化しています"));
        auto context = api.llama_context_default_params(); context.n_ctx = nctx; context.n_batch = 128; context.n_ubatch = 64;
        // Preserve earlier SWA entries so editing a prompt can safely rewind its
        // cached suffix. A rolling SWA cache may have evicted the needed prefix.
        if (api.architecture == "gemma4") context.swa_full = true;
        // Keep the full conversation length and GPU weights. Compress only the
        // K/V cache; Flash Attention reads quantized values without a full F16 copy.
        const bool compactCache = useGpu && kvCache == "q8_0" &&
            (api.architecture == "gemma4" || api.architecture == "qwen3" ||
             api.architecture == "qwen35" || api.architecture == "llama");
        if (compactCache) {
            context.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
            context.type_k = GGML_TYPE_Q8_0; context.type_v = GGML_TYPE_Q8_0;
        }
        context.n_threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 8); context.n_threads_batch = context.n_threads;
        context.abort_callback = [](void* data) { return static_cast<Impl*>(data)->cancelled(); }; context.abort_callback_data = &api;
        api.context = api.llama_init_from_model(api.model, context);
        if (!api.context) { api.release(); throw std::runtime_error("LLM context could not be created"); }
        api.path = path; api.contextSize = nctx; api.gpuLayers = gpu; api.speechEnabled = speech; api.kvCache = kvCache;
        qInfo() << "LLM conversation cache:" << (compactCache ? "Q8_0 + Flash Attention" : "F16") << "context" << nctx;
    }
    if (!api.cancelled() && progress) progress(1, QStringLiteral("会話モデルのロードが完了しました"));
}
void NativeThinker::generate(quint64 turn, const QJsonArray& history, const QVariantMap& settings,
    const std::atomic<quint64>& activeTurn, const std::function<void(QString)>& output,
    const std::function<QJsonObject(QString)>& search) {
    QElapsedTimer generationTimer;
    generationTimer.start();
    const int maxOutputTokens = std::clamp(settings.value("maxOutputTokens", 256).toInt(), 32, 768);
    const bool summarizing = settings.value("conversationSummary", false).toBool();
    const bool structured = settings.value("system1Structured", false).toBool() || summarizing;
    qint64 firstTextMs = -1;
    ResponseTextFilter responseFilter;
    auto visibleOutput = [&](const QString& text) {
        if (text.isEmpty()) return;
        if (firstTextMs < 0) {
            firstTextMs = generationTimer.elapsed();
            qInfo() << "LLM first text (ms):" << firstTextMs;
        }
        output(text);
    };
    load(turn, settings, activeTurn);
    auto& api = *impl_;
    if (api.cancelled()) return;
    struct CacheGuard {
        Impl& api;
        bool complete = false;
        ~CacheGuard() { if (!complete) api.decodedTokens.clear(); }
    } cacheGuard{api};
    const int nctx = api.contextSize;
    const bool qwen = api.architecture == "qwen3" || api.architecture == "qwen35";
    const bool gemma4 = api.architecture == "gemma4";
    const bool lfm = api.architecture == "lfm2" || api.architecture == "lfm2.5";
    const bool lfm3 = api.architecture == "lfm3";
    const bool searchEnabled = !structured && gemma4 && search && settings.value("webSearchEnabled", true).toBool();
    const bool penalized = !structured && (qwen || lfm || lfm3 || api.architecture == "llama");
    std::vector<QByteArray> roles, contents;
    roles.reserve(history.size() + 1); contents.reserve(history.size() + 1);
    roles.emplace_back("system");
    contents.emplace_back((settings.value("systemPrompt").toString().trimmed() +
        (searchEnabled ? GemmaSearchTool::declaration() : QString())).toUtf8());
    for (const auto& value : history) {
        const auto message = value.toObject(); const auto role = message.value("role").toString();
        if (role != "user" && role != "assistant") continue;
        roles.emplace_back(role.toUtf8());
        contents.emplace_back(message.value("content").toString().trimmed().toUtf8());
    }
    const auto chatTemplate = api.llama_model_chat_template(api.model, nullptr);
    auto formatConversation = [&]() -> QString {
        if (lfm3) {
            // Official LFM3 template, without tools, enable_thinking=false. The
            // C formatter cannot pass that Jinja option; JSON grammar must start
            // after the closed thinking prefix, rather than inside a think block.
            QString prompt=QStringLiteral("<|startoftext|>");
            for(size_t i=0;i<roles.size();++i)
                prompt+="<|im_start|>"+QString::fromUtf8(roles[i])+"\n"+QString::fromUtf8(contents[i])+"<|im_end|>\n";
            prompt+="<|im_start|>assistant\n<think></think>";
            qInfo()<<"LLM chat template: LFM3, enable_thinking=false";
            return prompt;
        }
        std::vector<llama_chat_message> messages;
        messages.reserve(roles.size());
        for (size_t i = 0; i < roles.size(); ++i) messages.push_back({roles[i].constData(), contents[i].constData()});
        // The C formatter does not accept tools. Use Gemma's canonical tool format.
        int promptBytes = !searchEnabled && chatTemplate && *chatTemplate ? api.llama_chat_apply_template(
            chatTemplate, messages.data(), messages.size(), true, nullptr, 0) : -1;
        QString prompt;
        if (promptBytes > 0) {
            std::vector<char> formatted(promptBytes);
            const auto written = api.llama_chat_apply_template(chatTemplate, messages.data(), messages.size(), true,
                formatted.data(), int(formatted.size()));
            if (written <= 0 || written > int(formatted.size())) throw std::runtime_error("LLM chat template formatting failed");
            prompt = QString::fromUtf8(formatted.data(), written);
            if (qwen && prompt.endsWith("<|im_start|>assistant\n")) prompt += "<think>\n\n</think>\n\n";
            qInfo() << "LLM chat template: GGUF via llama.cpp, architecture" << api.architecture;
        } else if (qwen || api.architecture == "gemma4") {
            // Keep the existing non-thinking formats for bundled models whose new
            // Jinja variants are not recognized by b11461's built-in C API formatter.
            const QString start = qwen ? "<|im_start|>" : "<|turn>";
            const QString end = qwen ? "<|im_end|>\n" : "<turn|>\n";
            for (size_t i = 0; i < roles.size(); ++i) {
                const auto role = QString::fromUtf8(roles[i]);
                const auto speaker = role == "assistant" && !qwen ? QString("model") : role;
                prompt += start + speaker + "\n" + QString::fromUtf8(contents[i]) + end;
            }
            prompt += qwen ? "<|im_start|>assistant\n<think>\n\n</think>\n\n" : "<|turn>model\n";
            qInfo() << "LLM chat template: bundled fallback, architecture" << api.architecture;
        } else {
            throw std::runtime_error(chatTemplate && *chatTemplate
                ? "このGGUFの会話テンプレートは同梱llama.cppのテンプレートAPIで処理できません。対応するGGUFまたはllama-serverを使用してください。"
                : "このGGUFには会話テンプレートがありません。会話テンプレートを含むGGUFを使用してください。");
        }
        return prompt;
    };
    QString prompt = formatConversation();
    const auto vocab = api.llama_model_get_vocab(api.model);
    QString toolTranscript;
    auto searchPrompt = [&] {
        QString formatted;
        for (size_t i = 0; i < roles.size(); ++i) {
            const auto role = QString::fromUtf8(roles[i]);
            formatted += "<|turn>" + (role == "assistant" ? QString("model") : role) + "\n" +
                QString::fromUtf8(contents[i]) + "<turn|>\n";
        }
        return formatted + "<|turn>model\n" + toolTranscript;
    };
    for (int toolRound = 0; toolRound <= 2 && !api.cancelled(); ++toolRound) {
        auto text = prompt.toUtf8();
        int count = api.llama_tokenize(vocab, text.constData(), int(text.size()), nullptr, 0, true, true);
        // Keep the current user request and fit older turns to the actual token
        // budget. This changes only model input, never persisted history.
        while (count < 0 && -count + maxOutputTokens > nctx && roles.size() > 2) {
            roles.erase(roles.begin() + 1); contents.erase(contents.begin() + 1);
            while (roles.size() > 2 && roles[1] == "assistant") {
                roles.erase(roles.begin() + 1); contents.erase(contents.begin() + 1);
            }
            prompt = searchEnabled ? searchPrompt() : formatConversation(); text = prompt.toUtf8();
            count = api.llama_tokenize(vocab, text.constData(), int(text.size()), nullptr, 0, true, true);
        }
        if (count >= 0) throw std::runtime_error("LLM tokenization failed");
        std::vector<llama_token> tokens(-count);
        count = api.llama_tokenize(vocab, text.constData(), int(text.size()), tokens.data(), int(tokens.size()), true, true);
        if (count <= 0 || count + maxOutputTokens > nctx) throw std::runtime_error("Conversation exceeds the native LLM context; shorten the prompt or clear history");
        int reused = 0;
        {
            auto lease = InferenceScheduler::gpu().acquire(false, [&] { return api.cancelled(); });
            if (!lease) return;
            const auto memory = api.llama_get_memory(api.context);
            const auto limit = std::min(tokens.size(), api.decodedTokens.size());
            while (size_t(reused) < limit && tokens[reused] == api.decodedTokens[reused]) ++reused;
            // Always decode the final prompt token to refresh logits. Logits from
            // a previous generation or a removed suffix do not describe this prompt.
            reused = std::min(reused, count - 1);
            if (reused > 0 && (api.llama_memory_seq_pos_min(memory, 0) != 0 ||
                    api.llama_memory_seq_pos_max(memory, 0) + 1 != llama_pos(api.decodedTokens.size()))) reused = 0;
            if (reused > 0 && size_t(reused) < api.decodedTokens.size() &&
                    !api.llama_memory_seq_rm(memory, 0, reused, -1)) reused = 0;
            // Recurrent backends may not support removing a partial sequence.
            // In that case, and after cancellation/errors, rebuild KV from scratch.
            if (!reused) api.llama_memory_clear(memory, true);
            api.decodedTokens.resize(reused);
            api.llama_synchronize(api.context);
        }
        qInfo() << "LLM incremental prefill: reused" << reused << "decode" << count - reused << "total" << count;
        for (int pos = reused; pos < count; pos += 128) {
            auto lease = InferenceScheduler::gpu().acquire(false, [&] { return api.cancelled(); });
            if (!lease) return;
            const auto result = api.decodeTokens(tokens.data() + pos, std::min(128, count - pos));
            if (result != 0) {
                if (api.cancelled()) return; throw std::runtime_error("LLM prompt decoding failed");
            }
        }
        auto sampler = api.llama_sampler_chain_init(api.llama_sampler_chain_default_params());
        if (!sampler) throw std::runtime_error("LLM sampler initialization failed");
        const auto cleanup = std::unique_ptr<llama_sampler, std::function<void(llama_sampler*)>>(sampler, api.llama_sampler_free);
        if (structured) {
            auto grammar = api.llama_sampler_init_grammar(vocab,
                summarizing ? ConversationSummary::grammar() : System1OutputParser::grammar(), "root");
            if (!grammar) throw std::runtime_error("System 1 JSON grammar initialization failed");
            api.llama_sampler_chain_add(sampler, grammar);
        }
        // Penalize candidates before top-k/top-p. A bounded, moderate penalty avoids
        // pushing common Japanese particles out of the vocabulary across a long chat.
        constexpr int penaltyWindow = 256;
        if (penalized) api.llama_sampler_chain_add(sampler,
            api.llama_sampler_init_penalties(api.llama_vocab_n_tokens(vocab), penaltyWindow,
                lfm3 ? 1.1f : lfm ? 1.05f : 1.08f, (lfm || lfm3) ? 0.0f : 0.05f, (lfm || lfm3) ? 0.0f : 0.45f));
        if (qwen) api.llama_sampler_chain_add(sampler, api.llama_sampler_init_temp(0.7f));
        if (lfm3) {
            api.llama_sampler_chain_add(sampler, api.llama_sampler_init_top_k(50));
            api.llama_sampler_chain_add(sampler, api.llama_sampler_init_temp(0.1f));
        } else if (lfm) {
            api.llama_sampler_chain_add(sampler, api.llama_sampler_init_temp(0.3f));
            api.llama_sampler_chain_add(sampler, api.llama_sampler_init_min_p(0.15f, 1));
        } else {
            api.llama_sampler_chain_add(sampler, api.llama_sampler_init_top_k(qwen ? 20 : gemma4 ? 64 : 40));
            api.llama_sampler_chain_add(sampler, api.llama_sampler_init_top_p(qwen ? 0.8f : gemma4 ? 0.95f : 0.9f, 1));
            if (!qwen) api.llama_sampler_chain_add(sampler, api.llama_sampler_init_temp(gemma4 ? 1.0f : 0.7f));
        }
        api.llama_sampler_chain_add(sampler, api.llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        if (penalized) {
            // The sampler is recreated for every reply. Seed only the last assistant
            // response, so repeating a whole response is penalized without penalizing
            // the user's requested wording or the system instructions.
            for (qsizetype i = history.size(); i > 0; --i) {
                const auto message = history.at(i - 1).toObject();
                if (message.value("role").toString() != "assistant") continue;
                const auto previous = message.value("content").toString().trimmed().toUtf8();
                if (previous.isEmpty()) continue;
                const auto needed = api.llama_tokenize(vocab, previous.constData(), int(previous.size()), nullptr, 0, false, false);
                if (needed < 0) {
                    std::vector<llama_token> recent(-needed);
                    const auto size = api.llama_tokenize(vocab, previous.constData(), int(previous.size()), recent.data(), int(recent.size()), false, false);
                    if (size <= 0) throw std::runtime_error("LLM repetition history tokenization failed");
                    for (int j = std::max(0, size - penaltyWindow); j < size; ++j)
                        api.llama_sampler_accept(sampler, recent[j]);
                }
                break;
            }
        }
        QStringDecoder decoder(QStringDecoder::Utf8);
        QString pending, toolCall, generatedText;
        QString firstRoundText;
        auto answerOutput = [&](const QString& text) {
            // Until we know whether the model will search, keep its draft out
            // of speech. A draft preceding a tool call is not a confirmed answer.
            if (searchEnabled && toolRound == 0) firstRoundText += text;
            else visibleOutput(responseFilter.push(text));
        };
        bool thought = false, stopped = false, collectingTool = false, toolComplete = false;
        const std::array<QString, 10> markers{
            "<think>", "</think>", "<|channel>", "<channel|>",
            "<|tool_call>", "<tool_call>", "<turn|>", "<|im_start|>",
            "<|start_header_id|>", "<|eot_id|>"};
        // Control tags may span multiple tokens. Keep incomplete prefixes out of speech.
        auto emitVisible = [&](bool flush) {
            while (!pending.isEmpty() && !stopped) {
                if (collectingTool) {
                    const auto end = pending.indexOf("<tool_call|>");
                    if (end < 0) {
                        if (flush) throw std::runtime_error("Web検索ツールの呼び出しが途中で終了しました。");
                        if (pending.size() > 4096) throw std::runtime_error("Web検索ツールの呼び出しが長すぎます。");
                        break;
                    }
                    toolCall = pending.left(end); toolComplete = true; stopped = true;
                    break;
                }
                qsizetype position = -1;
                int marker = -1;
                for (int i = 0; i < int(markers.size()); ++i) {
                    const auto found = pending.indexOf(markers[i]);
                    if (found >= 0 && (position < 0 || found < position)) { position = found; marker = i; }
                }
                if (marker >= 0) {
                    if (position > 0 && !thought && !api.cancelled()) answerOutput(pending.left(position));
                    pending.remove(0, position + markers[marker].size());
                    if (marker == 0 || marker == 2) thought = true;
                    else if (marker == 1 || marker == 3) thought = false;
                    else if (marker == 4 || marker == 5) {
                        if (!searchEnabled || marker != 4)
                            throw std::runtime_error("このモデル・設定ではWeb検索ツールを使用できません。");
                        collectingTool = true;
                    }
                    else stopped = true;
                    continue;
                }
                qsizetype held = 0;
                if (!flush) {
                    for (const auto& control : markers) {
                        for (qsizetype n = 1; n < control.size() && n <= pending.size(); ++n)
                            if (pending.endsWith(control.left(n))) held = std::max(held, n);
                    }
                }
                const auto visible = pending.size() - held;
                if (visible > 0 && !thought && !api.cancelled()) answerOutput(pending.left(visible));
                pending.remove(0, visible);
                break;
            }
        };
        for (int generated = 0; generated < maxOutputTokens && !api.cancelled(); ++generated) {
            auto lease = InferenceScheduler::gpu().acquire(false, [&] { return api.cancelled(); });
            if (!lease) return;
            auto token = api.llama_sampler_sample(sampler, api.context, -1);
            if (api.llama_vocab_is_eog(vocab, token)) break;
            std::vector<char> piece(256);
            int length = api.llama_token_to_piece(vocab, token, piece.data(), int(piece.size()), 0, true);
            if (length < 0) { piece.resize(-length); length = api.llama_token_to_piece(vocab, token, piece.data(), int(piece.size()), 0, true); }
            if (length < 0) throw std::runtime_error("LLM token conversion failed");
            // QStringDecoder::operator() returns a view that refers to its input.
            // Keep the token bytes alive until the decoded QString is materialized.
            const QByteArray tokenBytes(piece.data(), length);
            const QString fragment = decoder(tokenBytes);
            if (structured) {
                generatedText += fragment;
                visibleOutput(fragment);
                // A schema-valid object is the entire reply. Do not spend more
                // decode steps on trailing whitespace or wait for a sampled EOS.
                // The last sampled token has not been decoded, so it is correctly
                // absent from decodedTokens when the next prefill finds its prefix.
                if (fragment.contains('}') && (summarizing ? bool(ConversationSummary::parse(generatedText)) :
                        bool(System1OutputParser::parse(generatedText)))) break;
            }
            else { generatedText += fragment; pending += fragment; emitVisible(false); }
            if (stopped) break;
            const auto result = api.decodeTokens(&token, 1);
            if (result != 0) {
                if (api.cancelled()) return; throw std::runtime_error("LLM token decoding failed");
            }
        }
        if (!structured && !api.cancelled() && !stopped) emitVisible(true);
        if (api.cancelled()) return;
        if (!toolComplete) {
            if (searchEnabled && toolRound == 0) visibleOutput(responseFilter.push(firstRoundText));
            break;
        }
        if (toolRound == 2) throw std::runtime_error("Web検索の上限（1応答2回）に達しました。");
        const auto query = GemmaSearchTool::query(toolCall);
        // All GPU leases have ended before network I/O; TTS can continue while searching.
        const auto result = search(query);
        if (api.cancelled()) return;
        if (toolRound == 0) contents[0] += QString::fromUtf8(
            "\n検索結果を受信済みです。『調べます』などの挨拶や進行説明を繰り返さず、"
            "取得した要点をまとめて質問へ直接答えてください。失敗なら失敗を短く伝えてください。"
            "括弧で囲んだ演出や独り言を書かないでください。\n").toUtf8();
        const auto callEnd = generatedText.indexOf("<tool_call|>");
        toolTranscript += generatedText.left(callEnd + QString("<tool_call|>").size()) +
            "<|tool_response>response:web_search" + GemmaSearchTool::value(result) + "<tool_response|>";
        prompt = searchPrompt();
    }
    if (!api.cancelled()) visibleOutput(responseFilter.flush());
    cacheGuard.complete = !api.cancelled();
    if (!api.cancelled()) qInfo() << "LLM response complete (ms, includes TTS waits):" << generationTimer.elapsed();
}
