#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

class PcmRingBuffer {
public:
    static constexpr std::size_t channels = 2;
    static constexpr std::size_t frame_samples_per_channel = 480; // 10 ms at 48 kHz
    static constexpr std::size_t frame_samples = frame_samples_per_channel * channels; // 960 interleaved samples
    static constexpr std::size_t fade_frames = 96; // 2 ms per-channel anti-click ramp

    explicit PcmRingBuffer(std::size_t capacity_samples = frame_samples * 24,
        std::size_t prebuffer_samples = frame_samples * 5,
        std::size_t trim_target_samples = frame_samples * 7)
        : buffer_(std::max<std::size_t>(alignSamples(capacity_samples), frame_samples * 8), 0),
          prebuffer_samples_(std::min(alignSamples(prebuffer_samples), buffer_.size())),
          trim_target_samples_(std::min(alignSamples(trim_target_samples), buffer_.size())) {}

    void push(const std::int16_t* samples, std::size_t count) {
        count = alignSamples(count);
        if (!samples || !count) return;
        const std::lock_guard lock(mutex_);
        if (count >= buffer_.size()) {
            samples += count - trim_target_samples_;
            count = trim_target_samples_;
            read_pos_ = write_pos_ = count_ = 0;
            fade_in_next_ = true;
        } else if (count_ + count > buffer_.size()) {
            std::size_t keep = std::min(count_, trim_target_samples_ > count ? trim_target_samples_ - count : 0);
            keep = alignSamples(keep);
            const std::size_t drop = count_ - keep;
            // Crossfade per channel across the trim seam so clock-drift correction never clicks or swaps L/R.
            const std::size_t xf_frames = std::min<std::size_t>({fade_frames, drop / channels, keep / channels});
            for (std::size_t f = 0; f < xf_frames; ++f) {
                const float alpha = static_cast<float>(f + 1) / static_cast<float>(xf_frames + 1);
                for (std::size_t ch = 0; ch < channels; ++ch) {
                    const std::size_t offset = f * channels + ch;
                    const auto old_s = buffer_[(read_pos_ + offset) % buffer_.size()];
                    const auto new_s = buffer_[(read_pos_ + drop + offset) % buffer_.size()];
                    const int mixed = static_cast<int>(std::lround(old_s * (1.0f - alpha) + new_s * alpha));
                    buffer_[(read_pos_ + drop + offset) % buffer_.size()] =
                        static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
                }
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
        count = alignSamples(count);
        if (!destination || !count) return 0;
        const std::lock_guard lock(mutex_);
        const float gain = std::clamp(volume, 0.0f, 1.0f);

        if (!primed_ || count_ == 0) {
            primed_ = false;
            writeTailSilence(destination, count);
            return 0;
        }

        const std::size_t take = alignSamples(std::min(count_, count));
        if (take == 0) {
            primed_ = false;
            writeTailSilence(destination, count);
            return 0;
        }

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
            const std::size_t ramp_frames = std::min(take / channels, fade_frames);
            for (std::size_t f = 0; f < ramp_frames; ++f) {
                const float factor = static_cast<float>(f + 1) / static_cast<float>(ramp_frames + 1);
                for (std::size_t ch = 0; ch < channels; ++ch) {
                    const std::size_t idx = f * channels + ch;
                    destination[idx] = static_cast<std::int16_t>(std::lround(static_cast<float>(destination[idx]) * factor));
                }
            }
            fade_in_next_ = false;
        }

        for (std::size_t ch = 0; ch < channels; ++ch) {
            last_sample_[ch] = destination[take - channels + ch];
        }

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
        last_sample_.fill(0);
    }

    std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return count_;
    }

private:
    static constexpr std::size_t alignSamples(std::size_t samples) noexcept {
        return samples - (samples % channels);
    }

    void writeTailSilence(std::int16_t* destination, std::size_t count) {
        const bool non_zero = std::any_of(last_sample_.begin(), last_sample_.end(), [](std::int16_t s) { return s != 0; });
        if (non_zero) {
            const std::size_t ramp_frames = std::min(count / channels, fade_frames);
            for (std::size_t f = 0; f < ramp_frames; ++f) {
                const float factor = 1.0f - static_cast<float>(f + 1) / static_cast<float>(ramp_frames + 1);
                for (std::size_t ch = 0; ch < channels; ++ch) {
                    destination[f * channels + ch] =
                        static_cast<std::int16_t>(std::lround(static_cast<float>(last_sample_[ch]) * factor));
                }
            }
            const std::size_t written = ramp_frames * channels;
            if (count > written) std::memset(destination + written, 0, (count - written) * sizeof(std::int16_t));
            last_sample_.fill(0);
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
    std::array<std::int16_t, channels> last_sample_{};
};
