#include "clipboard_endpoint.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

namespace p2premote::linux_host {
bool ClipboardFrame(std::string_view bytes) {
    const auto packet = DecodePluginPacket(bytes);
    if (!packet) return false;
    return packet->id == "clipboard-sync" || packet->id == "file-clipboard" ||
        packet->id == "file-transfer" || packet->id == "remote-terminal" ||
        packet->id == "keyboard-sync";
}

int OpenClipboardEndpoint(const DesktopTarget& desktop, bool listen) {
    if (!desktop.valid() || desktop.session_id.empty()) return -1;
    const auto name = "p2premote-clipboard." + std::to_string(desktop.uid) + "." + desktop.session_id;
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
