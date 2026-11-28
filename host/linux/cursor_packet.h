#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace p2premote::linux_host {

inline constexpr unsigned cursor_max_dimension = 128;

// Pixels are tightly packed, straight-alpha RGBA.
std::string EncodeRgbaCursorPacket(unsigned width, unsigned height, unsigned hot_x, unsigned hot_y,
    std::span<const std::uint8_t> rgba);

} // namespace p2premote::linux_host
