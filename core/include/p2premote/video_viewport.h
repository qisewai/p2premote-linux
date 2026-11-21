#pragma once

#include <algorithm>
#include <cmath>

namespace p2premote {

struct VideoViewport {
    unsigned left{};
    unsigned top{};
    unsigned width{};
    unsigned height{};
};

inline VideoViewport FitVideo(unsigned target_width, unsigned target_height, unsigned width, unsigned height) {
    if (!target_width || !target_height || !width || !height) {
        return {};
    }
    const auto scale = std::min(static_cast<double>(target_width) / width, static_cast<double>(target_height) / height);
    const auto display_width = std::clamp(static_cast<unsigned>(std::lround(width * scale)), 1u, target_width);
    const auto display_height = std::clamp(static_cast<unsigned>(std::lround(height * scale)), 1u, target_height);
    return {(target_width - display_width) / 2, (target_height - display_height) / 2, display_width, display_height};
}

} // namespace p2premote
