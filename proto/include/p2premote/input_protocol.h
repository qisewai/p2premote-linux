#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace p2premote {

enum class InputKind : std::uint8_t {
    move = 1, button_down, button_up, wheel, key_down, key_up, text, reset, heartbeat, set_quality, cursor_ready
};

enum class InputButton : std::uint16_t { left = 1, right, middle };
enum class StreamQuality : std::uint16_t { p720 = 1, p1080, p1440 };

struct InputPacket {
    InputKind kind{};
    std::uint16_t code{};
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint32_t sequence{};
    std::uint64_t sent_ms{};
};

using InputBytes = std::array<std::uint8_t, 20>;

[[nodiscard]] InputBytes EncodeInput(const InputPacket& packet);
[[nodiscard]] std::optional<InputPacket> DecodeInput(std::span<const std::uint8_t> bytes);

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

private:
    InputReceiveClock clock_;
    std::uint32_t last_pointer_sequence_{};
};

} // namespace p2premote
