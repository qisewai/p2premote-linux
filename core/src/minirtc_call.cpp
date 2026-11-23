#include "minirtc_call.h"

#include "cursor_shape.h"
#include "dxgi_nv12_frame.h"
#include "minirtc_desktop_video.h"
#include "plugin_host.h"
#include "qpc.h"

#include <p2premote/plugin_protocol.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace p2premote {
namespace {

using namespace winrt::Windows::Data::Json;
constexpr char video_name[] = "desktop";
constexpr char controls_name[] = "control";
constexpr char motion_name[] = "pointer";
constexpr char cursor_name[] = "cursor";
constexpr char plugin_name[] = "plugin";

bool SupportsEncryptedPluginChannel(std::wstring_view sdp) noexcept {
    bool data_srtp{}, fingerprint{}, plugin_track{};
    while (!sdp.empty()) {
        const auto end = sdp.find(L'\n');
        auto line = sdp.substr(0, end);
        if (line.ends_with(L'\r')) line.remove_suffix(1);
        data_srtp |= line == L"a=x-minirtc-audio-data-srtp:1";
        fingerprint |= line.starts_with(L"a=fingerprint:sha-256 ");
        plugin_track |= line.starts_with(L"a=ssrc:") && line.ends_with(L" name:plugin");
        if (end == std::wstring_view::npos) break;
        sdp.remove_prefix(end + 1);
    }
    // MiniRTC validates the SDP and rejects data packets if SRTP is unavailable.
    return data_srtp && fingerprint && plugin_track;
}

std::optional<DecodedFrame> RetainNv12(const MiniRtcVideoFrame& frame, std::int64_t queued_qpc) {
    if (const auto* native = frame.native_frame; native && native->struct_size >= sizeof(*native)
            && native->width == frame.width
            && native->height == frame.height && native->owner && native->retain && native->release) {
        const auto retain = [&] {
            native->retain(native->owner);
            return std::shared_ptr<void>(native->owner, [release = native->release](void* ptr) noexcept {
                release(ptr);
            });
        };
        const auto width = static_cast<int>(frame.width);
        const auto height = static_cast<int>(frame.height);
        if (native->type == MiniRtcNativeVideoFrameD3d11Nv12 && native->payload.d3d11_nv12.texture && native->copy_to_nv12) {
            const auto& surface = native->payload.d3d11_nv12;
            return DecodedFrame{width, height, 0, 0, nullptr, nullptr, retain(), queued_qpc,
                static_cast<ID3D11Texture2D*>(surface.texture), surface.subresource, native->copy_to_nv12};
        }
        if (native->type == MiniRtcNativeVideoFrameCpuNv12) {
            const auto& planes = native->payload.cpu_nv12;
            constexpr auto max_stride = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
            if (planes.y_plane && planes.uv_plane && planes.y_stride >= frame.width && planes.y_stride <= max_stride
                    && planes.uv_stride >= frame.width && planes.uv_stride <= max_stride) {
                return DecodedFrame{width, height, static_cast<int>(planes.y_stride), static_cast<int>(planes.uv_stride),
                    planes.y_plane, planes.uv_plane, retain(), queued_qpc};
            }
        }
    }

    const auto luma = static_cast<std::size_t>(frame.width) * frame.height;
    const auto total = luma + luma / 2;
    if (!frame.data || frame.size < total) return std::nullopt;
    auto storage = std::make_shared<std::vector<std::uint8_t>>(total);
    std::memcpy(storage->data(), frame.data, total);
    const auto* y_plane = storage->data();
    const auto* uv_plane = y_plane + luma;
    const auto width = static_cast<int>(frame.width);
    return DecodedFrame{
        width,
        static_cast<int>(frame.height),
        width,
        width,
        y_plane,
        uv_plane,
        std::move(storage),
        queued_qpc
    };
}

const native_minirtc::Api* NativeApi() {
    static const auto* api = []() -> const native_minirtc::Api* {
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
        if (!length || length >= std::size(executable)) return nullptr;
        const auto dll = std::filesystem::path(executable).parent_path() / L"p2premote_minirtc.dll";
        const auto module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return nullptr;
        const auto exported = GetProcAddress(module, "P2PRemoteMiniRtcApi");
        const auto get_api = reinterpret_cast<native_minirtc::GetApi>(exported);
        if (get_api) {
            if (const auto* result = get_api(native_minirtc::abi_version)) return result;
        }
        FreeLibrary(module);
        return nullptr;
    }();
    return api;
}

JsonObject IceConfiguration(const std::vector<IceServerOptions>& options) {
    using namespace std::chrono;
    const auto expires = duration_cast<seconds>(system_clock::now().time_since_epoch()).count() + 600;
    const auto expiry = JsonValue::CreateNumberValue(static_cast<double>(expires));
    const JsonArray servers;
    for (const auto& option : options) {
        const JsonArray urls;
        for (const auto& url : option.urls) urls.Append(JsonValue::CreateStringValue(winrt::to_hstring(url)));
        const JsonObject entry;
        entry.SetNamedValue(L"urls", urls);
        if (!option.username.empty()) {
            entry.SetNamedValue(L"username", JsonValue::CreateStringValue(winrt::to_hstring(option.username)));
            entry.SetNamedValue(L"credential", JsonValue::CreateStringValue(winrt::to_hstring(option.credential)));
            entry.SetNamedValue(L"expires_at", expiry);
        }
        servers.Append(entry);
    }
    const JsonObject config;
    config.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
    config.SetNamedValue(L"id", JsonValue::CreateStringValue(winrt::to_hstring(expires)));
    config.SetNamedValue(L"expires_at", expiry);
    config.SetNamedValue(L"servers", servers);
    return config;
}

TurnMode TurnPolicy(const ConnectionOptions& options) {
    bool udp{}, tcp{};
    for (const auto& server : options.ice_servers) {
        for (const auto& url : server.urls) {
            if (url.starts_with("turn:") || url.starts_with("turns:")) {
                if (url.ends_with("transport=tcp") || url.starts_with("turns:")) tcp = true;
                else udp = true;
            }
        }
    }
    if (options.relay_only) return udp ? TurnForceUdp : TurnForceTcp;
    return udp || tcp ? TurnAutoUdpTcp : TurnDisabled;
}

} // namespace

