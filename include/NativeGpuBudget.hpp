#pragma once
#include <gguf.h>
#include <ggml-backend.h>
#include <QCoreApplication>
#include <QByteArray>
#include <QDebug>
#include <QLibrary>
#include <QString>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

// Budget before loading weights: an idle CUDA model still competes with Vulkan
// TTS for residency. Scheduling compute alone cannot prevent that slowdown.
inline int speechGpuLayers(const QString& path, int contextSize, int requested) {
    QLibrary backend(QCoreApplication::applicationDirPath() + "/ggml");
    QLibrary base(QCoreApplication::applicationDirPath() + "/ggml-base");
    backend.setLoadHints(QLibrary::PreventUnloadHint);
    base.setLoadHints(QLibrary::PreventUnloadHint);
    if (!backend.load() || !base.load()) throw std::runtime_error("GPU memory budget API could not be loaded");
    auto resolve = [&](QLibrary& library, const char* name) {
        auto symbol = library.resolve(name);
        return symbol ? symbol : base.resolve(name);
    };
#define BUDGET_API(lib, name) \
    const auto name = reinterpret_cast<decltype(&::name)>(resolve(lib, #name)); \
    if (!name) throw std::runtime_error("Missing GPU memory budget API: " #name);
    BUDGET_API(backend, ggml_backend_dev_count)
    BUDGET_API(backend, ggml_backend_dev_get)
    BUDGET_API(backend, ggml_backend_dev_type)
    BUDGET_API(backend, ggml_backend_dev_memory)
    BUDGET_API(base, gguf_init_from_file)
    BUDGET_API(base, gguf_free)
    BUDGET_API(base, gguf_find_key)
    BUDGET_API(base, gguf_get_kv_type)
    BUDGET_API(base, gguf_get_val_u32)
    BUDGET_API(base, gguf_get_val_str)
    BUDGET_API(base, gguf_get_arr_type)
    BUDGET_API(base, gguf_get_arr_n)
    BUDGET_API(base, gguf_get_arr_data)
    BUDGET_API(base, gguf_get_n_tensors)
    BUDGET_API(base, gguf_get_tensor_name)
    BUDGET_API(base, gguf_get_tensor_size)
#undef BUDGET_API
    size_t total = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        const auto device = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_GPU) continue;
        size_t available = 0;
        ggml_backend_dev_memory(device, &available, &total);
        break;
    }
    if (!total) throw std::runtime_error("LLM GPU memory could not be queried");
    const auto filename = path.toUtf8();
    const auto metadata = std::unique_ptr<gguf_context, decltype(gguf_free)>(
        gguf_init_from_file(filename.constData(), {true, nullptr}), gguf_free);
    if (!metadata) throw std::runtime_error("LLM GGUF memory metadata could not be read");
    const auto archKey = gguf_find_key(metadata.get(), "general.architecture");
    if (archKey < 0 || gguf_get_kv_type(metadata.get(), archKey) != GGUF_TYPE_STRING)
        throw std::runtime_error("LLM architecture metadata is missing");
    const auto architecture = QByteArray(gguf_get_val_str(metadata.get(), archKey));
    auto number = [&](const char* suffix, uint32_t fallback = 0) {
        const auto key = architecture + suffix;
        const auto index = gguf_find_key(metadata.get(), key.constData());
        if (index < 0) return fallback;
        const auto type = gguf_get_kv_type(metadata.get(), index);
        if (type == GGUF_TYPE_UINT32) return gguf_get_val_u32(metadata.get(), index);
        // Some supported architectures store different head sizes per block.
        // The largest value keeps the F16 cache estimate conservative.
        if (type == GGUF_TYPE_ARRAY && gguf_get_arr_type(metadata.get(), index) == GGUF_TYPE_UINT32) {
            const auto count = gguf_get_arr_n(metadata.get(), index);
            const auto values = static_cast<const uint32_t*>(gguf_get_arr_data(metadata.get(), index));
            return count ? *std::max_element(values, values + count) : fallback;
        }
        return fallback;
    };
    const auto blocks = number(".block_count");
    const auto heads = number(".attention.head_count");
    const auto kvHeads = number(".attention.head_count_kv", heads);
    const auto headSize = heads ? number(".embedding_length") / heads : 0;
    const auto keySize = number(".attention.key_length", headSize);
    const auto valueSize = number(".attention.value_length", headSize);
    if (!blocks || blocks > 1024 || !kvHeads || !keySize || !valueSize)
        throw std::runtime_error("LLM GPU memory metadata is incomplete");
    std::vector<quint64> weights(blocks, 0);
    quint64 outputWeights = 0, embeddingWeights = 0;
    for (int64_t i = 0; i < gguf_get_n_tensors(metadata.get()); ++i) {
        const auto name = QByteArray(gguf_get_tensor_name(metadata.get(), i));
        const auto bytes = quint64(gguf_get_tensor_size(metadata.get(), i));
        if (name.startsWith("blk.")) {
            bool valid = false;
            const auto layer = name.mid(4, name.indexOf('.', 4) - 4).toUInt(&valid);
            if (valid && layer < blocks) weights[layer] += bytes;
        } else if (name == "output.weight" || name == "output_norm.weight") outputWeights += bytes;
        else if (name == "token_embd.weight") embeddingWeights = bytes;
    }
    // Tied embeddings may also be used as the GPU output head.
    if (outputWeights <= 1024 * 1024) outputWeights += embeddingWeights;
    constexpr quint64 MiB = 1024 * 1024;
    // Includes Irodori + its codec, CUDA ASR, display/driver headroom. Keep a
    // further 384 MiB for LLM compute buffers and CUDA graph allocations.
    const quint64 reserved = 4096 * MiB;
    const quint64 budget = total > reserved ? total - reserved : 0;
    quint64 used = outputWeights + 384 * MiB;
    const quint64 paddedContext = ((quint64(contextSize) + 255) / 256) * 256;
    const quint64 kvPerBlock = paddedContext * kvHeads * (quint64(keySize) + valueSize) * 2; // F16 K/V
    int selected = 1; // b11461 offloads the output head before repeating blocks.
    for (int layer = int(blocks) - 1; layer >= 0 && selected < requested; --layer) {
        if (used + weights[layer] + kvPerBlock > budget) break;
        used += weights[layer] + kvPerBlock;
        ++selected;
    }
    qInfo() << "LLM speech GPU budget: layers" << selected << "/" << blocks + 1
            << "requested" << requested << "VRAM (MiB)" << total / MiB
            << "LLM estimate (MiB)" << used / MiB << "speech reserve (MiB)" << reserved / MiB;
    return selected;
}
