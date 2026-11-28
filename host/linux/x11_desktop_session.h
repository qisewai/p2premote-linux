#pragma once

#include "uinput_injector.h"
#include "x11_shm_capturer.h"

#include <condition_variable>

namespace p2premote::linux_host {

class X11DesktopSession final : public DesktopSession {
public:
    bool Start() override { return true; }
    void Stop() noexcept override;
    bool WaitForActivity(std::chrono::microseconds timeout, std::stop_token stop) override;
    std::optional<VideoFrameView> AcquireLatestFrame() override { return capture_.GrabFrame(); }
    bool IsFrameCurrent(const VideoFrameView& frame) const override { return capture_.IsFrameCurrent(frame); }
    std::optional<CursorUpdate> PollCursor(bool force) override { return capture_.PollCursor(force); }
    void MarkFrameSent() override { capture_.MarkFrameSent(); }
    DesktopSessionState State() const override;
    DesktopTarget Target() const override { return capture_.ActiveTarget(); }
    DesktopCapabilities Capabilities() const override;
    InputSink* Input() noexcept override { return &input_; }
    std::string Describe() const override { return capture_.Describe(); }

private:
    X11ShmCapturer capture_;
    UinputInjector input_;
    std::mutex wait_mutex_;
    std::condition_variable_any timer_;
};

} // namespace p2premote::linux_host
