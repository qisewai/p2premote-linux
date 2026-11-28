#include "backend_factory.h"

#if P2PREMOTE_LINUX_HOST_X11
#include "x11_desktop_session.h"
#include <unistd.h>
#endif

namespace p2premote::linux_host {

std::optional<BackendPreference> ParseBackend(std::string_view name) {
    if (name == "auto") return BackendPreference::automatic;
    if (name == "x11") return BackendPreference::x11;
    return {};
}

BackendProbe ProbeDesktop(BackendPreference) {
#if P2PREMOTE_LINUX_HOST_X11
    const bool desktop = DiscoverActiveXorg().has_value();
    const bool input = access("/dev/uinput", W_OK) == 0;
    return {true, desktop, input, !desktop ? "waiting_xorg" : (!input ? "input_unavailable" : "ready")};
#else
    return {false, false, false, "x11_backend_not_built"};
#endif
}

std::unique_ptr<DesktopSession> CreateDesktopSession(BackendPreference) {
#if P2PREMOTE_LINUX_HOST_X11
    return std::make_unique<X11DesktopSession>();
#else
    return {};
#endif
}

} // namespace p2premote::linux_host
