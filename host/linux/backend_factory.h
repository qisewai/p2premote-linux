#pragma once

#include "desktop_session.h"

namespace p2premote::linux_host {

enum class BackendPreference { automatic, x11 };

struct BackendProbe {
    bool x11_built{};
    bool xorg_active{};
    bool uinput_writable{};
    std::string_view reason;
};

std::optional<BackendPreference> ParseBackend(std::string_view name);
BackendProbe ProbeDesktop(BackendPreference preference);
std::unique_ptr<DesktopSession> CreateDesktopSession(BackendPreference preference);

} // namespace p2premote::linux_host
