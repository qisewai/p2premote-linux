#pragma once

#include "rtc_call.h"
#include "managed_api.h"
#include "stream_events.h"

namespace p2premote {

class MiniRtcCall final : public MediaCall {
public:
    static bool Available(const ConnectionOptions& options);
    MiniRtcCall(bool host, std::shared_ptr<StreamEvents> events, const ConnectionOptions& options,
        bool allow_input, unsigned frame_rate);
    ~MiniRtcCall() override;

    void ReceiveSignal(const std::string& payload);
    static bool IsSessionSignal(const std::string& payload);
    void Submit(const webrtc::VideoFrame& frame) override;
    bool SetVideoBitrate(int max_bitrate_bps) override;
    void RequestStats(bool negotiating = false) override;
    bool InputReady() const override { return allow_input_ && connected_.load(); }
    bool SendInput(const InputPacket& input) override;
    bool SendVideoSettings(const InputPacket& settings) override;
    bool SendCursor(const std::string& packet) override;
    bool UsesHardwareEncoder() const noexcept override { return false; }
    bool HasFrame() const override;
    std::optional<DecodedFrame> TakeFrame() override;
    std::uint64_t ReplacedFrames() const override;

private:
    static void SignalOut(const char*, size_t, void*) noexcept;
    static void Status(ConnectionStatus, const char*, size_t, void*);
    static void Video(const MiniRtcVideoFrame*, const char*, size_t, const char*, size_t, void*);
    static void Data(const char*, size_t, const char*, size_t, const char*, size_t, void*);
    static void Network(const char*, size_t, TraversalMode, const MiniRtcNetTrafficStats*, const char*, size_t, void*);

    const bool host_, allow_input_;
    const std::shared_ptr<StreamEvents> events_;
    const native_minirtc::Api* api_{};
    PeerPtr* peer_{};
    std::vector<IceServerOptions> ice_;
    std::vector<std::uint8_t> nv12_;
    std::atomic_bool connected_{};
    mutable std::mutex mutex_;
    std::optional<DecodedFrame> latest_;
    std::uint64_t replaced_{};
    RtcCounters counters_;
};

} // namespace p2premote
