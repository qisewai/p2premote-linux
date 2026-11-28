#pragma once

#include "desktop_target.h"
#include "input_backend.h"

#include <chrono>
#include <memory>
#include <stop_token>
#include <string_view>

namespace p2premote::linux_host {

enum class VideoPixelFormat { bgra32, bgrx32, nv12 };

struct VideoFrameView {
    int width{};
    int height{};
    VideoPixelFormat format{};
    const std::uint8_t* plane0{};
    int stride0{};
    const std::uint8_t* plane1{};
    int stride1{};
    std::uint64_t sequence{};
    std::int64_t capture_ts_us{};
    // A pushed frame retains its buffer even if a producer replaces the latest slot.
    std::shared_ptr<const void> storage;
    std::uint64_t desktop_epoch{};
};

struct CursorUpdate { std::string packet; };

enum class DesktopSessionState { idle, waiting_permission, waiting_desktop, ready, revoked, error };

struct DesktopCapabilities {
    bool capture_video{};
    bool inject_input{};
    bool embedded_cursor_only{};
    bool clipboard_control{};
};

class DesktopSession {
public:
    virtual ~DesktopSession() = default;
    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;
    virtual bool WaitForActivity(std::chrono::microseconds timeout, std::stop_token stop) = 0;
    // Polled backend memory remains valid until the next acquisition or Stop.
    virtual std::optional<VideoFrameView> AcquireLatestFrame() = 0;
    virtual bool IsFrameCurrent(const VideoFrameView& frame) const = 0;
    virtual std::optional<CursorUpdate> PollCursor(bool force) = 0;
    virtual void MarkFrameSent() = 0;
    virtual DesktopSessionState State() const = 0;
    virtual DesktopTarget Target() const = 0;
    virtual DesktopCapabilities Capabilities() const = 0;
    virtual InputSink* Input() noexcept = 0;
    virtual std::string Describe() const = 0;
};

constexpr std::string_view WaitingDesktopStatus(DesktopSessionState state) noexcept {
    switch (state) {
    case DesktopSessionState::waiting_permission: return "permission";
    case DesktopSessionState::revoked: return "revoked";
    default: return "display";
    }
}

} // namespace p2premote::linux_host
