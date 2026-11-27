#include "clipboard_endpoint.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <memory>

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

std::optional<ClipboardDesktop> ActiveClipboardDesktop() {
    char* id{};
    uid_t uid{};
    if (sd_seat_get_active("seat0", &id, &uid) < 0) return {};
    const CString session(id, std::free);
    if (!id || uid == 0) return {};
    char* type{};
    if (sd_session_get_type(id, &type) < 0) return {};
    const CString desktop_type(type, std::free);
    if (std::string_view(type) != "x11") return {};
    char* role{};
    if (sd_session_get_class(id, &role) < 0) return {};
    const CString session_class(role, std::free);
    unsigned vt{};
    if (std::string_view(role) != "user" || sd_session_get_vt(id, &vt) < 0 || !vt || !Unlocked(id)) return {};
    return ClipboardDesktop{id, vt, uid};
}

bool ClipboardFrame(std::string_view bytes) {
    const auto packet = DecodePluginPacket(bytes);
    if (!packet) return false;
    return packet->id == "clipboard-sync" || packet->id == "file-clipboard" ||
        packet->id == "file-transfer" || packet->id == "remote-terminal" ||
        packet->id == "keyboard-sync";
}

int OpenClipboardEndpoint(const ClipboardDesktop& desktop, bool listen) {
    const auto name = "p2premote-clipboard." + std::to_string(desktop.uid) + "." + desktop.session;
    sockaddr_un addr{};
    if (name.size() >= sizeof(addr.sun_path) - 1) return -1;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    const auto* address = reinterpret_cast<sockaddr*>(&addr);
    if (listen ? (bind(fd, address, size) == 0 && ::listen(fd, 2) == 0)
               : connect(fd, address, size) == 0) return fd;
    close(fd);
    return -1;
}

bool ClipboardPeer(int fd, uid_t expected_user, const std::string& expected_image) {
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) || credentials.uid != expected_user) return false;
    if (expected_image.empty()) return true;
    struct stat actual{}, registered{};
    const auto proc = "/proc/" + std::to_string(credentials.pid) + "/exe";
    return !stat(proc.c_str(), &actual) && !stat(expected_image.c_str(), &registered) &&
        actual.st_dev == registered.st_dev && actual.st_ino == registered.st_ino;
}
} // namespace p2premote::linux_host