bool MiniRtcCall::Available(const ConnectionOptions& options) {
    return NativeApi() && std::all_of(options.ice_servers.begin(), options.ice_servers.end(), [](const auto& server) {
        return std::none_of(server.urls.begin(), server.urls.end(), [](const auto& url) {
            return url.starts_with("turns:");
        });
    });
}

MiniRtcCall::MiniRtcCall(bool host, std::shared_ptr<StreamEvents> events, const ConnectionOptions& options,
    bool allow_input, unsigned frame_rate, ID3D11Device* device)
    : host_(host), allow_input_(allow_input), events_(std::move(events)), api_(NativeApi()), ice_(options.ice_servers) {
    device_.copy_from(device);
    if (!api_) throw std::runtime_error("MiniRTC 模块不可用，请确认 p2premote_minirtc.dll 位于程序目录");
    if (!Available(options)) throw std::runtime_error("当前 MiniRTC 模块不支持 turns: 协议");
    Params params{};
    const auto dir = options.ice_trace_dir.empty() ?
        std::filesystem::temp_directory_path() / L"P2PRemote" / L"MiniRTC" : options.ice_trace_dir;
    std::filesystem::create_directories(dir);
    const auto log = dir.string();
    if (log.size() >= sizeof(params.log_path)) throw std::runtime_error("MiniRTC 日志目录过长");
    std::copy(log.begin(), log.end(), params.log_path);
    params.user_id = host_ ? "host" : "viewer";
    params.user_data = this;
    params.turn_mode = TurnPolicy(options);
    params.enable_srtp = true;
    ConfigureMiniRtcDesktopVideo(params, frame_rate);
    params.hardware_acceleration = device_ != nullptr;
    params.windows_d3d11_device = device_.get();
    params.native_video_output = !host_;
    params.on_signal_status = [](SignalStatus, const char*, size_t, void*) {};
    params.on_connection_status = Status;
    params.on_receive_video_frame = Video;
    params.on_receive_data_buffer = Data;
    params.on_net_status_report = Network;
    if (allow_input_) {
        try {
            plugins_ = std::make_unique<PluginHost>(host_, [this](std::string_view frame) {
                return connected_.load() && api_->send_reliable(peer_, frame.data(), frame.size(), plugin_name) == 0;
            }, std::filesystem::path{}, [events = events_] { events->Notify(); });
        } catch (...) {
            OutputDebugStringW(L"P2PRemote: could not start plugin worker\n");
        }
    }
    const auto release = [api = api_](PeerPtr* peer) { api->destroy(&peer); };
    std::unique_ptr<PeerPtr, decltype(release)> peer(api_->create(&params), release);
    if (peer && plugins_ && api_->add_data(peer.get(), plugin_name, true) != 0) {
        plugins_.reset();
        OutputDebugStringW(L"P2PRemote: could not register plugin channel\n");
    }
    if (!peer || api_->add_video(peer.get(), video_name) != 0 || api_->add_data(peer.get(), controls_name, true) != 0 ||
        api_->add_data(peer.get(), motion_name, false) != 0 || api_->add_data(peer.get(), cursor_name, true) != 0 ||
        api_->init(peer.get(), SignalOut, this) != 0 || (!host_ && api_->join(peer.get(), "host") != 0)) {
        throw std::runtime_error("MiniRTC 初始化失败");
    }
    peer_ = peer.release();
}

