#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Bounded 16 kHz utterances for an offline recognizer used as live captions.
class SpeechSegmenter {
    std::vector<float> audio_, preRoll_;
    std::size_t voiced_ = 0, silence_ = 0, nextPartial_ = 4000, endpoint_ = 6400;
    float threshold_ = .025f;
    bool started_ = false;
public:
    static constexpr std::size_t maxSamples = 14 * 16000;
    struct Update { bool started = false, partial = false, final = false; };
    void configure(float threshold, double silenceSeconds) {
        threshold_ = std::isfinite(threshold) ? std::clamp(threshold, .001f, 1.f) : .025f;
        endpoint_ = std::size_t(16000 * (std::isfinite(silenceSeconds) ? std::clamp(silenceSeconds, .1, 3.) : .4));
        reset();
    }
    void reset() { audio_.clear(); preRoll_.clear(); voiced_ = silence_ = 0; nextPartial_ = 4000; started_ = false; }
    const std::vector<float>& audio() const { return audio_; }
    Update push(const float* samples, std::size_t count) {
        if (!count) return {};
        double energy = 0;
        for (std::size_t i = 0; i < count; ++i) energy += double(samples[i]) * samples[i];
        const bool voiced = std::sqrt(energy / count) >= threshold_;
        if (audio_.empty()) {
            if (!voiced) {
                preRoll_.insert(preRoll_.end(), samples, samples + count);
                if (preRoll_.size() > 3200) preRoll_.erase(preRoll_.begin(), preRoll_.end() - 3200);
                return {};
            }
            audio_.swap(preRoll_); nextPartial_ = audio_.size() + 4000;
        }
        audio_.insert(audio_.end(), samples, samples + count);
        if (voiced) { voiced_ += count; silence_ = 0; } else silence_ += count;
        Update update;
        if (!started_ && voiced_ >= 1600) { started_ = true; update.started = true; }
        if (silence_ >= endpoint_ || audio_.size() >= maxSamples) {
            if (!started_) { reset(); return {}; }
            if (audio_.size() > maxSamples) audio_.resize(maxSamples);
            update.final = true;
        } else if (started_ && audio_.size() >= nextPartial_) {
            update.partial = true; nextPartial_ = audio_.size() + 4000;
        }
        return update;
    }
};
