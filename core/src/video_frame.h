#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace p2premote {

struct Nv12Frame {
    int width{};
    int height{};
    int stride_y{};
    int stride_uv{};
    std::vector<std::uint8_t> data;

    [[nodiscard]] const std::uint8_t* DataY() const noexcept { return data.data(); }
    [[nodiscard]] const std::uint8_t* DataUV() const noexcept {
        return data.data() + static_cast<std::size_t>(stride_y) * height;
    }
    [[nodiscard]] std::uint8_t* MutableDataY() noexcept { return data.data(); }
    [[nodiscard]] std::uint8_t* MutableDataUV() noexcept {
        return data.data() + static_cast<std::size_t>(stride_y) * height;
    }
    [[nodiscard]] std::size_t ContiguousSize() const noexcept {
        return static_cast<std::size_t>(width) * height * 3 / 2;
    }
    [[nodiscard]] bool IsTightlyPacked() const noexcept {
        return width > 0 && height > 0 && ((width | height) & 1) == 0
            && stride_y == width && stride_uv == width && data.size() >= ContiguousSize();
    }

    [[nodiscard]] static std::shared_ptr<Nv12Frame> Create(int width, int height) {
        auto frame = std::make_shared<Nv12Frame>();
        frame->width = width;
        frame->height = height;
        frame->stride_y = width;
        frame->stride_uv = width;
        frame->data.resize(static_cast<std::size_t>(width) * height * 3 / 2);
        return frame;
    }
};

struct DecodedFrame {
    int width{};
    int height{};
    int stride_y{};
    int stride_uv{};
    const std::uint8_t* y_plane{};
    const std::uint8_t* uv_plane{};
    std::shared_ptr<void> owner;
    std::int64_t queued_qpc{};
};

} // namespace p2premote
