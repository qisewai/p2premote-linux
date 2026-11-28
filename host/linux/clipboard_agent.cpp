#include "clipboard_endpoint.h"
#include "clipboard_session.h"

#include <QApplication>

#include <systemd/sd-login.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace p2premote;
using namespace p2premote::linux_host;

namespace {
std::optional<DesktopTarget> OwnDesktop() {
    const auto uid = geteuid();
    const char* display = getenv("DISPLAY");
    if (!uid || !display || !*display) return {};
    char* id{};
    if (sd_pid_get_session(0, &id) < 0) {
        if (const char* env = getenv("XDG_SESSION_ID"); env && *env) id = strdup(env);
        else if (sd_seat_get_active("seat0", &id, nullptr) < 0) return {};
    }
    const std::unique_ptr<char, decltype(&free)> session(id, free);
    uid_t owner{};
    unsigned vt{};
    if (!id || sd_session_get_uid(id, &owner) < 0 || owner != uid ||
        sd_session_get_vt(id, &vt) < 0 || !vt) return {};
    char* value{};
    if (sd_session_get_display(id, &value) < 0) {
        const char* local_vt = getenv("XDG_VTNR");
        if (!local_vt || std::to_string(vt) != local_vt) return {};
    } else {
        const std::unique_ptr<char, decltype(&free)> seat_display(value, free);
        const std::string name(display);
        if (name != value && name != std::string(value) + ".0") return {};
    }
    return DesktopTarget{DisplayServer::x11, uid, id, vt, "/run/user/" + std::to_string(uid), 1, true};
}
}

int main(int argc, char** argv) {
    const auto desktop = OwnDesktop();
    if (!desktop) return 0;
    const int listener = OpenClipboardEndpoint(*desktop, true);
    if (listener < 0) return errno == EADDRINUSE ? 0 : 1;
    std::error_code error;
    const auto directory = std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
    if (error) return 1;
    const auto host = (directory / "p2premote_linux_host").string();
    // The idle listener owns no clipboard contents or mounts. Each accepted
    // connection gets its own Qt selection owner and FUSE lifetime.
    unsigned vt{};
    while (sd_session_get_vt(desktop->session_id.c_str(), &vt) >= 0 && vt == desktop->vt) {
        pollfd waiting{listener, POLLIN, 0};
        if (poll(&waiting, 1, 2000) <= 0) continue;
        const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) continue;
        const auto active = ActiveUserDesktop();
        if ((!ClipboardPeer(fd, 0, {}) && !ClipboardPeer(fd, desktop->uid, host)) ||
            !active || !active->valid() || !active->SameLogin(*desktop)) {
            close(fd);
            continue;
        }
        const auto child = fork();
        if (child == 0) {
            close(listener);
            const int rc = [&] {
                QApplication app(argc, argv);
                return RunHostClipboardSession(app, fd);
            }();
            close(fd);
            _exit(rc);
        }
        close(fd);
        if (child > 0) while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    close(listener);
    return 0;
}
