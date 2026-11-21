#include "rtc_call.h"
#include "cursor_shape.h"

#include "qpc.h"
#include "stream_events.h"
#include "mf_h264.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/make_ref_counted.h"
#include "api/rtc_event_log/rtc_event_log_factory_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/video/video_broadcaster.h"
#include "logging/rtc_event_log/events/rtc_event_bwe_update_delay_based.h"
#include "logging/rtc_event_log/events/rtc_event_bwe_update_loss_based.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_incoming.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_outgoing.h"
#include "pc/video_track_source.h"
#include "rtc_base/ssl_adapter.h"

#include <charconv>
#include <stdexcept>

namespace p2premote {
namespace {

using Peer = webrtc::PeerConnectionInterface;
constexpr char control_label[] = "p2premote.input.v1";
constexpr char motion_label[] = "p2premote.motion.v1";
// The screenshare ALR preset takes precedence over WebRTC-Video-Pacing.
// Keep the probing thresholds and use a higher pacing factor and a short drain target on LAN.
constexpr char lan_screenshare_trials[] = "WebRTC-ProbingScreenshareBwe/4.0,30,80,40,-60,3/";

class PacketTrace final : public webrtc::RtcEventLog {
public:
    explicit PacketTrace(std::shared_ptr<LatencyTrace> trace) : trace_(std::move(trace)) {}

    bool StartLogging(std::unique_ptr<webrtc::RtcEventLogOutput>, int64_t) override { return false; }
    void StopLogging() override {}

    void Log(std::unique_ptr<webrtc::RtcEvent> event) override {
        if (event->GetType() == webrtc::RtcEvent::Type::RtpPacketOutgoing) {
            Record("rtp_out", static_cast<const webrtc::RtcEventRtpPacketOutgoing&>(*event));
        } else if (event->GetType() == webrtc::RtcEvent::Type::RtpPacketIncoming) {
            Record("rtp_in", static_cast<const webrtc::RtcEventRtpPacketIncoming&>(*event));
        } else if (event->GetType() == webrtc::RtcEvent::Type::BweUpdateDelayBased) {
            const auto& update = static_cast<const webrtc::RtcEventBweUpdateDelayBased&>(*event);
            if (update.bitrate_bps() > 0) trace_->Record("bwe_delay_target_bps", update.bitrate_bps(),
                static_cast<unsigned>(update.detector_state()));
        } else if (event->GetType() == webrtc::RtcEvent::Type::BweUpdateLossBased) {
            const auto& update = static_cast<const webrtc::RtcEventBweUpdateLossBased&>(*event);
            if (update.bitrate_bps() > 0) trace_->Record("bwe_loss_target_bps", update.bitrate_bps(), update.fraction_loss());
        }
    }

private:
    template <typename Packet>
    void Record(const char* name, const Packet& packet) {
        if (!packet.payload_length()) return;
        const auto stream_and_sequence = (static_cast<std::uint64_t>(packet.Ssrc()) << 16) | packet.SequenceNumber();
        const auto flags = (packet.Marker() ? 1u : 0u) | (packet.rtx_original_sequence_number() ? 2u : 0u);
        trace_->Record(name, packet.Timestamp(), stream_and_sequence, packet.packet_length(), flags);
    }

    std::shared_ptr<LatencyTrace> trace_;
};

class PacketTraceFactory final : public webrtc::RtcEventLogFactoryInterface {
public:
    explicit PacketTraceFactory(std::shared_ptr<LatencyTrace> trace) : trace_(std::move(trace)) {}

