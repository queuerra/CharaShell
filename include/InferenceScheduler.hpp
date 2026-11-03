#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>

// CUDA and Vulkan use the same GPU. Let a waiting TTS sentence run before
// the next LLM batch, and release the GPU before waiting for audio playback.
class InferenceScheduler {
    std::mutex mutex_;
    std::condition_variable ready_;
    bool busy_ = false;
    unsigned speechWaiting_ = 0;
public:
    class Lease {
        InferenceScheduler* scheduler_ = nullptr;
    public:
        explicit Lease(InferenceScheduler* scheduler) : scheduler_(scheduler) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        explicit operator bool() const { return scheduler_ != nullptr; }
        ~Lease() {
            if (!scheduler_) return;
            {
                std::lock_guard lock(scheduler_->mutex_);
                scheduler_->busy_ = false;
            }
            scheduler_->ready_.notify_all();
        }
    };
    static InferenceScheduler& gpu() {
        static InferenceScheduler scheduler;
        return scheduler;
    }
    template<class Cancelled> Lease acquire(bool speech, Cancelled cancelled) {
        std::unique_lock lock(mutex_);
        if (speech) ++speechWaiting_;
        while (!cancelled() && (busy_ || (!speech && speechWaiting_)))
            ready_.wait_for(lock, std::chrono::milliseconds(10));
        if (speech) --speechWaiting_;
        if (cancelled()) {
            ready_.notify_all();
            return Lease(nullptr);
        }
        busy_ = true;
        return Lease(this);
    }
};