MiniRtcCall::~MiniRtcCall() {
    if (plugins_) plugins_->Stop();
    if (peer_) api_->destroy(&peer_);
}

void MiniRtcCall::SetPluginsConnected(bool connected) noexcept {
    if (!plugins_) return;
    const auto ready = connected && plugin_transport_.load() && connected_.load();
    workspace_running_ = ready;
    if (!ready) {
        workspace_peer_ = false;
        workspace_reply_pending_ = false;
    }
    plugins_->SetConnected(ready);
    if (ready) ProbeWorkspace();
}

void MiniRtcCall::SendWorkspaceMessage(std::string_view kind) noexcept {
    try {
        if (const auto frame = EncodePluginPacket({workspace_protocol_id, kind, "1"})) {
            api_->send_reliable(peer_, frame->data(), frame->size(), plugin_name);
        }
    } catch (...) {}
}

void MiniRtcCall::ProbeWorkspace() noexcept {
    if (!workspace_running_.load()) return;
    if (host_) {
        if (workspace_reply_pending_.exchange(false)) SendWorkspaceMessage("ready");
        return;
    }
    if (WorkspaceInputReady() || workspace_attempts_ == 12) return;
    const auto now = GetTickCount64();
    if (now < workspace_retry_) return;
    workspace_retry_ = now + 1'000;
    ++workspace_attempts_;
    SendWorkspaceMessage("hello");
}

void MiniRtcCall::SignalOut(const char* data, size_t size, void* context) noexcept {
    auto& self = *static_cast<MiniRtcCall*>(context);
    try {
        if (size <= 64 * 1024) self.events_->Push({StreamEventType::minirtc_signal, {}, std::string(data, size)});
    } catch (...) {}
}

bool MiniRtcCall::IsSessionSignal(const std::string& payload) {
    const auto json = JsonObject::Parse(winrt::to_hstring(payload));
    const auto type = json.GetNamedString(L"type");
    return type == L"join_transmission" || type == L"offer" || type == L"answer" ||
        type == L"new_candidate" || type == L"new_candidate_mid";
}

