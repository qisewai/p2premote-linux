#pragma once

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>

#include "api/peer_connection_interface.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/video/video_frame.h"
#include "rtc_base/thread.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct ID3D11Device;

namespace p2premote {

class StreamEvents;
class ScreenSource;
class RtcSsl;
class InputChannel;

struct DecodedFrame {
    webrtc::VideoFrame video;
    std::int64_t queued_qpc{};
};

class RtcCall final : private webrtc::PeerConnectionObserver,
                      private webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    RtcCall(bool host, std::shared_ptr<StreamEvents> events, const ConnectionOptions& options,
        bool allow_input = false, int max_framerate = 30, ID3D11Device* video_device = nullptr);
    ~RtcCall();

    void SetLocal();
    void SetRemote(const std::string& sdp);
    void AddRemoteIce(const std::string& payload);
    std::string LocalSdp() const;
    void Submit(const webrtc::VideoFrame& frame);
    bool SetVideoBitrate(int max_bitrate_bps);
    void RequestStats(bool negotiating = false);
    bool InputReady() const;
    bool SendInput(const InputPacket& input);
    bool SendCursor(const std::string& packet);
    bool UsesHardwareEncoder() const noexcept { return hardware_encoder_->load(); }

    bool HasFrame() const;
    std::optional<DecodedFrame> TakeFrame();
    std::uint64_t ReplacedFrames() const;

private:
    void DrainInput();
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
    void OnFrame(const webrtc::VideoFrame& frame) override;

    bool host_;
    bool trickle_;
    std::shared_ptr<StreamEvents> events_;
    std::shared_ptr<std::atomic_bool> hardware_encoder_ = std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<RtcSsl> ssl_;
    std::unique_ptr<webrtc::Thread> signaling_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<ScreenSource> source_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_track_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_;
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> video_sender_;
    std::unique_ptr<InputChannel> controls_;
    std::unique_ptr<InputChannel> motion_;
    webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> input_tasks_ = webrtc::PendingTaskSafetyFlag::CreateDetached();
    std::mutex input_mutex_;
    std::vector<InputPacket> pending_input_;
    bool input_posted_{};
    mutable std::mutex mutex_;
    std::optional<DecodedFrame> latest_;
    std::uint64_t replaced_frames_{};
};

} // namespace p2premote
