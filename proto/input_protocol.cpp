#include <p2premote/input_protocol.h>

#include <algorithm>
#include <limits>

namespace p2premote {
namespace {

constexpr std::uint8_t version = 1;

void WriteLittleEndian(std::span<std::uint8_t> bytes, std::uint64_t value) {
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

std::uint64_t ReadLittleEndian(std::span<const std::uint8_t> bytes) {
    std::uint64_t value{};
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        value = (value << 8) | *it;
    }
    return value;
}

bool ValidCode(InputKind kind, std::uint16_t code) {
    switch (kind) {
    case InputKind::move:
    case InputKind::reset:
    case InputKind::heartbeat:
    case InputKind::cursor_ready:
        return code == 0;
    case InputKind::button_down:
    case InputKind::button_up:
        return code >= static_cast<std::uint16_t>(InputButton::left)
            && code <= static_cast<std::uint16_t>(InputButton::middle);
    case InputKind::wheel:
        return code != 0;
    case InputKind::key_down:
    case InputKind::key_up:
        return (code & 0xff) != 0 && code < 512;
    case InputKind::text:
        return code == '\b' || code == '\r' || (code >= 32 && code != 127);
    case InputKind::set_video_settings:
    case InputKind::video_settings_reply:
        return code >= static_cast<std::uint16_t>(StreamQuality::p720)
            && code <= static_cast<std::uint16_t>(StreamQuality::p1440);
    }
    return false;
}

} // namespace

InputBytes EncodeInput(const InputPacket& packet) {
    InputBytes bytes{};
    std::span payload(bytes);
    bytes[0] = version;
    bytes[1] = static_cast<std::uint8_t>(packet.kind);
    WriteLittleEndian(payload.subspan<2, 2>(), packet.code);
    WriteLittleEndian(payload.subspan<4, 2>(), packet.x);
    WriteLittleEndian(payload.subspan<6, 2>(), packet.y);
    WriteLittleEndian(payload.subspan<8, 4>(), packet.sequence);
    WriteLittleEndian(payload.subspan<12, 8>(), packet.sent_ms);
    return bytes;
}

std::optional<InputPacket> DecodeInput(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != InputBytes{}.size() || bytes[0] != version) {
        return std::nullopt;
    }
    const auto kind = static_cast<InputKind>(bytes[1]);
    const auto code = static_cast<std::uint16_t>(ReadLittleEndian(bytes.subspan<2, 2>()));
    const auto sequence = static_cast<std::uint32_t>(ReadLittleEndian(bytes.subspan<8, 4>()));
    if (!sequence || !ValidCode(kind, code)) {
        return std::nullopt;
    }
    const auto x = static_cast<std::uint16_t>(ReadLittleEndian(bytes.subspan<4, 2>()));
    const auto y = static_cast<std::uint16_t>(ReadLittleEndian(bytes.subspan<6, 2>()));
    if (IsVideoSettingsMessage(kind) && (x < static_cast<std::uint16_t>(VideoBitratePreset::economy)
            || x > static_cast<std::uint16_t>(VideoBitratePreset::detail)
            || y > (kind == InputKind::video_settings_reply ? 1 : 0))) return std::nullopt;
    return InputPacket{kind, code, x, y, sequence, ReadLittleEndian(bytes.subspan<12, 8>())};
}

bool InputReceiveClock::IsFresh(std::uint64_t sent_ms, std::uint64_t received_ms) {
    constexpr auto max_ticks = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (sent_ms > max_ticks || received_ms > max_ticks) return false;

    // Uptime epochs differ by machine. The smallest observed offset includes the least transit delay.
    const auto offset = static_cast<std::int64_t>(received_ms) - static_cast<std::int64_t>(sent_ms);
    if (!best_offset_ms_ || offset < *best_offset_ms_) {
        best_offset_ms_ = offset;
        return true;
    }
    const auto excess = static_cast<std::uint64_t>(offset) - static_cast<std::uint64_t>(*best_offset_ms_);
    return excess <= input_max_age_ms;
}

std::optional<InputPacket> InputReceiveState::Accept(const InputPacket& input, std::uint64_t received_ms) {
    if (!clock_.IsFresh(input.sent_ms, received_ms)) {
        last_pointer_sequence_ = std::max(last_pointer_sequence_, input.sequence);
        if (input.kind == InputKind::move) return std::nullopt;
        return InputPacket{InputKind::reset};
    }
    if (input.kind == InputKind::move && input.sequence <= last_pointer_sequence_) return std::nullopt;
    if (input.kind == InputKind::move || input.kind == InputKind::button_down || input.kind == InputKind::button_up
            || input.kind == InputKind::wheel || input.kind == InputKind::reset) {
        last_pointer_sequence_ = std::max(last_pointer_sequence_, input.sequence);
    }
    return input;
}

} // namespace p2premote