void MiniRtcCall::ReceiveSignal(const std::string& payload) {
    auto message = JsonObject::Parse(winrt::to_hstring(payload));
    const auto kind = message.GetNamedString(L"type");
    if (plugins_ && (kind == L"offer" || kind == L"answer")) {
        const bool supported = SupportsEncryptedPluginChannel(message.GetNamedString(L"sdp", L""));
        plugin_transport_.store(supported);
        if (!supported) SetPluginsConnected(false);
    }
    if (kind == L"offer" || kind == L"user_join_transmission") {
        message.SetNamedValue(L"ice", IceConfiguration(ice_));
    }
    const auto wire = winrt::to_string(message.Stringify());
    if (api_->receive_signal(peer_, wire.data(), wire.size()) != 0) {
        throw std::runtime_error("收到无效的 MiniRTC 信令");
    }
}

void MiniRtcCall::Status(ConnectionStatus status, const char*, size_t, void* context) {
    auto& self = *static_cast<MiniRtcCall*>(context);
    if (status == Connected) {
        if (!self.connected_.exchange(true)) {
            self.events_->Push({StreamEventType::connected, {}, {}});
            self.events_->Push({StreamEventType::input_ready, {}, {}});
        }
    } else if (status == Closed || status == Failed || status == Disconnected ||
               status == IncorrectPassword || status == NoSuchTransmissionId || status == RemoteUnavailable) {
        self.connected_ = false;
        self.SetPluginsConnected(false);
        self.events_->Push({StreamEventType::input_ready, {}, {}});
        self.events_->Push({StreamEventType::media_lost, {}, "MiniRTC 连接已断开"});
    }
}

void MiniRtcCall::Network(const char*, size_t, TraversalMode route, const MiniRtcNetTrafficStats* stats,
    const char*, size_t, void* context) {
    if (!stats || route == UnknownMode || stats->selected_path == MiniRtcNetTrafficStats::Path::Unknown) return;
    auto& self = *static_cast<MiniRtcCall*>(context);
    const std::lock_guard lock(self.mutex_);
    using Path = MiniRtcNetTrafficStats::Path;
    const auto path = stats->selected_path;
    if (path == Path::Relay) {
        self.counters_.ice_path = IcePath::relay;
        self.counters_.ice_protocol.clear();
    } else {
        self.counters_.ice_path = path == Path::HostUdp || path == Path::HostTcp ? IcePath::host : IcePath::nat;
        self.counters_.ice_protocol = path == Path::HostUdp || path == Path::DirectUdp ? "udp" : "tcp";
    }
    if (self.counters_.video_codec.empty()) self.counters_.video_codec = "H264 (MiniRTC)";
    if (stats->rtt_ms >= 0) self.counters_.ice_rtt_ms = stats->rtt_ms;
    else self.counters_.ice_rtt_ms.reset();
    self.counters_.srtp_active = stats->srtp_active;
    self.counters_.upload_bps = stats->total_outbound_stats.bitrate;
    self.counters_.download_bps = stats->total_inbound_stats.bitrate;
    if (const auto& timing = stats->video_send_queue; timing.packets) {
        self.counters_.sent_packets += timing.packets;
        self.counters_.packet_send_queue_seconds += static_cast<double>(timing.total_wait_us) / 1'000'000;
        if (const auto& trace = self.events_->Trace()) {
            trace->Record("minirtc_send_queue_interval", timing.packets, timing.total_wait_us, 0, timing.p95_bucket_ms);
        }
    }
    self.counters_.video_packets_received = stats->video_inbound_stats.rtp_packet_count;
    if (self.counters_.video_packets_received) {
        self.counters_.video_loss_fraction = stats->video_inbound_stats.loss_rate;
    }
}

