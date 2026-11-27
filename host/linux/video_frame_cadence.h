#pragma once

#include "capture_backend.h"

#include <chrono>
#include <cstring>
#include <vector>

namespace p2premote::linux_host {

enum class FrameSubmission { skip, convert, repeat };

class VideoFrameCadence {
public:
    using Clock = std::chrono::steady_clock;

    FrameSubmission Next(const CapturedFrame& frame, int width, int height, Clock::time_point now) {
        const auto size = static_cast<std::size_t>(frame.stride) * frame.height;
        if (source_width_ != frame.width || source_height_ != frame.height || source_stride_ != frame.stride ||
            output_width_ != width || output_height_ != height || pixels_.size() != size ||
            std::memcmp(pixels_.data(), frame.bgra, size) != 0) {
            pixels_.assign(frame.bgra, frame.bgra + size);
            source_width_ = frame.width;
            source_height_ = frame.height;
            source_stride_ = frame.stride;
            output_width_ = width;
            output_height_ = height;
            pending_ = true;
            refinements_ = 4;
            return FrameSubmission::convert;
        }

        // Match the Windows timing for post-motion refinement and idle refresh.
        const auto interval = refinements_ ? std::chrono::microseconds(1'000'000 / 15) : std::chrono::milliseconds(900);
        return pending_ || now - last_submission_ >= interval ? FrameSubmission::repeat : FrameSubmission::skip;
    }

    void Submitted(Clock::time_point now) noexcept {
        if (!pending_ && refinements_) --refinements_;
        pending_ = false;
        last_submission_ = now;
    }

    void Reset() noexcept {
        pixels_.clear();
        pending_ = false;
        refinements_ = 0;
    }

private:
    std::vector<std::uint8_t> pixels_;
    int source_width_{}, source_height_{}, source_stride_{};
    int output_width_{}, output_height_{};
    Clock::time_point last_submission_{};
    bool pending_{};
    unsigned refinements_{};
};

} // namespace p2premote::linux_host
