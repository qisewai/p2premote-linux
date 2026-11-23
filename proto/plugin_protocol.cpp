#include <p2premote/plugin_protocol.h>

#include <algorithm>
#include <cstdint>

namespace p2premote {
namespace {

constexpr std::size_t header_size = 8;

} // namespace

bool IsPluginIdentifier(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 32 && std::ranges::all_of(value, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

std::optional<std::string> EncodePluginPacket(PluginPacket packet) {
    if (!IsPluginIdentifier(packet.id) || !IsPluginIdentifier(packet.type) ||
        packet.payload.size() > plugin_frame_limit - header_size - packet.id.size() - packet.type.size()) {
        return std::nullopt;
    }
    std::string frame(header_size, '\0');
    frame.reserve(header_size + packet.id.size() + packet.type.size() + packet.payload.size());
    frame[0] = 1;
    frame[1] = static_cast<char>(packet.id.size());
    frame[2] = static_cast<char>(packet.type.size());
    for (std::size_t byte = 0; byte < 4; ++byte) {
        frame[4 + byte] = static_cast<char>((packet.payload.size() >> (8 * byte)) & 0xff);
    }
    frame.append(packet.id).append(packet.type).append(packet.payload);
    return frame;
}

std::optional<PluginPacket> DecodePluginPacket(std::string_view frame) noexcept {
    if (frame.size() < header_size || frame.size() > plugin_frame_limit || frame[0] != 1 || frame[3] != 0) {
        return std::nullopt;
    }
    const auto id_size = static_cast<unsigned char>(frame[1]);
    const auto type_size = static_cast<unsigned char>(frame[2]);
    std::uint32_t payload_size{};
    for (std::size_t byte = 0; byte < 4; ++byte) {
        payload_size |= static_cast<std::uint32_t>(static_cast<unsigned char>(frame[4 + byte])) << (8 * byte);
    }
    const auto payload_offset = header_size + id_size + type_size;
    if (payload_offset > frame.size() || payload_size != frame.size() - payload_offset) return std::nullopt;
    const auto id = frame.substr(header_size, id_size);
    const auto type = frame.substr(header_size + id_size, type_size);
    if (!IsPluginIdentifier(id) || !IsPluginIdentifier(type)) return std::nullopt;
    return PluginPacket{id, type, frame.substr(payload_offset)};
}

} // namespace p2premote
