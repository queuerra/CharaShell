#pragma once
#include <memory>
#include <atomic>
#include <cstddef>

// Exactly one producer and one consumer; never reset while either is active.
template<class T, std::size_t Capacity> class RingBuffer {
    static_assert(Capacity > 1);
    std::unique_ptr<T[]> data_ = std::make_unique<T[]>(Capacity);
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
public:
    bool push(const T& value) {
        auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= Capacity - 1) return false;
        data_[w % Capacity] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& value) {
        auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        value = data_[r % Capacity];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    std::size_t size() const {
        const auto w = write_.load(std::memory_order_acquire);
        const auto r = read_.load(std::memory_order_acquire);
        return w - r;
    }
    std::size_t readPosition() const { return read_.load(std::memory_order_acquire); }
    std::size_t writePosition() const { return write_.load(std::memory_order_acquire); }
    // Consumer only. A producer snapshot discards old samples without dropping
    // samples published after the snapshot or looping over seconds of audio.
    void discardBefore(std::size_t position) {
        const auto r = read_.load(std::memory_order_relaxed);
        const auto distance = position - r;
        if (distance > 0 && distance <= Capacity - 1) read_.store(position, std::memory_order_release);
    }
};