    std::unique_ptr<webrtc::RtcEventLog> Create(const webrtc::Environment&) const override {
        return std::make_unique<PacketTrace>(trace_);
    }

private:
    std::shared_ptr<LatencyTrace> trace_;
};

void Check(const webrtc::RTCError& error, const char* operation) {
    if (!error.ok()) {
        throw std::runtime_error(std::string(operation) + " · " + error.message());
    }
}

class LocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
public:
    explicit LocalObserver(std::shared_ptr<StreamEvents> events) : events_(std::move(events)) {}

    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
        if (error.ok()) {
            events_->Push({StreamEventType::local_set, {}, {}});
        } else {
            events_->Push({StreamEventType::error, {}, error.message()});
        }
    }

private:
    std::shared_ptr<StreamEvents> events_;
};

class RemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    explicit RemoteObserver(std::shared_ptr<StreamEvents> events) : events_(std::move(events)) {}

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (error.ok()) {
            events_->Push({StreamEventType::remote_set, {}, {}});
        } else {
            events_->Push({StreamEventType::error, {}, error.message()});
        }
    }

private:
    std::shared_ptr<StreamEvents> events_;
};

class StatsObserver : public webrtc::RTCStatsCollectorCallback {
public:
    StatsObserver(std::shared_ptr<StreamEvents> events, bool negotiating)
        : events_(std::move(events)), negotiating_(negotiating) {}

    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        RtcCounters counters;
        if (negotiating_) {
            auto& checks = counters.ice_checks.emplace();
            for (const auto* pair : report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
                checks.requests_sent += pair->requests_sent.value_or(0);
                checks.requests_received += pair->requests_received.value_or(0);
                checks.responses_received += pair->responses_received.value_or(0);
            }
            for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
                if (transport->dtls_state) checks.dtls_state = *transport->dtls_state;
                if (transport->selected_candidate_pair_id) break;
            }
            events_->Push({StreamEventType::stats, {}, {}, std::move(counters)});
            return;
        }
        const auto& trace = events_->Trace();
        for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
            counters.bytes_sent += transport->bytes_sent.value_or(0);
            counters.bytes_received += transport->bytes_received.value_or(0);
            if (counters.ice_path != IcePath::unknown || !transport->selected_candidate_pair_id) continue;
            const auto* pair = report->GetAs<webrtc::RTCIceCandidatePairStats>(*transport->selected_candidate_pair_id);
            if (!pair || !pair->local_candidate_id || !pair->remote_candidate_id) continue;
            const auto* local = report->GetAs<webrtc::RTCLocalIceCandidateStats>(*pair->local_candidate_id);
            const auto* remote = report->GetAs<webrtc::RTCRemoteIceCandidateStats>(*pair->remote_candidate_id);
            if (!local || !remote || !local->candidate_type || !remote->candidate_type) continue;
            if (*local->candidate_type == "relay" || *remote->candidate_type == "relay") {
                counters.ice_path = IcePath::relay;
            } else if (*local->candidate_type == "host" && *remote->candidate_type == "host") {
                counters.ice_path = IcePath::host;
            } else {
                counters.ice_path = IcePath::nat;
            }
            if (pair->current_round_trip_time) counters.ice_rtt_ms = *pair->current_round_trip_time * 1000;
            if (trace && pair->available_outgoing_bitrate.value_or(0) > 0) {
                trace->Record("available_outgoing_bps", static_cast<std::uint64_t>(*pair->available_outgoing_bitrate));
            }
        }
        for (const auto* row : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
            if (row->codec_id && row->frames_decoded) {
                const auto* codec = report->GetAs<webrtc::RTCCodecStats>(*row->codec_id);
                if (codec && codec->mime_type) counters.video_codec = *codec->mime_type;
            }
            counters.received += row->frames_received.value_or(0);
            counters.decoded += row->frames_decoded.value_or(0);
            counters.decode_seconds += row->total_decode_time.value_or(0);
            counters.rtc_frames_dropped += row->frames_dropped.value_or(0);
            if (row->jitter_buffer_delay && row->jitter_buffer_emitted_count) {
                counters.jitter_buffer_seconds += *row->jitter_buffer_delay;
                counters.jitter_buffer_frames += *row->jitter_buffer_emitted_count;
            }
        }
        for (const auto* row : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
            if (row->codec_id && row->frames_encoded) {
                const auto* codec = report->GetAs<webrtc::RTCCodecStats>(*row->codec_id);
                if (codec && codec->mime_type) counters.video_codec = *codec->mime_type;
            }
            counters.encoded += row->frames_encoded.value_or(0);
            counters.encode_seconds += row->total_encode_time.value_or(0);
            if (row->total_packet_send_delay && row->packets_sent) {
                counters.packet_send_queue_seconds += *row->total_packet_send_delay;
                counters.sent_packets += *row->packets_sent;
            }
            if (trace && row->frames_encoded && row->target_bitrate.value_or(0) > 0) {
                trace->Record("encoder_target_bps", static_cast<std::uint64_t>(*row->target_bitrate));
            }
        }
        if (trace) {
            if (counters.sent_packets) trace->Record("send_queue_total", counters.sent_packets,
                static_cast<std::uint64_t>(counters.packet_send_queue_seconds * 1'000'000));
            if (counters.jitter_buffer_frames) trace->Record("receive_buffer_total", counters.jitter_buffer_frames,
                static_cast<std::uint64_t>(counters.jitter_buffer_seconds * 1'000'000));
        }
        events_->Push({StreamEventType::stats, {}, {}, counters});
    }

