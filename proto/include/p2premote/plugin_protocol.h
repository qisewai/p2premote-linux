#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace p2premote {

inline constexpr std::size_t plugin_frame_limit = 64 * 1024;

struct PluginPacket {
    std::string_view id;
    std::string_view type;
    std::string_view payload;
};

bool IsPluginIdentifier(std::string_view value) noexcept;
std::optional<std::string> EncodePluginPacket(PluginPacket packet);
// Returned views borrow the input frame.
std::optional<PluginPacket> DecodePluginPacket(std::string_view frame) noexcept;

} // namespace p2premote
