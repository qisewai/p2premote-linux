#include "x11_desktop_session.h"

namespace p2premote::linux_host {

void X11DesktopSession::Stop() noexcept {
    input_.ReleaseAll();
    capture_.Disconnect();
}

bool X11DesktopSession::WaitForActivity(std::chrono::microseconds timeout, std::stop_token stop) {
    // XShm is sampled at the caller's cadence. The stop token wakes shutdown.
    std::unique_lock lock(wait_mutex_);
    return timer_.wait_for(lock, stop, timeout, [] { return false; });
}

DesktopSessionState X11DesktopSession::State() const {
    return capture_.HasPublishedTarget() ? DesktopSessionState::ready : DesktopSessionState::waiting_desktop;
}

DesktopCapabilities X11DesktopSession::Capabilities() const {
    const auto target = Target();
    return {true, input_.IsAvailable(), false, target.valid() && !target.session_id.empty()};
}

} // namespace p2premote::linux_host
