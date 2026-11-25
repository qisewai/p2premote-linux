#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

class PcmRingBuffer {
public:
    static constexpr std::size_t frame_samples = 480; // 10 ms at 48 kHz mono
    static constexpr std::size_t fade_samples = 96;   // 2 ms anti-click ramp

    explicit PcmRingBuffer(std::size_t capacity_samples = frame_samples * 24,
        std::size_t prebuffer_samples = frame_samples * 5,
        std::size_t trim_target_samples = frame_samples * 7)
        : buffer_(std::max<std::size_t>(capacity_samples, frame_samples * 8), 0),
          prebuffer_samples_(std::min(prebuffer_samples, buffer_.size())),
          trim_target_samples_(std::min(trim_target_samples, buffer_.size())) {}

    void push(const std::int16_t* samples, std::size_t count) {
        if (!samples || !count) return;
        const std::lock_guard lock(mutex_);
        if (count >= buffer_.size()) {
            samples += count - trim_target_samples_;
            count = trim_target_samples_;
            read_pos_ = write_pos_ = count_ = 0;
            fade_in_next_ = true;
        } else if (count_ + count > buffer_.size()) {
            const std::size_t keep = std::min(count_, trim_target_samples_ > count ? trim_target_samples_ - count : 0);
            const std::size_t drop = count_ - keep;
            // Crossfade across the trim seam so clock-drift correction never clicks.
            const std::size_t xf = std::min<std::size_t>({fade_samples, drop, keep});
            for (std::size_t i = 0; i < xf; ++i) {
                const float alpha = static_cast<float>(i + 1) / static_cast<float>(xf + 1);
                const auto old_s = buffer_[(read_pos_ + i) % buffer_.size()];
                const auto new_s = buffer_[(read_pos_ + drop + i) % buffer_.size()];
                const int mixed = static_cast<int>(std::lround(old_s * (1.0f - alpha) + new_s * alpha));
                buffer_[(read_pos_ + drop + i) % buffer_.size()] = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
            }
            read_pos_ = (read_pos_ + drop) % buffer_.size();
            count_ = keep;
        }
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[write_pos_] = samples[i];
            write_pos_ = (write_pos_ + 1) % buffer_.size();
        }
        count_ += count;
        if (!primed_ && count_ >= prebuffer_samples_) {
            primed_ = true;
            fade_in_next_ = true;
        }
    }

    std::size_t pop(std::int16_t* destination, std::size_t count, float volume = 1.0f) {
        if (!destination || !count) return 0;
        const std::lock_guard lock(mutex_);
        const float gain = std::clamp(volume, 0.0f, 1.0f);

        if (!primed_ || count_ == 0) {
            primed_ = false;
            writeTailSilence(destination, count);
            return 0;
        }

        const std::size_t take = std::min(count_, count);
        for (std::size_t i = 0; i < take; ++i) {
            std::int16_t sample = buffer_[read_pos_];
            read_pos_ = (read_pos_ + 1) % buffer_.size();
            if (gain <= 0.001f) {
                sample = 0;
            } else if (gain < 0.999f) {
                const int scaled = static_cast<int>(std::lround(static_cast<float>(sample) * gain));
                sample = static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
            }
            destination[i] = sample;
        }
        count_ -= take;

        if (fade_in_next_) {
            const std::size_t ramp = std::min(take, fade_samples);
            for (std::size_t i = 0; i < ramp; ++i) {
                const float factor = static_cast<float>(i + 1) / static_cast<float>(ramp + 1);
                destination[i] = static_cast<std::int16_t>(std::lround(static_cast<float>(destination[i]) * factor));
            }
            fade_in_next_ = false;
        }

        last_sample_ = destination[take - 1];

        if (take < count) {
            primed_ = false;
            writeTailSilence(destination + take, count - take);
        }
        return take;
    }

    void clear() {
        const std::lock_guard lock(mutex_);
        read_pos_ = write_pos_ = count_ = 0;
        primed_ = false;
        fade_in_next_ = true;
        last_sample_ = 0;
    }

    std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return count_;
    }

private:
    void writeTailSilence(std::int16_t* destination, std::size_t count) {
        if (last_sample_ != 0) {
            const std::size_t ramp = std::min(count, fade_samples);
            for (std::size_t i = 0; i < ramp; ++i) {
                const float factor = 1.0f - static_cast<float>(i + 1) / static_cast<float>(ramp + 1);
                destination[i] = static_cast<std::int16_t>(std::lround(static_cast<float>(last_sample_) * factor));
            }
            if (count > ramp) std::memset(destination + ramp, 0, (count - ramp) * sizeof(std::int16_t));
            last_sample_ = 0;
        } else {
            std::memset(destination, 0, count * sizeof(std::int16_t));
        }
    }

    mutable std::mutex mutex_;
    std::vector<std::int16_t> buffer_;
    std::size_t read_pos_{};
    std::size_t write_pos_{};
    std::size_t count_{};
    std::size_t prebuffer_samples_;
    std::size_t trim_target_samples_;
    bool primed_{};
    bool fade_in_next_{true};
    std::int16_t last_sample_{};
};
