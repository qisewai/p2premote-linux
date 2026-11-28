#include "video_frame_conversion.h"

#include <libyuv.h>

namespace p2premote::linux_host {

bool Nv12FrameBuffer::Convert(const VideoFrameView& frame, int width, int height) {
    if (!ValidFrame(frame) || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        (width & 1) || (height & 1)) return false;
    const auto luma_size = static_cast<std::size_t>(width) * height;
    pixels_.resize(luma_size + luma_size / 2);
    auto* y = pixels_.data();
    auto* uv = y + luma_size;
    const bool scale = width != frame.width || height != frame.height;
    if (frame.format == VideoPixelFormat::nv12) {
        if (scale) {
            return libyuv::NV12Scale(frame.plane0, frame.stride0, frame.plane1, frame.stride1,
                frame.width, frame.height, y, width, uv, width, width, height, libyuv::kFilterBilinear) == 0;
        }
        libyuv::CopyPlane(frame.plane0, frame.stride0, y, width, width, height);
        libyuv::CopyPlane(frame.plane1, frame.stride1, uv, width, width, height / 2);
        return true;
    }
    auto* src = frame.plane0;
    auto stride = frame.stride0;
    if (scale) {
        scaled_.resize(luma_size * 4);
        if (libyuv::ARGBScale(src, stride, frame.width, frame.height, scaled_.data(), width * 4,
            width, height, libyuv::kFilterBilinear) != 0) return false;
        src = scaled_.data();
        stride = width * 4;
    }
    return libyuv::ARGBToNV12(src, stride, y, width, uv, width, width, height) == 0;
}

} // namespace p2premote::linux_host
