#include "host_session.h"
#include "service_support.h"
#include "video_frame_cadence.h"

#include "minirtc_desktop_video.h"
#include <p2premote/plugin_protocol.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

namespace p2premote::linux_host {
namespace {

constexpr char kVideoStream[] = "desktop";
constexpr char kAudioStream[] = "desktop_audio";
constexpr char kControlStream[] = "control-v3";
constexpr char kPointerStream[] = "pointer";
constexpr char kCursorStream[] = "cursor";

std::uint64_t MonotonicMs() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::int64_t UnixSeconds() noexcept {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

int QualityMaxHeight(StreamQuality quality) noexcept {
    switch (quality) {
    case StreamQuality::p720: return 720;
    case StreamQuality::p1440: return 1440;
    case StreamQuality::p1080:
    default: return 1080;
    }
}

std::string InjectEmptyLanIce(const std::string& json) {
    const auto kind = ExtractJsonString(json, "type");
    if (kind != "offer" && kind != "user_join_transmission") return json;
    const auto close = json.rfind('}');
    if (close == std::string::npos) return json;
    const auto expires = std::to_string(UnixSeconds() + 600);
    const std::string ice = ",\"ice\":{\"version\":1,\"id\":\"" + expires + "\",\"expires_at\":" + expires + ",\"servers\":[]}";
    return json.substr(0, close) + ice + "}";
}

} // namespace

LinuxHostSession::LinuxHostSession(HostConfig config, std::unique_ptr<DesktopSession> desktop,
    std::optional<CloudCallbacks> cloud)
    : config_(std::move(config)),
      cloud_(std::move(cloud)),
      api_(P2PRemoteMiniRtcApi(p2premote::native_minirtc::abi_version)),
      desktop_(std::move(desktop)),
      max_height_(config_.max_height) {
    if (!api_) {
        std::cerr << "[host] MiniRTC ABI version mismatch\n";
        return;
    }
    if (!desktop_ || !desktop_->Start()) {
        std::cerr << "[host] No usable desktop backend\n";
        return;
    }
    desktop_access_.Attach(desktop_->Input());
    if (!desktop_access_.CanInject()) {
        std::cerr << "[host] Input unavailable, desktop viewing remains available\n";
    }

    if (!cloud_) {
        LanSignalServer::Callbacks cb;
        cb.on_client_authed = [this](const std::string& ip) { OnClientAuthed(ip); };
        cb.on_minirtc = [this](const std::string& payload) { OnMiniRtcSignal(payload); };
        cb.on_ended = [this](const std::string& reason) { OnClientEnded(reason); };
        server_ = std::make_unique<LanSignalServer>(
            config_.display_name, config_.port, config_.access_code, std::move(cb), config_.discovery_port,
            SupportsControl());
        if (!server_->Start()) return;
    }
    capture_thread_ = std::jthread([this](std::stop_token stop) { CaptureLoop(stop); });
    audio_thread_ = std::jthread([this](std::stop_token stop) {
        auto audio = CreateDesktopAudio();
        audio->Capture(stop, [this] { return AudioState(); },
            [this](DesktopAudioState state, const char* data, std::size_t size) { SendDesktopAudio(state, data, size); });
    });
}

LinuxHostSession::~LinuxHostSession() {
    if (audio_thread_.joinable()) {
        audio_thread_.request_stop();
        audio_thread_.join();
    }
    if (capture_thread_.joinable()) {
        capture_thread_.request_stop();
        capture_thread_.join();
    }
    if (server_) server_->Stop();
    StopPeer();
    if (desktop_) desktop_->Stop();
}

void LinuxHostSession::OnClientAuthed(const std::string& peer_ip) {
    std::cout << "[host] viewer authenticated from " << peer_ip << "\n" << std::flush;
    if (StartPeer(TurnDisabled)) PublishState("negotiating");
    else server_->EndActiveClient("本机正在被远控或无法建立连接");
}

void LinuxHostSession::OnMiniRtcSignal(const std::string& payload) {
    const std::lock_guard lock(peer_mutex_);
    if (!peer_) return;
    const auto wire = InjectEmptyLanIce(payload);
    if (api_->receive_signal(peer_, wire.data(), wire.size()) != 0) {
        server_->EndActiveClient("MiniRTC rejected signaling payload");
    }
}

void LinuxHostSession::OnClientEnded(const std::string& reason) {
    std::cout << "[host] session ended: " << reason << "\n" << std::flush;
    StopPeer();
    PublishState("waiting");
}

void LinuxHostSession::PublishState(std::string_view state) {
    if (cloud_) cloud_->status(state);
    else NotifyHostState(state, config_.port);
}

bool LinuxHostSession::StartCloudPeer(TurnMode turn) {
    return cloud_ && StartPeer(turn);
}

bool LinuxHostSession::ReceiveCloudSignal(const std::string& payload) {
    const std::lock_guard lock(peer_mutex_);
    return cloud_ && peer_ && api_->receive_signal(peer_, payload.data(), payload.size()) == 0;
}

void LinuxHostSession::StopCloudPeer() {
    if (cloud_) StopPeer();
}

bool LinuxHostSession::StartPeer(TurnMode turn) {
    StopPeer();
    const std::lock_guard lock(peer_mutex_);
    if (!api_) return false;
    const auto lock_path = config_.log_dir + "/console.lock";
    console_lock_ = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (console_lock_ < 0) return false;
    const auto unlock = [&] { close(std::exchange(console_lock_, -1)); };
    struct stat stat{};
    if (fstat(console_lock_, &stat) != 0 || !S_ISREG(stat.st_mode) || stat.st_uid != geteuid() ||
        (stat.st_mode & 0077) != 0 || flock(console_lock_, LOCK_EX | LOCK_NB) != 0) {
        unlock();
        return false;
    }

    Params params{};
    const auto copy_len = std::min(config_.log_dir.size(), sizeof(params.log_path) - 1);
    std::memcpy(params.log_path, config_.log_dir.data(), copy_len);
    params.user_id = "host";
    params.user_data = this;
    params.turn_mode = turn;
    params.enable_srtp = true;
    ConfigureMiniRtcDesktopVideo(params, config_.fps);
    params.hardware_acceleration = false;
    params.native_video_output = false;
    params.on_signal_status = [](SignalStatus, const char*, size_t, void*) {};
    params.on_signal_message = [](const char*, size_t, void*) {};
    params.on_connection_status = ConnectionStatusCb;
    params.on_net_status_report = [](const char*, size_t, TraversalMode, const MiniRtcNetTrafficStats*,
                                     const char*, size_t, void*) {};
    params.on_receive_video_frame = [](const MiniRtcVideoFrame*, const char*, size_t, const char*, size_t, void*) {};
    params.on_receive_data_buffer = DataCb;

    PeerPtr* peer = api_->create(&params);
    if (!peer) {
        unlock();
        return false;
    }
    if (api_->add_video(peer, kVideoStream) != 0 ||
        AddAudioStream(peer, kAudioStream) != 0 ||
        api_->add_data(peer, kControlStream, true) != 0 ||
        api_->add_data(peer, kPointerStream, false) != 0 ||
        api_->add_data(peer, kCursorStream, true) != 0 ||
        api_->add_data(peer, "plugin", true) != 0 ||
        api_->init(peer, SignalOut, this) != 0) {
        api_->destroy(&peer);
        unlock();
        return false;
    }
    peer_ = peer;
    {
        const std::lock_guard guard(clipboard_mutex_);
        clipboard_ = CreateClipboardBridge([this] { return ClipboardTarget(); }, [this](const DesktopTarget& target, std::string_view frame) {
            const std::lock_guard lock(peer_mutex_);
            return peer_ && ClipboardTarget() == target && api_->send_reliable(peer_, frame.data(), frame.size(), "plugin") == 0;
        });
    }
    connected_.store(false, std::memory_order_release);
    cursor_ready_.store(false, std::memory_order_release);
    force_cursor_.store(true, std::memory_order_release);
    {
        const std::lock_guard ilock(input_mutex_);
        desktop_access_.Clear();
        peer_active_.store(true, std::memory_order_release);
    }
    return true;
}

void LinuxHostSession::StopPeer() {
    {
        const std::lock_guard ilock(input_mutex_);
        peer_active_.store(false, std::memory_order_release);
        connected_.store(false, std::memory_order_release);
        cursor_ready_.store(false, std::memory_order_release);
        ++peer_generation_;
        speaker_enabled_ = false;
        ++audio_revision_;
        desktop_access_.Clear();
    }
    std::unique_ptr<ClipboardBridge> clipboard;
    {
        const std::lock_guard lock(clipboard_mutex_);
        clipboard = std::move(clipboard_);
    }
    clipboard.reset();
    PeerPtr* old = nullptr;
    {
        const std::lock_guard lock(peer_mutex_);
        old = std::exchange(peer_, nullptr);
    }
    if (old && api_) {
        api_->destroy(&old);
    }
    if (console_lock_ >= 0) close(std::exchange(console_lock_, -1));
}

void LinuxHostSession::SignalOut(const char* data, std::size_t size, void* ctx) noexcept {
    auto* self = static_cast<LinuxHostSession*>(ctx);
    if (!self || !data || !size || size > 64 * 1024) return;
    try {
        if (self->cloud_) self->cloud_->send_signal(std::string(data, size), self->PeerGeneration());
        else if (self->server_) self->server_->Send("minirtc", std::string(data, size));
    } catch (...) {}
}

void LinuxHostSession::ConnectionStatusCb(ConnectionStatus status, const char*, std::size_t, void* ctx) {
    auto* self = static_cast<LinuxHostSession*>(ctx);
    if (!self) return;
    const std::lock_guard ilock(self->input_mutex_);
    if (!self->peer_active_.load(std::memory_order_acquire)) return;
    if (status == Connected) {
        if (!self->connected_.exchange(true, std::memory_order_acq_rel)) {
            if (self->server_) self->server_->SetMediaConnected(true);
            self->PublishState("display");
            self->force_cursor_.store(true, std::memory_order_release);
        }
    } else if (status == Closed || status == Failed || status == Disconnected ||
               status == IncorrectPassword || status == NoSuchTransmissionId || status == RemoteUnavailable) {
        self->connected_.store(false, std::memory_order_release);
        self->speaker_enabled_ = false;
        ++self->audio_revision_;
        self->desktop_access_.Clear();
        if (self->cloud_) self->cloud_->disconnected(self->PeerGeneration());
        else self->server_->EndActiveClient("MiniRTC disconnected");
    }
}

void LinuxHostSession::DataCb(const char* data, std::size_t size, const char* remote_peer_id,
    std::size_t remote_peer_id_size, const char* source, std::size_t source_size, void* ctx) {
    auto* self = static_cast<LinuxHostSession*>(ctx);
    if (!self || !data || !source || !self->connected_.load(std::memory_order_acquire)) return;
    self->HandleData(data, size, remote_peer_id, remote_peer_id_size, source, source_size);
}

void LinuxHostSession::HandleData(const char* data, std::size_t size, const char* remote_peer_id,
    std::size_t remote_peer_id_size, const char* source, std::size_t source_size) {
    const std::string_view label(source, source_size);
    if (label == "plugin") {
        const std::string_view frame(data, size);
        if (const auto packet = DecodePluginPacket(frame);
            packet && packet->id == "_workspace" && packet->type == "hello") {
            if (const auto reply = EncodePluginPacket({"_workspace", "ready", "1"})) {
                const std::lock_guard lock(peer_mutex_);
                if (peer_ && connected_.load(std::memory_order_acquire)) {
                    api_->send_reliable(peer_, reply->data(), reply->size(), "plugin");
                }
            }
            return;
        }
        const std::lock_guard lock(clipboard_mutex_);
        if (clipboard_) clipboard_->Receive(frame);
        return;
    }
    if (label != kControlStream && label != kPointerStream) return;

    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(data), size);
    const auto packet = DecodeInput(bytes);
    if (!packet) {
        if (label == kControlStream) {
            if (const auto command = DecodeInputCommand(bytes)) {
                const std::lock_guard ilock(input_mutex_);
                if (connected_.load(std::memory_order_acquire) && DesktopCurrentLocked()) {
                    desktop_access_.Apply(*command, MonotonicMs());
                }
            }
        }
        return;
    }

