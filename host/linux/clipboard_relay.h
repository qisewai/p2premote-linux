#pragma once
#include "desktop_target.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace p2premote::linux_host {

class ClipboardBridge {
public:
    using ActiveTarget = std::function<std::optional<DesktopTarget>()>;
    using Send = std::function<bool(const DesktopTarget&, std::string_view)>;
    virtual ~ClipboardBridge() = default;
    virtual void Receive(std::string_view bytes) = 0;
};

std::unique_ptr<ClipboardBridge> CreateClipboardBridge(ClipboardBridge::ActiveTarget target, ClipboardBridge::Send send);

} // namespace p2premote::linux_host
