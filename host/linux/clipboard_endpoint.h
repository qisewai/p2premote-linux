#pragma once

#include <p2premote/plugin_protocol.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

#include <optional>
#include <string>

namespace p2premote::linux_host {

struct ClipboardDesktop {
    std::string session;
    unsigned vt{};
    uid_t uid{};
    bool operator==(const ClipboardDesktop&) const = default;
};

std::optional<ClipboardDesktop> ActiveClipboardDesktop();
bool ClipboardFrame(std::string_view bytes);
int OpenClipboardEndpoint(const ClipboardDesktop& desktop, bool listen);
bool ClipboardPeer(int fd, uid_t expected_user, const std::string& expected_image);

} // namespace p2premote::linux_host