void MiniRtcCall::Data(const char* data, size_t size, const char*, size_t, const char* source, size_t source_size,
    void* context) {
    auto& self = *static_cast<MiniRtcCall*>(context);
    if (!data || !source || !self.connected_.load()) return;
    const std::string_view label(source, source_size);
    if (label == controls_name || (self.host_ && label == motion_name)) {
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(data), size);
        if (label == controls_name && self.host_ && self.allow_input_ && self.WorkspaceInputReady() && self.workspace_running_.load() &&
                !bytes.empty() && bytes.front() != 1) {
            try {
                if (auto command = DecodeInputCommand(bytes)) {
                    if (const auto& trace = self.events_->Trace()) {
                        trace->Record("input_command_received", command->sent_ms,
                            static_cast<std::uint64_t>(command->kind), command->units.size());
                    }
                    self.events_->PushInputCommand(std::move(*command));
                }
            } catch (...) {}
            return;
        }
        const auto input = DecodeInput(bytes);
        if (!input) return;
        if (IsVideoSettingsMessage(input->kind)) {
            if (label == controls_name && input->kind == (self.host_ ? InputKind::set_video_settings : InputKind::video_settings_reply)) {
                self.events_->Push({StreamEventType::video_settings, {}, {}, {}, *input});
            }
        } else if (self.host_ && self.allow_input_ && ((input->kind == InputKind::move) == (label == motion_name))) {
            if (const auto& trace = self.events_->Trace()) trace->Pointer("input_received", *input);
            self.events_->PushInput(*input);
        }
    } else if (!self.host_ && self.allow_input_ && label == cursor_name && size <= cursor_max_packet) {
        self.events_->Push({StreamEventType::cursor, {}, std::string(data, size)});
    } else if (self.plugins_ && label == plugin_name && self.workspace_running_.load()) {
        const std::string_view frame(data, size);
        const auto packet = DecodePluginPacket(frame);
        if (!packet) return;
        if (packet->id != workspace_protocol_id) {
            self.plugins_->Receive(frame);
        } else if (packet->payload == "1") {
            if (self.host_ && packet->type == "hello") {
                self.workspace_peer_ = true;
                self.workspace_reply_pending_ = true;
                self.events_->Notify();
            } else if (!self.host_ && packet->type == "ready") {
                self.workspace_peer_ = true;
            }
        }
    }
}

void MiniRtcCall::Video(const MiniRtcVideoFrame* frame, const char*, size_t, const char* source, size_t source_size,
    void* context) {
    auto& self = *static_cast<MiniRtcCall*>(context);
    if (self.host_ || !source || std::string_view(source, source_size) != video_name || !frame ||
        !frame->width || !frame->height || ((frame->width | frame->height) & 1u) ||
        frame->width > 2560 || frame->height > 1440) return;
    const auto queued = QpcNow();
    auto decoded = RetainNv12(*frame, queued);
    if (!decoded) return;
    if (const auto& trace = self.events_->Trace()) {
        // MiniRTC's Windows clock and the application both use QPC.
        constexpr auto max_timestamp_us = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 10);
        const auto id = static_cast<std::uint64_t>(queued);
        if (frame->received_timestamp && frame->received_timestamp <= frame->decoded_timestamp
                && frame->decoded_timestamp <= max_timestamp_us) {
            const auto decoder_complete = ToQpc(static_cast<std::int64_t>(frame->decoded_timestamp) * 10);
            if (decoder_complete <= queued) {
                const auto assembled = ToQpc(static_cast<std::int64_t>(frame->received_timestamp) * 10);
                trace->Record("minirtc_frame_assembled", id, 0, 0, 0, assembled);
                trace->Record("minirtc_decoder_complete", id, 0, 0, 0, decoder_complete);
            }
        }
        trace->Record("minirtc_frame_queued", id, 0, frame->size, 0, queued);
    }
    {
        const std::lock_guard lock(self.mutex_);
        self.counters_.video_codec = decoded->texture ? "H264 (DXVA)" : "H264 (MiniRTC)";
        self.replaced_ += self.latest_.has_value();
        self.latest_.emplace(std::move(*decoded));
        ++self.counters_.received;
        ++self.counters_.decoded;
    }
    self.events_->Notify();
}

