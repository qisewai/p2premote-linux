#pragma once

#include "managed_api.h"
#include "stream_events.h"
#include "video_frame.h"

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>

#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote {

class PluginHost;
class PluginOverlays;

class MiniRtcCall final {
public:
    static bool Available(const ConnectionOptions& options);
    MiniRtcCall(bool host, std::shared_ptr<StreamEvents> events, const ConnectionOptions& options,
        bool allow_input, unsigned frame_rate, ID3D11Device* device, std::shared_ptr<PluginOverlays> overlays = {},
        bool diagnostics = false);
    ~MiniRtcCall();

    void ReceiveSignal(const std::string& payload);
    static bool IsSessionSignal(const std::string& payload);
    void Submit(const Nv12Frame& frame);
    bool SetVideoBitrate(int max_bitrate_bps);
    void RequestStats();
    bool InputReady() const { return allow_input_ && connected_.load(); }
    bool SendInput(const InputPacket& input);
    bool SendInputCommand(const InputCommand& command);
    bool WorkspaceInputReady() const noexcept { return workspace_peer_.load(); }
    using AudioReceiver = std::function<void(std::string_view source, const char* data, std::size_t size)>;

    bool SendVideoSettings(const InputPacket& settings);
    bool SendAudioSettings(const InputPacket& settings);
    bool SendAudio(const char* data, std::size_t size, const char* stream_id);
    bool SetAudioSendPaused(bool paused);
    void SetAudioReceiver(AudioReceiver receiver);
    bool SendCursor(const std::string& packet);
    void SetPluginsConnected(bool connected) noexcept;
    void ProbeWorkspace() noexcept;
    PluginHost* Plugins() const noexcept { return plugins_.get(); }
    bool HasFrame() const;
    std::optional<DecodedFrame> TakeFrame();
    std::uint64_t ReplacedFrames() const;

private:
    static void SignalOut(const char*, size_t, void*) noexcept;
    static void Status(ConnectionStatus, const char*, size_t, void*);
    static void Video(const MiniRtcVideoFrame*, const char*, size_t, const char*, size_t, void*);
    static void Audio(const char*, size_t, const char*, size_t, const char*, size_t, void*);
    static void Data(const char*, size_t, const char*, size_t, const char*, size_t, void*);
    static void Network(const char*, size_t, TraversalMode, const MiniRtcNetTrafficStats*, const char*, size_t, void*);
    void SendWorkspaceMessage(std::string_view kind) noexcept;

    const bool host_, allow_input_, diagnostics_;
    const std::shared_ptr<StreamEvents> events_;
    winrt::com_ptr<ID3D11Device> device_;
    const native_minirtc::Api* api_{};
    PeerPtr* peer_{};
    std::vector<IceServerOptions> ice_;
    std::vector<std::uint8_t> nv12_;
    std::atomic_bool connected_{};
    std::bitset<4> sent_buttons_;
    std::optional<InputPacket> drag_checkpoint_;
    mutable std::mutex mutex_;
    std::optional<DecodedFrame> latest_;
    std::uint64_t replaced_{};
    RtcCounters counters_;
    std::mutex audio_mutex_;
    AudioReceiver audio_receiver_;
    std::unique_ptr<PluginHost> plugins_;
    std::atomic_bool plugin_transport_{};
    std::atomic_bool workspace_peer_{};
    std::atomic_bool workspace_running_{};
    std::atomic_bool workspace_reply_pending_{};
    ULONGLONG workspace_retry_{};
    unsigned workspace_attempts_{};
};

} // namespace p2premote
