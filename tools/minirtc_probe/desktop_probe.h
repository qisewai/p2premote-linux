#pragma once

#include "minirtc.h"

#include <chrono>
#include <functional>
#include <memory>

namespace desktop_probe {

inline constexpr char video_stream[] = "probe_screen";
inline constexpr char input_stream[] = "probe_input";

class DesktopProbe {
public:
    DesktopProbe(bool host, bool check);
    ~DesktopProbe();

    void ReceiveInput(const char* data, size_t size);
    void ReceiveVideo(const MiniRtcVideoFrame& frame);
    bool Run(PeerPtr* peer, std::chrono::seconds duration, const std::function<bool()>& ended);

private:
    struct Session;
    std::unique_ptr<Session> session_;
};

} // namespace desktop_probe