    const auto now = MonotonicMs();
    if (packet->kind == InputKind::set_video_settings && label == kControlStream) {
        const auto quality = static_cast<StreamQuality>(packet->code);
        const auto bitrate = VideoSettingsBitrate(*packet);
        max_height_.store(std::min(config_.max_height, QualityMaxHeight(quality)), std::memory_order_release);
        bool applied = true;
        if (bitrate >= 500'000 && bitrate <= 32'000'000) {
            const char* peer_id = (remote_peer_id && remote_peer_id_size > 0) ? remote_peer_id : "viewer";
            const std::size_t peer_id_len = (remote_peer_id && remote_peer_id_size > 0) ? remote_peer_id_size : 6u;
            const std::lock_guard lock(peer_mutex_);
            if (peer_) applied = api_->set_bitrate(peer_, peer_id, peer_id_len, static_cast<int>(bitrate)) == 0;
        }
        const auto reply = EncodeInput(VideoSettingsPacket(
            InputKind::video_settings_reply, quality, applied ? bitrate : 0, packet->sequence, now));
        const std::lock_guard lock(peer_mutex_);
        if (peer_) {
            api_->send_reliable(peer_, reinterpret_cast<const char*>(reply.data()), reply.size(), kControlStream);
        }
        return;
    }

    if (packet->kind == InputKind::cursor_ready && label == kControlStream) {
        cursor_ready_.store(true, std::memory_order_release);
        force_cursor_.store(true, std::memory_order_release);
        return;
    }

