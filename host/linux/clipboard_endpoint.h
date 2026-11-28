#pragma once

#include "desktop_target.h"

#include <p2premote/plugin_protocol.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

#include <optional>
#include <string>

namespace p2premote::linux_host {

bool ClipboardFrame(std::string_view bytes);
int OpenClipboardEndpoint(const DesktopTarget& desktop, bool listen);
bool ClipboardPeer(int fd, uid_t expected_user, const std::string& expected_image);

} // namespace p2premote::linux_host
