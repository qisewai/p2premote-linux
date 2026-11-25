#include "cursor_shape.h"

#include <cstdint>
#include <vector>

namespace {

constexpr std::size_t header_size = 9;

unsigned read16(std::string_view data, std::size_t offset) noexcept {
    return static_cast<unsigned char>(data[offset]) | (static_cast<unsigned char>(data[offset + 1]) << 8);
}

std::size_t maskStride(unsigned width) noexcept {
    return static_cast<std::size_t>((width + 31) / 32) * 4;
}

bool maskBit(const unsigned char* row, unsigned x) noexcept {
    return (row[x >> 3] & (0x80u >> (x & 7u))) != 0;
}

} // namespace

std::optional<DecodedCursor> decodeRemoteCursor(std::string_view data) {
    if (data.empty() || data[0] != linux_cursor_marker) return std::nullopt;
    if (data.size() == 1) return DecodedCursor{false, {}, 0, 0};
    if (data.size() < header_size) return DecodedCursor{true, {}, 0, 0};

    const unsigned width = read16(data, 1);
    const unsigned height = read16(data, 3);
    const unsigned hot_x = read16(data, 5);
    const unsigned hot_y = read16(data, 7);
    if (!width || !height || width > linux_cursor_max_dimension || height > linux_cursor_max_dimension
        || hot_x >= width || hot_y >= height) {
        return DecodedCursor{true, {}, 0, 0};
    }

    const std::size_t stride = maskStride(width);
    const std::size_t single_mask = stride * height;
    const std::size_t color_bytes = static_cast<std::size_t>(width) * height * 4;
    const bool is_color = data.size() == header_size + single_mask + color_bytes;
    if (!is_color && data.size() != header_size + single_mask * 2) {
        return DecodedCursor{true, {}, 0, 0};
    }

    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    if (image.isNull()) return DecodedCursor{true, {}, 0, 0};

    const auto* mask_data = reinterpret_cast<const unsigned char*>(data.data() + header_size);
    if (is_color) {
        const auto* pixels = reinterpret_cast<const unsigned char*>(data.data() + header_size + single_mask);
        bool has_alpha{};
        for (std::size_t i = 3; i < color_bytes; i += 4) {
            if (pixels[i] != 0) {
                has_alpha = true;
                break;
            }
        }
        for (unsigned y = 0; y < height; ++y) {
            auto* dst = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(y)));
            const auto* and_row = mask_data + y * stride;
            const auto* src = pixels + static_cast<std::size_t>(y) * width * 4;
            for (unsigned x = 0; x < width; ++x) {
                const unsigned b = src[x * 4 + 0];
                const unsigned g = src[x * 4 + 1];
                const unsigned r = src[x * 4 + 2];
                const unsigned a = src[x * 4 + 3];
                if (has_alpha) {
                    dst[x] = qRgba(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), static_cast<int>(a));
                } else if (!maskBit(and_row, x)) {
                    dst[x] = qRgba(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), 255);
                } else if (r == 0 && g == 0 && b == 0) {
                    dst[x] = qRgba(0, 0, 0, 0);
                } else {
                    dst[x] = qRgba(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), 255);
                }
            }
        }
        return DecodedCursor{true, std::move(image), static_cast<int>(hot_x), static_cast<int>(hot_y)};
    }

    const auto* xor_data = mask_data + single_mask;
    std::vector<std::uint8_t> xor_pixels(static_cast<std::size_t>(width) * height, 0);
    bool has_xor{};
    for (unsigned y = 0; y < height; ++y) {
        auto* dst = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(y)));
        const auto* and_row = mask_data + y * stride;
        const auto* xor_row = xor_data + y * stride;
        for (unsigned x = 0; x < width; ++x) {
            const bool a = maskBit(and_row, x);
            const bool v = maskBit(xor_row, x);
            if (!a && !v) {
                dst[x] = qRgba(0, 0, 0, 255);
            } else if (!a && v) {
                dst[x] = qRgba(255, 255, 255, 255);
            } else if (a && !v) {
                dst[x] = qRgba(0, 0, 0, 0);
            } else {
                // X11/Wayland bitmap cursors cannot invert the framebuffer directly;
                // render XOR pixels (such as Windows I-beam) in black with a white halo.
                dst[x] = qRgba(0, 0, 0, 255);
                xor_pixels[static_cast<std::size_t>(y) * width + x] = 1;
                has_xor = true;
            }
        }
    }
    if (has_xor) {
        for (unsigned y = 0; y < height; ++y) {
            auto* dst = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(y)));
            for (unsigned x = 0; x < width; ++x) {
                if (qAlpha(dst[x]) != 0) continue;
                const bool near_xor =
                    (x > 0 && xor_pixels[static_cast<std::size_t>(y) * width + (x - 1)]) ||
                    (x + 1 < width && xor_pixels[static_cast<std::size_t>(y) * width + (x + 1)]) ||
                    (y > 0 && xor_pixels[static_cast<std::size_t>(y - 1) * width + x]) ||
                    (y + 1 < height && xor_pixels[static_cast<std::size_t>(y + 1) * width + x]);
                if (near_xor) dst[x] = qRgba(255, 255, 255, 255);
            }
        }
    }
    return DecodedCursor{true, std::move(image), static_cast<int>(hot_x), static_cast<int>(hot_y)};
}