private:
    std::shared_ptr<StreamEvents> events_;
    bool negotiating_;
};

} // namespace

class InputChannel final : public webrtc::DataChannelObserver {
public:
    InputChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel,
                 std::shared_ptr<StreamEvents> events, bool receive, bool motion)
        : channel_(std::move(channel)), events_(std::move(events)), receive_(receive), motion_(motion) {
        channel_->RegisterObserver(this);
    }

    ~InputChannel() override { channel_->UnregisterObserver(); }

    bool IsOpen() const { return channel_->state() == webrtc::DataChannelInterface::kOpen; }

    bool Send(const InputPacket& input) {
        const auto& trace = events_->Trace();
        if (!IsOpen() || channel_->buffered_amount() > (motion_ ? 512u : 16'384u)) {
            if (trace) trace->Pointer("input_rejected", input);
            return false;
        }
        const auto bytes = EncodeInput(input);
        const auto sent = channel_->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true));
        if (trace) trace->Pointer(sent ? "input_channel_accepted" : "input_rejected", input);
        return sent;
    }

    bool SendCursor(const std::string& packet) {
        if (!IsOpen() || channel_->buffered_amount() >= cursor_max_packet) return false;
        return channel_->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(packet.data(), packet.size()), true));
    }

private:
    void OnStateChange() override { events_->Push({StreamEventType::input_ready, {}, {}}); }

    void OnMessage(const webrtc::DataBuffer& buffer) override {
        if (!buffer.binary) return;
        if (!receive_) {
            const auto size = buffer.data.size();
            const auto* data = buffer.data.data<char>();
            if (!motion_ && size && size <= cursor_max_packet && data[0] == cursor_marker) {
                events_->Push({StreamEventType::cursor, {}, std::string(data, size)});
            }
            return;
        }
        const auto input = DecodeInput({buffer.data.data<std::uint8_t>(), buffer.data.size()});
        if (input && (input->kind == InputKind::move) == motion_) {
            if (const auto& trace = events_->Trace()) trace->Pointer("input_received", *input);
            events_->PushInput(*input);
        }
    }

    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
    std::shared_ptr<StreamEvents> events_;
    bool receive_;
    bool motion_;
};

class RtcSsl {
public:
    RtcSsl() {
        if (!webrtc::InitializeSSL()) {
            throw std::runtime_error("无法初始化 WebRTC DTLS");
        }
    }
    ~RtcSsl() { webrtc::CleanupSSL(); }
};

class ScreenSource : public webrtc::VideoTrackSource {
public:
    ScreenSource() : VideoTrackSource(false) {}
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }
    void Submit(const webrtc::VideoFrame& frame) { broadcaster_.OnFrame(frame); }

