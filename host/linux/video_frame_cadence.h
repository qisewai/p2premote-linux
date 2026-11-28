#pragma once

#include "video_frame_conversion.h"

#include <chrono>
#include <cstring>
#include <vector>

namespace p2premote::linux_host {

enum class FrameSubmission { skip, convert, repeat };

class VideoFrameCadence {
public:
    using Clock = std::chrono::steady_clock;

    FrameSubmission Next(const VideoFrameView& frame, int width, int height, Clock::time_point now) {
        if (!ValidFrame(frame)) return FrameSubmission::skip;
        const bool planar = frame.format == VideoPixelFormat::nv12;
        const bool ignore_alpha = frame.format == VideoPixelFormat::bgrx32;
        const int row_bytes = frame.width * (planar ? 1 : 4);
        const auto luma_size = static_cast<std::size_t>(row_bytes) * frame.height;
        const auto size = luma_size + (planar ? luma_size / 2 : 0);
        if (source_width_ != frame.width || source_height_ != frame.height || format_ != frame.format ||
            output_width_ != width || output_height_ != height || pixels_.size() != size ||
            !EqualPlane(frame.plane0, frame.stride0, pixels_.data(), row_bytes, frame.height, ignore_alpha) ||
            (planar && !EqualPlane(frame.plane1, frame.stride1, pixels_.data() + luma_size, row_bytes, frame.height / 2))) {
            pixels_.resize(size);
            CopyPlane(frame.plane0, frame.stride0, pixels_.data(), row_bytes, frame.height);
            if (planar) CopyPlane(frame.plane1, frame.stride1, pixels_.data() + luma_size, row_bytes, frame.height / 2);
            source_width_ = frame.width;
            source_height_ = frame.height;
            format_ = frame.format;
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
    static bool EqualPlane(const std::uint8_t* src, int stride, const std::uint8_t* dst, int row_bytes, int rows,
        bool ignore_alpha = false) {
        if (stride == row_bytes && !ignore_alpha) {
            return std::memcmp(src, dst, static_cast<std::size_t>(row_bytes) * rows) == 0;
        }
        for (int row = 0; row < rows; ++row) {
            const auto* src_row = src + static_cast<std::size_t>(row) * stride;
            const auto* dst_row = dst + static_cast<std::size_t>(row) * row_bytes;
            if (!ignore_alpha) {
                if (std::memcmp(src_row, dst_row, row_bytes) != 0) return false;
                continue;
            }
            for (int x = 0; x < row_bytes; x += 4) {
                if (std::memcmp(src_row + x, dst_row + x, 3) != 0) return false;
            }
        }
        return true;
    }
    static void CopyPlane(const std::uint8_t* src, int stride, std::uint8_t* dst, int row_bytes, int rows) {
        if (stride == row_bytes) {
            std::memcpy(dst, src, static_cast<std::size_t>(row_bytes) * rows);
            return;
        }
        for (int row = 0; row < rows; ++row) {
            std::memcpy(dst + static_cast<std::size_t>(row) * row_bytes,
                src + static_cast<std::size_t>(row) * stride, row_bytes);
        }
    }
    std::vector<std::uint8_t> pixels_;
    VideoPixelFormat format_{};
    int source_width_{}, source_height_{};
    int output_width_{}, output_height_{};
    Clock::time_point last_submission_{};
    bool pending_{};
    unsigned refinements_{};
};

} // namespace p2premote::linux_host
