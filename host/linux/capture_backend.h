#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p2premote::linux_host {

struct CapturedFrame {
    int width{};
    int height{};
    int stride{};
    const std::uint8_t* bgra{}; // Valid until next GrabFrame() call
};

struct CursorUpdate {
    std::string packet; // Exact P2P Remote 'C' cursor packet
};

// Pluggable Linux screen capture backend interface.
// First implementation: X11ShmCapturer (Xorg + XShm + XFixes, VT-following).
// Future backends (DRM/KMS DMA-BUF, PipeWire) implement the same contract.
class LinuxCaptureBackend {
public:
    virtual ~LinuxCaptureBackend() = default;

    // Captures the current screen frame. Returns std::nullopt if no display
    // is active yet (e.g. during VT transition or GDM restart).
    virtual std::optional<CapturedFrame> GrabFrame() = 0;

    // Polls for cursor shape changes. Returns a cursor packet only when the
    // cursor changed (or when force is true).
    virtual std::optional<CursorUpdate> PollCursor(bool force) = 0;

    virtual void MarkFrameSent() noexcept = 0;

    // Safe to call off the capture thread. Zero means the last captured target is no longer active.
    virtual unsigned ActiveFrameVt() const = 0;
    bool InputAvailable() const { return ActiveFrameVt() != 0; }

    // Current backend status summary for logging.
    virtual std::string Describe() const = 0;
};

} // namespace p2premote::linux_host