void MiniRtcCall::Submit(const Nv12Frame& frame) {
    if (!host_ || !connected_) return;
    const int width = frame.width, height = frame.height;
    if (width <= 0 || height <= 0 || ((width | height) & 1)) return;
    const auto luma = static_cast<std::size_t>(width) * height;
    MiniRtcVideoFrame input{};
    input.size = luma * 3 / 2;
    input.width = width;
    input.height = height;
    if (frame.texture) {
        using native_minirtc::DxgiNv12Frame;
        const auto native = std::unique_ptr<DxgiNv12Frame, decltype(&DxgiNv12Frame::Release)>(
            new DxgiNv12Frame(frame.texture.get(), 0, width, height), &DxgiNv12Frame::Release);
        input.native_frame = &native->descriptor;
        api_->send_video(peer_, &input, video_name);
        return;
    }
    const char* payload = nullptr;
    if (frame.IsTightlyPacked()) {
        payload = reinterpret_cast<const char*>(frame.DataY());
    } else {
        nv12_.resize(luma * 3 / 2);
        auto* dst_y = nv12_.data();
        auto* dst_uv = dst_y + luma;
        for (int row = 0; row < height; ++row) {
            std::memcpy(dst_y + static_cast<std::size_t>(row) * width,
                frame.DataY() + static_cast<std::size_t>(row) * frame.stride_y, width);
        }
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(dst_uv + static_cast<std::size_t>(row) * width,
                frame.DataUV() + static_cast<std::size_t>(row) * frame.stride_uv, width);
        }
        payload = reinterpret_cast<const char*>(nv12_.data());
    }
    input.data = payload;
    api_->send_video(peer_, &input, video_name);
}

bool MiniRtcCall::SetVideoBitrate(int bitrate) {
    return host_ && connected_ && api_->set_bitrate(peer_, "viewer", 6, bitrate) == 0;
}

void MiniRtcCall::RequestStats() {
    ProbeWorkspace();
    if (!connected_) return;
    const std::lock_guard lock(mutex_);
    events_->Push({StreamEventType::stats, {}, {}, counters_});
}

bool MiniRtcCall::SendInput(const InputPacket& packet) {
    if (host_ || !InputReady()) return false;
    const auto bytes = EncodeInput(packet);
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    if (packet.kind == InputKind::move) return api_->send_data(peer_, data, bytes.size(), motion_name) == 0;
    return api_->send_reliable(peer_, data, bytes.size(), controls_name) == 0;
}

bool MiniRtcCall::SendInputCommand(const InputCommand& command) {
    if (host_ || !InputReady() || !WorkspaceInputReady() || !workspace_running_.load()) return false;
    const auto bytes = EncodeInputCommand(command);
    return !bytes.empty() && api_->send_reliable(peer_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), controls_name) == 0;
}

bool MiniRtcCall::SendVideoSettings(const InputPacket& settings) {
    if (!connected_.load() || settings.kind != (host_ ? InputKind::video_settings_reply : InputKind::set_video_settings)) return false;
    const auto bytes = EncodeInput(settings);
    return api_->send_reliable(peer_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), controls_name) == 0;
}

bool MiniRtcCall::SendCursor(const std::string& packet) {
    return host_ && InputReady() && api_->send_reliable(peer_, packet.data(), packet.size(), cursor_name) == 0;
}

bool MiniRtcCall::HasFrame() const {
    const std::lock_guard lock(mutex_);
    return latest_.has_value();
}

std::optional<DecodedFrame> MiniRtcCall::TakeFrame() {
    const std::lock_guard lock(mutex_);
    return std::exchange(latest_, std::nullopt);
}

std::uint64_t MiniRtcCall::ReplacedFrames() const {
    const std::lock_guard lock(mutex_);
    return replaced_;
}

} // namespace p2premote
