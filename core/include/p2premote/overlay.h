#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote {

enum class OverlayAnchor { top_left, top_center, top_right, bottom_left, bottom_center, bottom_right };

inline std::optional<OverlayAnchor> ParseOverlayAnchor(std::string_view name) {
    constexpr std::string_view anchors[]{"top-left", "top-center", "top-right",
        "bottom-left", "bottom-center", "bottom-right"};
    for (int index = 0; index < 6; ++index) {
        if (anchors[index] == name) return static_cast<OverlayAnchor>(index);
    }
    return std::nullopt;
}

inline constexpr std::uint32_t overlay_default_color = 0xffffffff;

struct PluginOverlayItem {
    std::wstring id, plugin_id, title, text;
    OverlayAnchor anchor{OverlayAnchor::bottom_right};
    int priority{100};
    int font_size{11};
    std::uint32_t bg_rgb{overlay_default_color};
    std::uint32_t text_rgb{overlay_default_color};
    std::uint8_t opacity{190};
    bool monospace{};
    bool visible{true};
    bool default_visible{true};
    bool operator==(const PluginOverlayItem&) const = default;
};

struct PluginOverlaySnapshot {
    std::uint64_t revision{};
    std::vector<PluginOverlayItem> items;
};

} // namespace p2premote
