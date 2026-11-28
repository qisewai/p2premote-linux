#pragma once

#include "desktop_session.h"

#include <span>
#include <vector>

namespace p2premote::linux_host {

inline bool ValidFrame(const VideoFrameView& frame) noexcept {
    if (!frame.plane0 || frame.width <= 0 || frame.height <= 0 ||
        frame.width > 16384 || frame.height > 16384) return false;
    switch (frame.format) {
    case VideoPixelFormat::bgra32:
    case VideoPixelFormat::bgrx32: return frame.stride0 >= frame.width * 4;
    case VideoPixelFormat::nv12:
        return !(frame.width & 1) && !(frame.height & 1) && frame.plane1 &&
            frame.stride0 >= frame.width && frame.stride1 >= frame.width;
    }
    return false;
}

class Nv12FrameBuffer {
public:
    bool Convert(const VideoFrameView& frame, int width, int height);
    std::span<const std::uint8_t> Pixels() const noexcept { return pixels_; }

private:
    std::vector<std::uint8_t> pixels_;
    std::vector<std::uint8_t> scaled_;
};

} // namespace p2premote::linux_host
