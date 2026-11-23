#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace p2premote {

enum class InputKind : std::uint8_t {
    move = 1, button_down, button_up, wheel, key_down, key_up, text, reset, heartbeat, cursor_ready,
    set_video_settings, video_settings_reply
};

enum class InputButton : std::uint16_t { left = 1, right, middle };
enum class StreamQuality : std::uint16_t { p720 = 1, p1080, p1440 };
enum class VideoBitratePreset : std::uint16_t { economy = 1, automatic, detail };

constexpr int VideoBitrateLimit(VideoBitratePreset preset) noexcept {
    switch (preset) {
    case VideoBitratePreset::economy: return 4'000'000;
    case VideoBitratePreset::detail: return 16'000'000;
    case VideoBitratePreset::automatic: return 8'000'000;
    }
    return 8'000'000;
}

constexpr bool IsVideoSettingsMessage(InputKind kind) noexcept {
    return kind == InputKind::set_video_settings || kind == InputKind::video_settings_reply;
}

struct InputPacket {
    InputKind kind{};
    std::uint16_t code{};
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint32_t sequence{};
    std::uint64_t sent_ms{};
};

// Video settings use code for the resolution and x for the bitrate preset.
// Replies echo the request sequence and settings, with y set to 1 on failure.

using InputBytes = std::array<std::uint8_t, 20>;

[[nodiscard]] InputBytes EncodeInput(const InputPacket& packet);
[[nodiscard]] std::optional<InputPacket> DecodeInput(std::span<const std::uint8_t> bytes);

enum class InputCommandKind : std::uint8_t { shortcut = 1, text };
inline constexpr std::size_t shortcut_limit = 8;
inline constexpr std::size_t input_text_limit = 256;

struct InputCommand {
    InputCommandKind kind{};
    // Windows virtual keys for a shortcut, or UTF-16 code units for text.
    std::vector<std::uint16_t> units;
    std::uint64_t sent_ms{};
};

[[nodiscard]] std::vector<std::uint8_t> EncodeInputCommand(const InputCommand& command);
[[nodiscard]] std::optional<InputCommand> DecodeInputCommand(std::span<const std::uint8_t> bytes);

constexpr unsigned input_heartbeat_ms = 150;
constexpr unsigned input_lease_ms = 750;
constexpr unsigned input_max_age_ms = 2000;

class InputReceiveClock {
public:
    [[nodiscard]] bool IsFresh(std::uint64_t sent_ms, std::uint64_t received_ms);

private:
    std::optional<std::int64_t> best_offset_ms_;
};

class InputReceiveState {
public:
    [[nodiscard]] std::optional<InputPacket> Accept(const InputPacket& input, std::uint64_t received_ms);
    bool AcceptCommand(const InputCommand& command, std::uint64_t received_ms) { return clock_.IsFresh(command.sent_ms, received_ms); }

private:
    InputReceiveClock clock_;
    std::uint32_t last_pointer_sequence_{};
};

} // namespace p2premote
