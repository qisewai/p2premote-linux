#pragma once

#include "managed_api.h"
#include "stream_events.h"
#include "video_frame.h"

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace p2premote {

class PluginHost;

class MiniRtcCall final {
public:
    static bool Available(const ConnectionOptions& options);
    MiniRtcCall(bool host, std::shared_ptr<StreamEvents> events, const ConnectionOptions& options,
        bool allow_input, unsigned frame_rate, ID3D11Device* device);
    ~MiniRtcCall();

    void ReceiveSignal(const std::string& payload);
    static bool IsSessionSignal(const std::string& payload);
    void Submit(const Nv12Frame& frame);
    bool SetVideoBitrate(int max_bitrate_bps);
    void RequestStats();
    bool InputReady() const { return allow_input_ && connected_.load(); }
    bool SendInput(const InputPacket& input);
    bool SendVideoSettings(const InputPacket& settings);
    bool SendCursor(const std::string& packet);
    void SetPluginsConnected(bool connected) noexcept;
    bool HasFrame() const;
    std::optional<DecodedFrame> TakeFrame();
    std::uint64_t ReplacedFrames() const;

private:
    static void SignalOut(const char*, size_t, void*) noexcept;
    static void Status(ConnectionStatus, const char*, size_t, void*);
    static void Video(const MiniRtcVideoFrame*, const char*, size_t, const char*, size_t, void*);
    static void Data(const char*, size_t, const char*, size_t, const char*, size_t, void*);
    static void Network(const char*, size_t, TraversalMode, const MiniRtcNetTrafficStats*, const char*, size_t, void*);

    const bool host_, allow_input_;
    const std::shared_ptr<StreamEvents> events_;
    winrt::com_ptr<ID3D11Device> device_;
    const native_minirtc::Api* api_{};
    PeerPtr* peer_{};
    std::vector<IceServerOptions> ice_;
    std::vector<std::uint8_t> nv12_;
    std::atomic_bool connected_{};
    mutable std::mutex mutex_;
    std::optional<DecodedFrame> latest_;
    std::uint64_t replaced_{};
    RtcCounters counters_;
    std::unique_ptr<PluginHost> plugins_;
    std::atomic_bool plugin_transport_{};
};

} // namespace p2premote