private:
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override { return &broadcaster_; }
    webrtc::VideoBroadcaster broadcaster_;
};

RtcCall::RtcCall(bool host, std::shared_ptr<StreamEvents> events, const ConnectionOptions& options,
    bool allow_input, int max_framerate, ID3D11Device* video_device)
    : host_(host), trickle_(options.mode == ConnectionMode::internet), events_(std::move(events)),
      ssl_(std::make_unique<RtcSsl>()), signaling_(webrtc::Thread::Create()) {
    if (!signaling_->Start()) {
        throw std::runtime_error("无法启动 WebRTC 信令线程");
    }
    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.signaling_thread = signaling_.get();
    if (const auto& trace = events_->Trace()) dependencies.event_log_factory = std::make_unique<PacketTraceFactory>(trace);
    if (host && options.mode == ConnectionMode::lan_direct) {
        dependencies.env = webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>(lan_screenshare_trials));
    } else if (!host && (options.test_playout_delay || options.mode == ConnectionMode::lan_direct)) {
        dependencies.env = webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>(
            options.test_playout_delay ? "WebRTC-ForcePlayoutDelay/min_ms:0,max_ms:0/"
                                       : "WebRTC-ForcePlayoutDelay/min_ms:0,max_ms:30/"));
    }
    dependencies.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    dependencies.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    dependencies.video_encoder_factory = CreateVideoEncoderFactory(host ? video_device : nullptr, hardware_encoder_);
    dependencies.video_decoder_factory = CreateVideoDecoderFactory(host ? nullptr : video_device);
    webrtc::EnableMedia(dependencies);
    factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
    if (!factory_) {
        throw std::runtime_error("无法创建 WebRTC 工厂");
    }
    Peer::RTCConfiguration config;
    if (options.relay_only) config.type = Peer::kRelay;
    for (const auto& ice : options.ice_servers) {
        Peer::IceServer server;
        server.urls = ice.urls;
        server.username = ice.username;
        server.password = ice.credential;
        config.servers.push_back(std::move(server));
    }
    auto created = factory_->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(this));
    Check(created.error(), "无法创建 PeerConnection");
    peer_ = created.MoveValue();

    if (host_) {
        if (options.mode == ConnectionMode::lan_direct) {
            // Seed LAN bandwidth estimation above the Internet startup rate. Feedback can still lower it.
            webrtc::BitrateSettings bitrate;
            bitrate.start_bitrate_bps = 6'000'000;
            Check(peer_->SetBitrate(bitrate), "无法配置局域网起始码率");
        }
        source_ = webrtc::make_ref_counted<ScreenSource>();
        const auto track = factory_->CreateVideoTrack(source_, "screen");
        track->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kText);
        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
        auto transceiver = peer_->AddTransceiver(track, init);
        Check(transceiver.error(), "无法添加屏幕视频轨");
        video_sender_ = transceiver.value()->sender();
        auto parameters = video_sender_->GetParameters();
        parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
        parameters.encodings.front().max_framerate = max_framerate;
        parameters.encodings.front().max_bitrate_bps = 4'000'000;
        Check(video_sender_->SetParameters(parameters), "无法配置视频发送参数");

        if (allow_input) {
            signaling_->BlockingCall([this, lan = options.mode == ConnectionMode::lan_direct] {
                webrtc::DataChannelInit reliable;
                webrtc::DataChannelInit latest_motion;
                latest_motion.ordered = false;
                latest_motion.maxRetransmits = 0;
                if (lan) {
                    reliable.priority = webrtc::PriorityValue(webrtc::Priority::kHigh);
                    latest_motion.priority = webrtc::PriorityValue(webrtc::Priority::kMedium);
                }
                auto controls = peer_->CreateDataChannelOrError(control_label, &reliable);
                Check(controls.error(), "无法创建键鼠控制通道");
                auto motion = peer_->CreateDataChannelOrError(motion_label, &latest_motion);
                Check(motion.error(), "无法创建鼠标移动通道");
                controls_ = std::make_unique<InputChannel>(controls.MoveValue(), events_, true, false);
                motion_ = std::make_unique<InputChannel>(motion.MoveValue(), events_, true, true);
            });
        }
    }
}

