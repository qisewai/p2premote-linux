#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace p2premote::linux_host {

enum class DisplayServer { unknown, x11, wayland, win32, macos };

struct DesktopTarget {
    DisplayServer server{};
    std::uint32_t uid{};
    std::string session_id;
    unsigned vt{};
    std::string runtime_dir;
    std::uint64_t epoch{};
    bool unlocked{};

    bool valid() const noexcept { return server != DisplayServer::unknown && epoch && unlocked; }
    bool operator==(const DesktopTarget&) const = default;
    bool SameLogin(const DesktopTarget& other) const noexcept {
        return server == other.server && !session_id.empty() && session_id == other.session_id &&
            uid == other.uid && vt == other.vt && runtime_dir == other.runtime_dir;
    }
};

// The active graphical user, including whether desktop integration is permitted.
std::optional<DesktopTarget> ActiveUserDesktop();

} // namespace p2premote::linux_host
