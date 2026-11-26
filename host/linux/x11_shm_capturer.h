#pragma once

#include "capture_backend.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct xcb_connection_t;

namespace p2premote::linux_host {

struct ActiveXorgTarget {
    int pid{};
    unsigned uid{};
    std::string display;
    std::string authority;
    int vt{};

    bool operator==(const ActiveXorgTarget& other) const = default;
};

// Discovers the active VT's Xorg process (including rootless GDM/user Xorg
// started with -displayfd) by inspecting /sys/class/tty/tty0/active and /proc.
std::optional<ActiveXorgTarget> DiscoverActiveXorg();

// Encodes an XFixes ARGB cursor image into the P2P Remote wire cursor packet.
std::string EncodeXFixesCursor(unsigned width, unsigned height, unsigned hot_x, unsigned hot_y,
    const std::uint32_t* pixels);

class X11ShmCapturer final : public LinuxCaptureBackend {
public:
    X11ShmCapturer() = default;
    ~X11ShmCapturer() override;

    X11ShmCapturer(const X11ShmCapturer&) = delete;
    X11ShmCapturer& operator=(const X11ShmCapturer&) = delete;

    std::optional<CapturedFrame> GrabFrame() override;
    std::optional<CursorUpdate> PollCursor(bool force) override;
    void MarkFrameSent() noexcept override;
    unsigned ActiveFrameVt() const override;
    std::string Describe() const override;

private:
    bool EnsureConnected();
    bool AttachShm(int width, int height);
    void DetachShm() noexcept;
    void Disconnect() noexcept;

    ActiveXorgTarget current_target_;
    xcb_connection_t* display_{};
    std::uint32_t root_{};
    bool has_xfixes_{};
    std::atomic<int> frame_vt_{};

    int shmid_{-1};
    void* shmaddr_{};
    std::uint32_t shmseg_{};
    int width_{};
    int height_{};
    int stride_{};

    unsigned long last_cursor_serial_{};
    bool cursor_sent_{};
    std::chrono::steady_clock::time_point last_discovery_{};
    std::chrono::steady_clock::time_point last_geom_check_{};
};

} // namespace p2premote::linux_host
