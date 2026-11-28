#include "cursor_packet.h"

#include <algorithm>

namespace p2premote::linux_host {

std::string EncodeRgbaCursorPacket(unsigned width, unsigned height, unsigned hot_x, unsigned hot_y,
    std::span<const std::uint8_t> rgba) {
    constexpr std::size_t header_size = 9;
    if (!width || !height || width > cursor_max_dimension || height > cursor_max_dimension ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4) return {};

    const std::size_t mask_stride = ((width + 31u) / 32u) * 4u;
    const std::size_t mask_size = mask_stride * height;
    std::string packet(header_size + mask_size + rgba.size(), '\0');
    packet[0] = 'C';
    const auto put = [&packet](std::size_t offset, unsigned value) {
        packet[offset] = static_cast<char>(value & 0xffu);
        packet[offset + 1] = static_cast<char>(value >> 8);
    };
    put(1, width);
    put(3, height);
    put(5, std::min(hot_x, width - 1));
    put(7, std::min(hot_y, height - 1));
    auto* mask = reinterpret_cast<std::uint8_t*>(packet.data() + header_size);
    auto* bgra = mask + mask_size;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            bgra[offset] = rgba[offset + 2];
            bgra[offset + 1] = rgba[offset + 1];
            bgra[offset + 2] = rgba[offset];
            bgra[offset + 3] = rgba[offset + 3];
            if (!rgba[offset + 3]) mask[y * mask_stride + (x >> 3)] |= 0x80u >> (x & 7u);
        }
    }
    return packet;
}

} // namespace p2premote::linux_host
