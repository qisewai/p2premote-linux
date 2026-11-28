#pragma once

#include "desktop_target.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace p2premote::linux_host {

struct DesktopAudioState {
    std::uint64_t revision{};
    DesktopTarget target;

    bool operator==(const DesktopAudioState&) const = default;
};

class AudioBackend {
public:
    using ActiveTarget = std::function<std::optional<DesktopAudioState>()>;
    using Send = std::function<void(DesktopAudioState, const char*, std::size_t)>;
    virtual ~AudioBackend() = default;
    virtual void Capture(std::stop_token stop, const ActiveTarget& state, const Send& send) = 0;
};

std::unique_ptr<AudioBackend> CreateDesktopAudio();
int RunDesktopAudioCapture();

} // namespace p2premote::linux_host