RtcCall::~RtcCall() {
    signaling_->BlockingCall([this] {
        input_tasks_->SetNotAlive();
        motion_.reset();
        controls_.reset();
        if (remote_track_) {
            remote_track_->RemoveSink(this);
            remote_track_ = nullptr;
        }
        video_sender_ = nullptr;
        peer_->Close();
    });
    peer_ = nullptr;
}

void RtcCall::SetLocal() {
    peer_->SetLocalDescription(webrtc::make_ref_counted<LocalObserver>(events_));
}

void RtcCall::SetRemote(const std::string& sdp) {
    auto description = webrtc::CreateSessionDescription(host_ ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer, sdp);
    if (!description) {
        throw std::runtime_error("无法解析远端 SDP");
    }
    peer_->SetRemoteDescription(std::move(description), webrtc::make_ref_counted<RemoteObserver>(events_));
}

void RtcCall::AddRemoteIce(const std::string& payload) {
    const auto mid_end = payload.find('\n');
    const auto index_end = mid_end == std::string::npos ? mid_end : payload.find('\n', mid_end + 1);
    if (index_end == std::string::npos) throw std::runtime_error("远端 ICE 候选格式无效");
    int index{};
    const auto last = payload.data() + index_end;
    const auto parsed = std::from_chars(payload.data() + mid_end + 1, last, index);
    if (parsed.ec != std::errc{} || parsed.ptr != last || index < 0) {
        throw std::runtime_error("远端 ICE 候选索引无效");
    }
    auto candidate = webrtc::IceCandidate::Create(payload.substr(0, mid_end), index, payload.substr(index_end + 1));
    if (!candidate) throw std::runtime_error("无法解析远端 ICE 候选");
    peer_->AddIceCandidate(std::move(candidate), [events = events_](webrtc::RTCError error) {
        if (!error.ok()) events->Push({StreamEventType::error, {}, "无法应用远端 ICE 候选"});
    });
}

std::string RtcCall::LocalSdp() const {
    return signaling_->BlockingCall([this] {
        std::string sdp;
        if (const auto* description = peer_->local_description(); description && description->ToString(&sdp)) {
            return sdp;
        }
        throw std::runtime_error("无法生成本地 SDP");
    });
}

void RtcCall::Submit(const webrtc::VideoFrame& frame) { source_->Submit(frame); }

bool RtcCall::SetVideoBitrate(int max_bitrate_bps) {
    return signaling_->BlockingCall([this, max_bitrate_bps] {
        auto parameters = video_sender_->GetParameters();
        if (parameters.encodings.empty()) return false;
        parameters.encodings.front().max_bitrate_bps = max_bitrate_bps;
        return video_sender_->SetParameters(parameters).ok();
    });
}

void RtcCall::RequestStats(bool negotiating) {
    const auto observer = webrtc::make_ref_counted<StatsObserver>(events_, negotiating);
    peer_->GetStats(observer.get());
}

bool RtcCall::InputReady() const {
    return signaling_->BlockingCall([this] {
        return controls_ && motion_ && controls_->IsOpen() && motion_->IsOpen();
    });
}

bool RtcCall::SendInput(const InputPacket& input) {
    if (host_) return false;
    {
        const std::lock_guard lock(input_mutex_);
        const bool move = input.kind == InputKind::move;
        if (move && !pending_input_.empty() && pending_input_.back().kind == InputKind::move) {
            if (const auto& trace = events_->Trace()) trace->Pointer("input_coalesced", pending_input_.back());
            pending_input_.back() = input;
        } else if (pending_input_.size() < 64) {
            pending_input_.push_back(input);
        } else {
            if (!move) events_->Push({StreamEventType::error, {}, "键鼠发送队列已满，已停止会话以释放输入"});
            return move;
        }
        if (input_posted_) return true;
        input_posted_ = true;
    }
    signaling_->PostTask(webrtc::SafeTask(input_tasks_, [this] { DrainInput(); }));
    return true;
}

