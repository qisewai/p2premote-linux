#include "desktop_target.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace p2premote::linux_host {
namespace {
using CString = std::unique_ptr<char, decltype(&std::free)>;

bool Unlocked(const char* id) {
    sd_bus* bus{};
    if (sd_bus_default_system(&bus) < 0) return false;
    const std::unique_ptr<sd_bus, decltype(&sd_bus_unref)> connection(bus, sd_bus_unref);
    char* path{};
    if (sd_bus_path_encode("/org/freedesktop/login1/session", id, &path) < 0) return false;
    const CString name(path, std::free);
    int locked{};
    return sd_bus_get_property_trivial(bus, "org.freedesktop.login1", path,
        "org.freedesktop.login1.Session", "LockedHint", nullptr, 'b', &locked) >= 0 && !locked;
}
}

std::optional<DesktopTarget> ActiveUserDesktop() {
    char* id{};
    uid_t uid{};
    if (sd_seat_get_active("seat0", &id, &uid) < 0) return {};
    const CString session(id, std::free);
    if (!id || uid == 0) return {};
    char* type{};
    if (sd_session_get_type(id, &type) < 0) return {};
    const CString desktop_type(type, std::free);
    const auto server = std::string_view(type) == "x11" ? DisplayServer::x11 : DisplayServer::unknown;
    if (server == DisplayServer::unknown) return {};
    char* role{};
    if (sd_session_get_class(id, &role) < 0) return {};
    const CString session_class(role, std::free);
    unsigned vt{};
    if (std::string_view(role) != "user" || sd_session_get_vt(id, &vt) < 0 || !vt) return {};
    return DesktopTarget{server, uid, id, vt, "/run/user/" + std::to_string(uid), 1, Unlocked(id)};
}

} // namespace p2premote::linux_host