    if (packet->kind == InputKind::set_audio_settings) {
        if (label != kControlStream) return;
        const std::lock_guard lock(input_mutex_);
        const bool enabled = (packet->code & audio_flag_speaker) != 0;
        if (connected_.load(std::memory_order_acquire) && speaker_enabled_ != enabled) {
            speaker_enabled_ = enabled;
            ++audio_revision_;
        }
        return;
    }

    if (label == kPointerStream && packet->kind != InputKind::move) return;

    const std::lock_guard ilock(input_mutex_);
    if (connected_.load(std::memory_order_acquire) && DesktopCurrentLocked()) desktop_access_.Apply(*packet, now);
}

bool LinuxHostSession::DesktopCurrentLocked() const {
    return desktop_access_.Target().valid() && desktop_access_.Matches(desktop_->State(), desktop_->Target());
}

std::optional<DesktopTarget> LinuxHostSession::ClipboardTarget() {
    const std::lock_guard lock(input_mutex_);
    if (!connected_.load() || !SupportsControl() || !DesktopCurrentLocked() ||
        !desktop_->Capabilities().clipboard_control) return {};
    return desktop_access_.UserTarget();
}

std::optional<DesktopAudioState> LinuxHostSession::AudioState() {
    const std::lock_guard lock(input_mutex_);
    if (!connected_.load() || !speaker_enabled_ || !DesktopCurrentLocked()) return {};
    if (auto target = desktop_access_.UserTarget()) return DesktopAudioState{audio_revision_, std::move(*target)};
    return {};
}

void LinuxHostSession::SendDesktopAudio(DesktopAudioState state, const char* data, std::size_t size) {
    const std::lock_guard lock(peer_mutex_);
    if (!peer_ || AudioState() != state) return;
    const MiniRtcAudioFrame frame{data, size, 0};
    SendAudioFrame(peer_, &frame, kAudioStream);
}

void LinuxHostSession::CaptureLoop(std::stop_token stop) {
    const auto frame_interval = std::chrono::microseconds(1'000'000 / std::clamp(config_.fps, 5u, 60u));
    VideoFrameCadence cadence;
    auto last_cursor_poll = std::chrono::steady_clock::now();
    auto last_stats_log = std::chrono::steady_clock::now();
    std::uint64_t frames_sent = 0;
    std::uint64_t previous_generation = 0;
    bool logged_first_frame = false;
    std::string_view waiting_status = "display";

    while (!stop.stop_requested()) {
        const auto generation = peer_generation_.load(std::memory_order_acquire);
        if (previous_generation != generation) {
            previous_generation = generation;
            logged_first_frame = false;
            waiting_status = "display";
            frames_sent = 0;
            cadence.Reset();
        }
        if (!connected_.load(std::memory_order_acquire)) {
            cadence.Reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        const auto tick_start = std::chrono::steady_clock::now();

        const auto recheck_desktop = [&] {
            const auto state = desktop_->State();
            const auto status = WaitingDesktopStatus(state);
            const std::lock_guard ilock(input_mutex_);
            const bool display_lost = desktop_access_.Recheck(state, desktop_->Target(), MonotonicMs());
            if (display_lost) {
                cadence.Reset();
                force_cursor_.store(true, std::memory_order_release);
            }
            if (display_lost || (!desktop_access_.Target().valid() && status != waiting_status)) {
                waiting_status = status;
                PublishState(status);
            }
            return state != DesktopSessionState::waiting_permission && state != DesktopSessionState::revoked &&
                state != DesktopSessionState::error;
        };

        const bool can_capture = recheck_desktop();
        const auto frame = can_capture ? desktop_->AcquireLatestFrame() : std::nullopt;
        const bool desktop_allowed = can_capture && recheck_desktop();
        if (frame && desktop_allowed && ValidFrame(*frame)) {
            const int cap_h = max_height_.load(std::memory_order_acquire);
            int dst_w = frame->width;
            int dst_h = frame->height;
            if (cap_h > 0 && dst_h > cap_h) {
                dst_w = static_cast<int>(static_cast<std::int64_t>(dst_w) * cap_h / dst_h);
                dst_h = cap_h;
            }
            dst_w &= ~1;
            dst_h &= ~1;

            const auto submission = dst_w >= 64 && dst_h >= 64
                ? cadence.Next(*frame, dst_w, dst_h, tick_start) : FrameSubmission::skip;
            const bool converted = submission != FrameSubmission::convert || nv12_.Convert(*frame, dst_w, dst_h);
            if (!converted) cadence.Reset();
            if (converted && submission != FrameSubmission::skip) {
                MiniRtcVideoFrame vf{};
                vf.data = reinterpret_cast<const char*>(nv12_.Pixels().data());
                vf.size = nv12_.Pixels().size();
                vf.width = static_cast<std::uint32_t>(dst_w);
                vf.height = static_cast<std::uint32_t>(dst_h);

                const std::lock_guard lock(peer_mutex_);
                if (peer_ && connected_.load(std::memory_order_acquire) && generation == peer_generation_.load() &&
                    desktop_->IsFrameCurrent(*frame) &&
                    api_->send_video(peer_, &vf, kVideoStream) == 0) {
                    cadence.Submitted(tick_start);
                    desktop_->MarkFrameSent();
                    {
                        const std::lock_guard ilock(input_mutex_);
                        if (connected_.load(std::memory_order_acquire) &&
                            desktop_access_.Activate(desktop_->Target())) {
                            PublishState(SupportsControl() ? "streaming" : "viewing");
                        }
                    }
                    ++frames_sent;
                    if (!logged_first_frame) {
                        logged_first_frame = true;
                        last_stats_log = tick_start;
                        std::cout << "[host] first frame submitted: " << dst_w << "x" << dst_h
                                  << " (" << desktop_->Describe() << ")\n" << std::flush;
                    } else if (tick_start - last_stats_log >= std::chrono::seconds(10)) {
                        last_stats_log = tick_start;
                        std::cout << "[host] streaming: " << frames_sent << " frames sent, "
                                  << dst_w << "x" << dst_h << " (" << desktop_->Describe() << ")\n" << std::flush;
                    }
                }
            }
        }

        if (cursor_ready_.load(std::memory_order_acquire) &&
            (tick_start - last_cursor_poll >= std::chrono::milliseconds(33) ||
             force_cursor_.load(std::memory_order_acquire))) {
            last_cursor_poll = tick_start;
            const bool force = force_cursor_.exchange(false, std::memory_order_acq_rel);
            if (const auto cursor = desktop_->PollCursor(force)) {
                const std::lock_guard lock(peer_mutex_);
                if (peer_ && connected_.load(std::memory_order_acquire) && generation == peer_generation_.load()) {
                    api_->send_reliable(peer_, cursor->packet.data(), cursor->packet.size(), kCursorStream);
                }
            }
        }

        const auto elapsed = std::chrono::steady_clock::now() - tick_start;
        if (elapsed < frame_interval) {
            desktop_->WaitForActivity(std::chrono::ceil<std::chrono::microseconds>(frame_interval - elapsed), stop);
        }
    }
}

} // namespace p2premote::linux_host