void RtcCall::DrainInput() {
    std::vector<InputPacket> batch;
    {
        const std::lock_guard lock(input_mutex_);
        batch.swap(pending_input_);
        input_posted_ = false;
    }
    for (const auto& input : batch) {
        if (input.kind == InputKind::move) {
            if (motion_) motion_->Send(input);
        } else if (!controls_ || !controls_->Send(input)) {
            events_->Push({StreamEventType::error, {}, "键鼠控制通道发送失败，已停止会话以释放输入"});
            return;
        }
    }
}

bool RtcCall::SendCursor(const std::string& packet) {
    return signaling_->BlockingCall([this, &packet] { return host_ && controls_ && controls_->SendCursor(packet); });
}

bool RtcCall::HasFrame() const {
    const std::lock_guard lock(mutex_);
    return latest_.has_value();
}

std::optional<DecodedFrame> RtcCall::TakeFrame() {
    const std::lock_guard lock(mutex_);
    return std::exchange(latest_, std::nullopt);
}

std::uint64_t RtcCall::ReplacedFrames() const {
    const std::lock_guard lock(mutex_);
    return replaced_frames_;
}

void RtcCall::OnIceGatheringChange(Peer::IceGatheringState state) {
    if (state == Peer::kIceGatheringComplete) {
        events_->Push({StreamEventType::gathered, {}, {}});
    }
}

void RtcCall::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    if (trickle_ && candidate) {
        events_->Push({StreamEventType::ice_candidate, {}, candidate->sdp_mid() + '\n'
            + std::to_string(candidate->sdp_mline_index()) + '\n' + candidate->ToString()});
    }
}

void RtcCall::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    if (host_) {
        return;
    }
    const auto label = channel->label();
    if (label == control_label && !controls_ && channel->ordered() && !channel->maxRetransmitsOpt()
            && !channel->maxPacketLifeTime()) {
        controls_ = std::make_unique<InputChannel>(std::move(channel), events_, false, false);
    } else if (label == motion_label && !motion_ && !channel->ordered() && channel->maxRetransmitsOpt() == 0) {
        motion_ = std::make_unique<InputChannel>(std::move(channel), events_, false, true);
    }
    events_->Push({StreamEventType::input_ready, {}, {}});
}

void RtcCall::OnConnectionChange(Peer::PeerConnectionState state) {
    if (state == Peer::PeerConnectionState::kConnected) {
        events_->Push({StreamEventType::connected, {}, {}});
    } else if (state == Peer::PeerConnectionState::kFailed || state == Peer::PeerConnectionState::kDisconnected) {
        events_->Push({StreamEventType::error, {}, "WebRTC 连接已断开"});
    }
}

void RtcCall::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    const auto receiver = transceiver->receiver();
    auto track = receiver->track();
    if (!host_ && !remote_track_ && track->kind() == "video") {
        receiver->SetJitterBufferMinimumDelay(0.0);
        remote_track_ = static_cast<webrtc::VideoTrackInterface*>(track.get());
        remote_track_->AddOrUpdateSink(this, {});
    }
}

void RtcCall::OnFrame(const webrtc::VideoFrame& frame) {
    const auto now = QpcNow();
    if (const auto& trace = events_->Trace()) trace->Record("frame_decoded", frame.rtp_timestamp(), 0, 0, 0, now);
    {
        const std::lock_guard lock(mutex_);
        if (latest_) ++replaced_frames_;
        latest_ = DecodedFrame{frame, now};
    }
    events_->Notify();
}

} // namespace p2premote
