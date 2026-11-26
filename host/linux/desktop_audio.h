#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>

namespace p2premote::linux_host {

struct DesktopAudioState {
    std::uint64_t revision{};
    unsigned vt{};

    bool operator==(const DesktopAudioState&) const = default;
};

void StreamDesktopAudio(std::stop_token stop,
    const std::function<std::optional<DesktopAudioState>()>& state,
    const std::function<void(DesktopAudioState, const char*, std::size_t)>& send);
int RunDesktopAudioCapture();

} // namespace p2premote::linux_host
