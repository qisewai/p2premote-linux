#include <p2premote/stream_session.h>

#include "d3d11_preview.h"
#include "d3d11_video.h"
#include "cursor_shape.h"
#include "desktop_input.h"
#include "dxgi_capturer.h"
#include "qpc.h"
#include "rtc_call.h"
#include "signal_socket.h"
#include "stream_events.h"
#ifdef P2PREMOTE_HAVE_MINIRTC
#include "minirtc_call.h"
#endif

#include "api/video/i420_buffer.h"
#include "api/make_ref_counted.h"
#include "libyuv/convert.h"
#include "libyuv/convert_argb.h"
#include "libyuv/convert_from_argb.h"
#include "libyuv/scale.h"
#include "rtc_base/time_utils.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <utility>

namespace p2premote {
namespace {

constexpr ULONGLONG reconnect_window_ms = 30'000;
constexpr ULONGLONG close_grace_ms = 1'000;
constexpr ULONGLONG video_settings_timeout_ms = 3'000;

struct VideoLimits {
    int width;
    int height;
};

constexpr VideoLimits LimitsFor(StreamQuality quality) {
    switch (quality) {
    case StreamQuality::p1440: return {2560, 1440};
    case StreamQuality::p1080: return {1920, 1080};
    case StreamQuality::p720: return {1280, 720};
    }
    return {1280, 720};
}

constexpr int LegacyVideoBitrate(StreamQuality quality) {
    switch (quality) {
    case StreamQuality::p720: return VideoBitrateLimit(VideoBitratePreset::economy);
    case StreamQuality::p1080: return VideoBitrateLimit(VideoBitratePreset::automatic);
    case StreamQuality::p1440: return VideoBitrateLimit(VideoBitratePreset::detail);
    }
    return VideoBitrateLimit(VideoBitratePreset::economy);
}

bool NeedsCaptureRateLimit(HMONITOR monitor, unsigned target_fps) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    return !monitor || !GetMonitorInfoW(monitor, &info) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)
        || mode.dmDisplayFrequency <= 1 || mode.dmDisplayFrequency > target_fps;
}

std::pair<int, int> ScaledSize(int width, int height, VideoLimits limits) {
    const auto scale = std::min({1.0, static_cast<double>(limits.width) / width,
        static_cast<double>(limits.height) / height});
    return {std::max(2, static_cast<int>(width * scale) & ~1), std::max(2, static_cast<int>(height * scale) & ~1)};
}

class CapturePixels {
public:
    explicit CapturePixels(ID3D11Device* device) : device_(device) { device->GetImmediateContext(context_.put()); }
    ~CapturePixels() {
        staging_ = nullptr;
        context_->ClearState();
        context_->Flush();
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> Convert(const CapturedFrame& frame, VideoLimits limits) {
        const auto width = static_cast<int>(frame.metadata.width);
        const auto height = static_cast<int>(frame.metadata.height);
        if (!raw_ || raw_->width() != width || raw_->height() != height) {
            D3D11_TEXTURE2D_DESC spec{};
            spec.Width = frame.metadata.width;
            spec.Height = frame.metadata.height;
            spec.MipLevels = spec.ArraySize = spec.SampleDesc.Count = 1;
            spec.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            spec.Usage = D3D11_USAGE_STAGING;
            spec.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_ = nullptr;
            winrt::check_hresult(device_->CreateTexture2D(&spec, nullptr, staging_.put()));
            raw_ = webrtc::I420Buffer::Create(width, height);
        }

        const D3D11_BOX content{0, 0, 0, frame.metadata.width, frame.metadata.height, 1};
        context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, frame.texture.get(), 0, &content);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto converted = libyuv::ARGBToI420Matrix(static_cast<const std::uint8_t*>(mapped.pData),
            static_cast<int>(mapped.RowPitch), raw_->MutableDataY(), raw_->StrideY(), raw_->MutableDataU(),
            raw_->StrideU(), raw_->MutableDataV(), raw_->StrideV(), &libyuv::kArgbH709Constants, width, height);
        context_->Unmap(staging_.get(), 0);
        if (converted != 0) {
            throw std::runtime_error("无法转换屏幕帧颜色格式");
        }

        const auto [output_width, output_height] = ScaledSize(width, height, limits);
        auto output = webrtc::I420Buffer::Create(output_width, output_height);
        output->ScaleFrom(*raw_);
        return output;
    }

private:
    ID3D11Device* device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    webrtc::scoped_refptr<webrtc::I420Buffer> raw_;
};

struct ActiveStream {
    struct VideoRequest {
        InputPacket packet;
        ULONGLONG deadline;
        bool retried{};
    };
    explicit ActiveStream(std::shared_ptr<LatencyTrace> trace)
        : events(std::make_shared<StreamEvents>(std::move(trace))) {}

    ~ActiveStream() {
        events->SetDesktopInput(nullptr);
        DestroyDecodedCursor(remote_cursor);
    }

    std::shared_ptr<StreamEvents> events;
    std::unique_ptr<SignalSocket> signal;
    std::unique_ptr<MediaCall> rtc;
    std::unique_ptr<DesktopInput> desktop;
    winrt::com_ptr<ID3D11Device> device;
    std::unique_ptr<IVideoCapturer> capture;
    std::unique_ptr<CapturePixels> pixels;
    std::unique_ptr<D3d11FrameDifference> frame_difference;
    std::optional<webrtc::VideoFrame> last_video;
    StreamQuality last_video_quality{};
    std::int64_t last_submit_qpc{};
    std::int64_t last_change_qpc{};
    std::int64_t quality_refresh_due{};
    unsigned refinement_left{};
    std::uint64_t idle_repeats{};
    bool dynamic_cadence{};
    std::unique_ptr<D3d11VideoConverter> converter;
    bool gpu_conversion_failed{};
    bool gpu_backbuffer_failed{};
    std::unique_ptr<D3d11Preview> renderer;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> last_native;
    std::vector<std::uint8_t> bgra;
    HMONITOR monitor{};
    HWND surface{};
    std::int64_t negotiation_start{};
    std::int64_t next_capture{};
    std::int64_t capture_interval{};
    std::int64_t last_frame_timestamp_us{};
    bool dxgi_capture{};
    bool dxgi_attempted{};
    bool redraw{};
    bool host_registered{};
    ULONGLONG transport_close_deadline{};
    bool sdp_sent{};
    bool remote_sdp_set{};
    bool minirtc_supported{};
    bool minirtc{};
    std::vector<std::string> local_ice;
    std::vector<std::string> remote_ice;
    std::optional<IceCheckStats> ice_checks;
    // Start at 720p so older Viewers can decode the stream before any quality request.
    StreamQuality quality{StreamQuality::p720};
    ULONGLONG video_probe_deadline{};
    bool legacy_settings_sent{};
    std::optional<VideoRequest> video_request;
    std::optional<std::pair<StreamQuality, VideoBitratePreset>> rejected_video_settings;
    StreamInputMode input_mode{};
    bool input_ready{};
    bool cursor_requested{};
    bool cursor_separate{};
    bool cursor_sent{};
    bool cursor_visible{};
    HCURSOR last_cursor{};
    HCURSOR remote_cursor{};
    std::uint32_t outgoing_input_sequence{};
    InputReceiveState input_state;
    std::vector<InputPacket> input;

    RtcCall& WebRtc() { return static_cast<RtcCall&>(*rtc); }

    void StartFrameComparison() {
        if (!dynamic_cadence) return;
        try {
            frame_difference = std::make_unique<D3d11FrameDifference>(device.get());
        } catch (const winrt::hresult_error& error) {
            if (const auto& trace = events->Trace()) {
                trace->Record("frame_comparison_unavailable", static_cast<std::uint32_t>(error.code().value));
            }
        }
    }

    bool ShouldSubmit(const CapturedFrame& frame) {
        if (!frame_difference) return true;
        const auto width = frame.metadata.width;
        const auto height = frame.metadata.height;
        std::optional<UINT> changed;
        try {
            changed = frame_difference->ChangedTiles(frame.texture.get(), width, height);
        } catch (const winrt::hresult_error& error) {
            frame_difference.reset();
            if (const auto& trace = events->Trace()) {
                trace->Record("frame_comparison_unavailable", static_cast<std::uint32_t>(error.code().value));
            }
        }
        if (!last_video || last_video_quality != quality
            || (changed && *changed * 20 >= D3d11FrameDifference::TileCount(width, height))) {
            refinement_left = 6;
            last_change_qpc = QpcNow();
            return true;
        }
        if (!changed || *changed) return true;
        if (!refinement_left || QpcNow() - last_change_qpc > QpcFrequency() / 4) return false;
        --refinement_left;
        return true;
    }

    const wchar_t* SharingStatus() const {
        return dxgi_capture ? L"正在共享所选显示器 · DXGI 桌面复制" : L"正在共享所选显示器 · WGC";
    }

    void ResetInput() {
        input.clear();
        if (desktop) desktop->Submit({InputKind::reset});
        else input.push_back({InputKind::reset});
    }

    winrt::com_ptr<ID3D11Texture2D> ConvertOnGpu(ID3D11Texture2D* source, UINT subresource,
        UINT source_width, UINT source_height, UINT width, UINT height, DXGI_FORMAT format) {
        if (gpu_conversion_failed) return {};
        try {
            if (!converter) converter = std::make_unique<D3d11VideoConverter>(device.get());
            return converter->Convert(source, subresource, source_width, source_height, width, height, format);
        } catch (const winrt::hresult_error&) {
            converter.reset();
            gpu_conversion_failed = true;
            return {};
        }
    }

    bool WriteBackBuffer(const Nv12Frame& frame) {
        if (gpu_backbuffer_failed || gpu_conversion_failed) return false;
        try {
            if (!converter) converter = std::make_unique<D3d11VideoConverter>(device.get());
            const auto fit = renderer->Viewport(frame.width(), frame.height());
            const RECT destination{static_cast<LONG>(fit.left), static_cast<LONG>(fit.top),
                static_cast<LONG>(fit.left + fit.width), static_cast<LONG>(fit.top + fit.height)};
            converter->Blit(frame.texture(), frame.subresource(), frame.width(), frame.height(), renderer->BackBuffer(), destination);
            return true;
        } catch (const winrt::hresult_error&) {
            gpu_backbuffer_failed = true;
            return false;
        }
    }
};

const char* InitialSignal(ConnectionMode mode, bool host) {
    switch (mode) {
    case ConnectionMode::internet: return host ? "listen_internet" : "join_internet";
    case ConnectionMode::lan_direct: return host ? "listen_direct" : "join_direct";
    case ConnectionMode::session_code: return host ? "create" : "join";
    }
    return "";
}

const char* IcePathName(IcePath path) {
    switch (path) {
    case IcePath::host: return "host";
    case IcePath::nat: return "nat";
    case IcePath::relay: return "relay";
    case IcePath::unknown: return "";
    }
    return "";
}

} // namespace

struct StreamSession::Impl {
    struct Reconnect {
        HMONITOR monitor{};
        HWND surface{};
        StreamFrameRate frame_rate{};
        ULONGLONG deadline{};
        ULONGLONG next_attempt{};
        unsigned attempts{};
        bool desktop{};
    };

    Impl(bool is_host, ConnectionOptions options) : host(is_host), connection(std::move(options)),
        requested_quality(connection.SupportsQualitySelection() ? StreamQuality::p1080 : StreamQuality::p720) {
        if (connection.signal_url.empty()) connection.signal_url = L"ws://127.0.0.1:8787/signal";
    }

    void StartWgc(ActiveStream& stream, bool capture_cursor) {
        if (!capture_item || capture_monitor != stream.monitor) {
            capture_item = CreateMonitorCaptureItem(stream.monitor);
            capture_monitor = stream.monitor;
        }
        stream.capture = StartMonitorCapture(capture_item, stream.device.get(), capture_cursor);
        stream.dxgi_capture = false;
        stream.next_capture = 0;
    }

    bool ApplyVideoSettings(ActiveStream& stream, StreamQuality resolution, int bitrate) {
        if (stats.video_bitrate_limit_bps != bitrate) {
            if (!stream.rtc->SetVideoBitrate(bitrate)) return false;
            stats.video_bitrate_limit_bps = bitrate;
        }
        if (stream.quality != resolution) {
            stream.quality = resolution;
            if (!stream.dxgi_capture) stream.quality_refresh_due = QpcNow() + QpcFrequency();
        }
        detail = stream.SharingStatus();
        return true;
    }

    bool host;
    ConnectionOptions connection;
    bool drawable{true};
    StreamState state{StreamState::idle};
    std::wstring detail;
    std::wstring code;
    HMONITOR capture_monitor{};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item{nullptr};
    std::unique_ptr<ActiveStream> active;
    std::optional<Reconnect> reconnect;
    StreamStatistics stats;
    StreamQuality requested_quality;
    VideoBitratePreset requested_bitrate{VideoBitratePreset::automatic};
    std::int64_t start{};
    double capture_to_submit_total{};
    double draw_to_present_total{};
    std::int64_t decoded_frame_wait_ticks{};
    std::deque<StreamStatistics> samples;
};

StreamSession::StreamSession(bool host, ConnectionOptions connection)
    : impl_(std::make_unique<Impl>(host, std::move(connection))) {}
StreamSession::~StreamSession() = default;

void StreamSession::Start(HMONITOR monitor, HWND surface, const std::wstring& code, StreamFrameRate frame_rate) {
    if (impl_->active) return;
    impl_->reconnect.reset();
    Open(monitor, surface, code, frame_rate);
}

void StreamSession::Open(HMONITOR monitor, HWND surface, const std::wstring& code, StreamFrameRate frame_rate) {
    const auto& connection = impl_->connection;
    auto trace = connection.latency_trace_dir.empty() ? nullptr
        : std::make_shared<LatencyTrace>(connection.latency_trace_dir, impl_->host);
    auto active = std::make_unique<ActiveStream>(std::move(trace));
    active->monitor = monitor;
    active->surface = surface;
    active->dynamic_cadence = impl_->host && connection.DynamicCadenceEnabled();
    const auto target_fps = static_cast<unsigned>(frame_rate);
    active->capture_interval = impl_->host && NeedsCaptureRateLimit(monitor, target_fps) ? QpcFrequency() / target_fps : 0;
    const auto& target = connection.managed && !impl_->host ? connection.remote_device : code;
#ifdef P2PREMOTE_HAVE_MINIRTC
    active->minirtc_supported = connection.managed && MiniRtcCall::Available(connection);
#endif
    active->signal = std::make_unique<SignalSocket>(active->events, connection.signal_url,
        InitialSignal(connection.mode, impl_->host), winrt::to_string(target), connection.access_token,
        connection.managed, active->minirtc_supported);
    impl_->samples.clear();
    impl_->stats = {};
    impl_->stats.target_fps = impl_->host ? target_fps : 0;
    impl_->capture_to_submit_total = impl_->draw_to_present_total = 0;
    impl_->decoded_frame_wait_ticks = 0;
    impl_->active = std::move(active);
    impl_->code.clear();
    impl_->detail = L"正在连接信令服务 · " + connection.signal_url;
    impl_->state = StreamState::waiting;
}

void StreamSession::Approve(StreamInputMode input) {
    if (!impl_->host || impl_->state != StreamState::approval) {
        return;
    }
    auto& active = *impl_->active;
    if (input == StreamInputMode::desktop && impl_->connection.mode == ConnectionMode::session_code) {
        throw std::logic_error("整桌面控制需要局域网直连或认证公网会话");
    }
    active.input_mode = input;
    if (input == StreamInputMode::desktop) active.desktop = std::make_unique<DesktopInput>(active.monitor, active.events->Trace().get());
    active.negotiation_start = QpcNow();
    impl_->state = StreamState::negotiating;
    impl_->detail = L"已授权，正在建立视频连接";
    const bool allow_input = input != StreamInputMode::view_only;
#ifdef P2PREMOTE_HAVE_MINIRTC
    if (active.minirtc) {
        active.rtc = std::make_unique<MiniRtcCall>(true, active.events, impl_->connection, allow_input,
            impl_->stats.target_fps);
        active.signal->Send("accept", allow_input ? "control" : "view", true);
        return;
    }
#endif
    if (impl_->connection.hardware_video) active.device = CreateD3d11Device(true);
    active.rtc = std::make_unique<RtcCall>(
        true, active.events, impl_->connection, allow_input, impl_->stats.target_fps, active.device.get());
    active.signal->Send("accept");
    active.WebRtc().SetLocal();
}

void StreamSession::SetQuality(StreamQuality quality) {
    impl_->requested_quality = quality;
    if (impl_->active) impl_->active->rejected_video_settings.reset();
    UpdateVideoSettings();
}

void StreamSession::SetVideoBitratePreset(VideoBitratePreset preset) {
    impl_->requested_bitrate = preset;
    if (impl_->active) impl_->active->rejected_video_settings.reset();
    UpdateVideoSettings();
}

void StreamSession::UpdateVideoSettings() {
    if (impl_->host || impl_->state != StreamState::streaming || !impl_->connection.SupportsQualitySelection()) return;
    auto& active = *impl_->active;
    if (active.transport_close_deadline) return;
    auto& stats = impl_->stats;
    const auto now = GetTickCount64();
    const auto resolution = impl_->requested_quality;
    const auto preset = impl_->requested_bitrate;

    if (stats.video_settings_mode == VideoSettingsMode::detecting) {
        if (!active.video_probe_deadline) active.video_probe_deadline = now + video_settings_timeout_ms;
        if (now >= active.video_probe_deadline) {
            stats.video_settings_mode = VideoSettingsMode::legacy;
            active.video_request.reset();
        }
    }
    if (stats.video_settings_mode == VideoSettingsMode::legacy) {
        if ((!active.legacy_settings_sent || active.quality != resolution)
                && SubmitInput({InputKind::set_quality, static_cast<std::uint16_t>(resolution)})) {
            active.legacy_settings_sent = true;
            active.quality = resolution;
        }
        return;
    }
    if (active.video_request) {
        auto& request = *active.video_request;
        if (now < request.deadline) {
            if (!request.retried && now + video_settings_timeout_ms / 2 >= request.deadline) {
                request.retried = active.rtc->SendVideoSettings(request.packet);
            }
            return;
        }
        active.rejected_video_settings = std::pair{static_cast<StreamQuality>(request.packet.code),
            static_cast<VideoBitratePreset>(request.packet.x)};
        active.video_request.reset();
        impl_->detail = L"画质设置未获确认";
    }
    if ((active.quality == resolution && stats.video_bitrate_limit_bps == VideoBitrateLimit(preset))
            || active.rejected_video_settings == std::pair{resolution, preset}) return;

    const InputPacket request{InputKind::set_video_settings, static_cast<std::uint16_t>(resolution),
        static_cast<std::uint16_t>(preset), 0, ++active.outgoing_input_sequence, now};
    if (active.rtc->SendVideoSettings(request)) active.video_request = ActiveStream::VideoRequest{request, now + video_settings_timeout_ms};
}

void StreamSession::Stop(std::wstring status) {
    impl_->reconnect.reset();
    Close(std::move(status), true);
}

void StreamSession::Close(std::wstring status, bool intentional) {
    if (auto active = std::move(impl_->active)) {
        if (impl_->state == StreamState::negotiating && active->ice_checks) {
            const auto& checks = *active->ice_checks;
            status += L" · ICE 发送 " + std::to_wstring(checks.requests_sent)
                + L"，收到回复 " + std::to_wstring(checks.responses_received)
                + L"，收到对端探测 " + std::to_wstring(checks.requests_received);
            if (!checks.dtls_state.empty()) status += L" · DTLS " + std::wstring(winrt::to_hstring(checks.dtls_state));
        }
        active->events->SetDesktopInput(nullptr);
        active->desktop.reset();
        active->capture.reset();
        active->pixels.reset();
        if (intentional) active->signal->EndSession();
        active->signal.reset();
        active->rtc.reset();
    }
    impl_->state = StreamState::stopped;
    impl_->detail = std::move(status);
    impl_->code.clear();
}

void StreamSession::Recover(std::wstring status) {
    auto& retry = impl_->reconnect;
    const auto& active = *impl_->active;
    if ((impl_->connection.mode != ConnectionMode::lan_direct && !impl_->connection.managed)
        || (!retry && impl_->state != StreamState::streaming && !active.host_registered)) {
        Close(std::move(status), false);
        return;
    }
    const auto now = GetTickCount64();
    if (!retry) {
        retry.emplace(Impl::Reconnect{active.monitor, active.surface,
            impl_->host ? static_cast<StreamFrameRate>(impl_->stats.target_fps) : StreamFrameRate::fps30,
            now + reconnect_window_ms, 0, 0, DesktopControl()});
    }
    const auto delay_seconds = 1u << std::min(retry->attempts, 2u);
    retry->next_attempt = now + delay_seconds * 1'000;
    Close(std::move(status), false);
    impl_->state = StreamState::reconnecting;
}

void StreamSession::Resize(bool drawable) noexcept {
    impl_->drawable = drawable;
    if (impl_->active) {
        impl_->active->redraw = true;
    }
}

void StreamSession::DisplayChanged() noexcept {
    impl_->capture_item = nullptr;
    impl_->capture_monitor = nullptr;
}

HANDLE StreamSession::Event() const noexcept {
    return impl_->active ? impl_->active->events->Ready() : nullptr;
}

HANDLE StreamSession::CaptureEvent() const noexcept {
    return impl_->active && impl_->active->capture ? impl_->active->capture->ReadyEvent() : nullptr;
}

HANDLE StreamSession::PresentEvent() const {
    if (impl_->active && impl_->active->renderer && impl_->drawable) {
        const auto& active = *impl_->active;
        if (active.rtc->HasFrame() || (active.redraw && (active.last_native || active.renderer->HasImage()))) {
            return active.renderer->PresentEvent();
        }
    }
    return nullptr;
}

void StreamSession::ProcessEvents() {
    const auto mode = impl_->connection.mode;
    const auto local_cursor = impl_->connection.SupportsLocalCursor();
    for (const auto& event : impl_->active->events->Drain()) {
        auto& active = *impl_->active;
        switch (event.type) {
        case StreamEventType::signal_lost:
        case StreamEventType::media_lost:
            if ((mode == ConnectionMode::lan_direct || impl_->connection.managed)
                && impl_->state == StreamState::streaming) {
                if (!active.transport_close_deadline) {
                    // Stop accepting input immediately and let a pending explicit end arrive first.
                    active.transport_close_deadline = GetTickCount64() + close_grace_ms;
                    active.events->SetDesktopInput(nullptr);
                    active.ResetInput();
                    impl_->detail = winrt::to_hstring(event.payload).c_str();
                }
                break;
            }
            Recover(winrt::to_hstring(event.payload).c_str());
            return;
        case StreamEventType::error:
            Stop(winrt::to_hstring(event.payload).c_str());
            return;
        case StreamEventType::signal:
            if (event.retryable && (event.name == "ended" || (event.name == "error" && impl_->reconnect))) {
                Recover(winrt::to_hstring(event.payload).c_str());
                return;
            }
            if (event.name == "ended" && impl_->host && impl_->connection.auto_control) {
                const auto monitor = active.monitor;
                const auto frame_rate = static_cast<StreamFrameRate>(impl_->stats.target_fps);
                Stop(L"Viewer 已结束会话");
                Start(monitor, nullptr, {}, frame_rate);
                return;
            }
            if (event.name == "error" || event.name == "ended") {
                Stop(winrt::to_hstring(event.payload).c_str());
                return;
            }
            if (impl_->host && mode != ConnectionMode::session_code && impl_->state == StreamState::waiting && event.name == "ready") {
                active.host_registered = true;
                impl_->reconnect.reset();
                impl_->detail = impl_->connection.managed ? L"等待同账号设备连接"
                    : mode == ConnectionMode::internet ? L"等待已配置的公网主控机" : L"等待主控机直连";
                if (impl_->connection.auto_control) impl_->detail += L" · 自动允许整桌面控制";
            } else if (impl_->host && mode == ConnectionMode::session_code && impl_->state == StreamState::waiting && event.name == "room") {
                impl_->code = winrt::to_hstring(event.payload).c_str();
                impl_->detail = L"等待 Viewer 连接（代码 2 分钟内有效）";
            } else if (impl_->host && impl_->state == StreamState::waiting && event.name == "request") {
                impl_->state = StreamState::approval;
                active.minirtc = active.minirtc_supported && event.minirtc;
                if (impl_->connection.auto_control) Approve(StreamInputMode::desktop);
                else if (mode == ConnectionMode::internet) impl_->detail = L"收到已认证的公网请求，请确认授权";
                else if (mode == ConnectionMode::lan_direct) {
                    impl_->detail = L"收到直连请求 · " + winrt::to_hstring(event.payload) + L" · 请确认授权";
                } else impl_->detail = L"收到观看请求，请确认是否共享所选显示器";
            } else if (!impl_->host && event.name == "waiting" && impl_->state == StreamState::waiting) {
                impl_->detail = L"等待 Host 确认";
            } else if (!impl_->host && event.name == "accepted" && impl_->state == StreamState::waiting) {
                impl_->state = StreamState::negotiating;
                impl_->detail = L"Host 已允许，正在建立视频连接";
                active.negotiation_start = QpcNow();
                if (event.minirtc && !active.minirtc_supported) {
                    Stop(L"会话请求了不可用的 MiniRTC 模块");
                    return;
                }
                active.minirtc = event.minirtc;
#ifdef P2PREMOTE_HAVE_MINIRTC
                if (active.minirtc) {
                    active.rtc = std::make_unique<MiniRtcCall>(false, active.events, impl_->connection,
                        event.payload == "control", 30);
                    break;
                }
#endif
                if (impl_->connection.hardware_video) active.device = CreateD3d11Device(true);
                active.rtc = std::make_unique<RtcCall>(false, active.events, impl_->connection, false, 30, active.device.get());
#ifdef P2PREMOTE_HAVE_MINIRTC
            } else if (active.minirtc && active.rtc && event.name == "minirtc" &&
                (impl_->state == StreamState::negotiating || impl_->state == StreamState::streaming)) {
                static_cast<MiniRtcCall&>(*active.rtc).ReceiveSignal(event.payload);
#endif
            } else if (!active.minirtc && mode == ConnectionMode::internet && active.rtc && event.name == "ice"
                && (impl_->state == StreamState::negotiating || impl_->state == StreamState::streaming)) {
                if (active.remote_sdp_set) active.WebRtc().AddRemoteIce(event.payload);
                else {
                    if (active.remote_ice.size() >= 64) throw std::runtime_error("远端 ICE 候选过多");
                    active.remote_ice.push_back(event.payload);
                }
            } else if (!active.minirtc && impl_->state == StreamState::negotiating &&
                active.rtc && event.name == (impl_->host ? "answer" : "offer")) {
                active.WebRtc().SetRemote(event.payload);
            } else {
                Stop(L"收到与当前会话状态不符的信令");
                return;
            }
            break;
        case StreamEventType::remote_set:
            active.remote_sdp_set = true;
            for (const auto& candidate : active.remote_ice) active.WebRtc().AddRemoteIce(candidate);
            active.remote_ice.clear();
            if (!impl_->host) {
                active.WebRtc().SetLocal();
            }
            break;
        case StreamEventType::gathered:
        case StreamEventType::local_set:
            if (!active.sdp_sent && (mode == ConnectionMode::internet) == (event.type == StreamEventType::local_set)) {
                active.signal->Send(impl_->host ? "offer" : "answer", active.WebRtc().LocalSdp());
                active.sdp_sent = true;
                for (auto& candidate : active.local_ice) active.signal->Send("ice", std::move(candidate));
                active.local_ice.clear();
            }
            break;
        case StreamEventType::ice_candidate:
            if (active.sdp_sent) active.signal->Send("ice", event.payload);
            else {
                if (active.local_ice.size() >= 64) throw std::runtime_error("本机 ICE 候选过多");
                active.local_ice.push_back(event.payload);
            }
            break;
        case StreamEventType::minirtc_signal:
#ifdef P2PREMOTE_HAVE_MINIRTC
            if (active.minirtc && MiniRtcCall::IsSessionSignal(event.payload)) {
                active.signal->Send("minirtc", event.payload);
            }
#endif
            break;
        case StreamEventType::connected:
            if (impl_->state == StreamState::negotiating) {
                if (impl_->host) {
                    if (!active.device) active.device = CreateD3d11Device();
                    active.pixels = std::make_unique<CapturePixels>(active.device.get());
                    if (impl_->connection.SupportsQualitySelection()) {
                        impl_->ApplyVideoSettings(active, active.quality, LegacyVideoBitrate(active.quality));
                    }
                    active.StartFrameComparison();
                    try {
                        impl_->StartWgc(active, active.input_mode != StreamInputMode::test_pad);
                    } catch (...) {
                        DisplayChanged();
                        throw;
                    }
                } else {
                    active.renderer = std::make_unique<D3d11Preview>(active.surface, active.device);
                }
                impl_->start = QpcNow();
                impl_->state = StreamState::streaming;
                impl_->reconnect.reset();
                if (active.input_ready) active.events->SetDesktopInput(active.desktop.get());
                impl_->detail = impl_->host ? active.SharingStatus() : L"正在接收视频";
            }
            break;
        case StreamEventType::stats: {
            const auto& rtc = event.counters;
            auto& stats = impl_->stats;
            if (mode == ConnectionMode::internet && !active.minirtc) {
                auto& peak = stats.ice_diagnostics ? *stats.ice_diagnostics : stats.ice_diagnostics.emplace();
                const auto& sample = rtc.ice_diagnostics;
                peak.local_srflx = std::max(peak.local_srflx, sample.local_srflx);
                peak.remote_srflx = std::max(peak.remote_srflx, sample.remote_srflx);
                peak.direct_udp_pairs = std::max(peak.direct_udp_pairs, sample.direct_udp_pairs);
                peak.direct_udp_succeeded = std::max(peak.direct_udp_succeeded, sample.direct_udp_succeeded);
                peak.direct_udp_requests = std::max(peak.direct_udp_requests, sample.direct_udp_requests);
                peak.direct_udp_responses = std::max(peak.direct_udp_responses, sample.direct_udp_responses);
            }
            if (rtc.ice_checks) {
                auto& checks = active.ice_checks;
                if (!checks) checks.emplace();
                checks->requests_sent = std::max(checks->requests_sent, rtc.ice_checks->requests_sent);
                checks->requests_received = std::max(checks->requests_received, rtc.ice_checks->requests_received);
                checks->responses_received = std::max(checks->responses_received, rtc.ice_checks->responses_received);
                if (!rtc.ice_checks->dtls_state.empty()) checks->dtls_state = rtc.ice_checks->dtls_state;
                break;
            }
            stats.encoded = rtc.encoded;
            stats.received = rtc.received;
            stats.decoded = rtc.decoded;
            stats.bytes_sent = rtc.bytes_sent;
            stats.bytes_received = rtc.bytes_received;
            stats.video_packets_received = rtc.video_packets_received;
            stats.video_packets_lost = rtc.video_packets_lost;
            stats.upload_bps = rtc.upload_bps;
            stats.download_bps = rtc.download_bps;
            stats.video_loss_fraction = rtc.video_loss_fraction;
            stats.minirtc = active.minirtc;
            stats.srtp_active = rtc.srtp_active;
            if (!rtc.video_codec.empty()) {
                const auto slash = rtc.video_codec.find('/');
                stats.video_codec = winrt::to_hstring(rtc.video_codec.substr(slash == std::string::npos ? 0 : slash + 1));
            }
            stats.encode_ms = rtc.encoded ? rtc.encode_seconds * 1000 / static_cast<double>(rtc.encoded) : 0;
            stats.decode_ms = rtc.decoded ? rtc.decode_seconds * 1000 / static_cast<double>(rtc.decoded) : 0;
            if (rtc.sent_packets) {
                stats.packet_send_queue_ms = rtc.packet_send_queue_seconds * 1000 / static_cast<double>(rtc.sent_packets);
            }
            if (rtc.jitter_buffer_frames) {
                stats.jitter_buffer_ms = rtc.jitter_buffer_seconds * 1000 / static_cast<double>(rtc.jitter_buffer_frames);
            }
            stats.rtc_frames_dropped = rtc.rtc_frames_dropped;
            stats.ice_path = rtc.ice_path;
            stats.ice_protocol = rtc.ice_protocol;
            stats.relay_protocol = rtc.relay_protocol;
            stats.ice_rtt_ms = rtc.ice_rtt_ms;
            if (impl_->state == StreamState::streaming) {
                if (!impl_->host) stats.decoded_replaced = active.rtc->ReplacedFrames();
                stats.seconds = QpcMilliseconds(QpcNow() - impl_->start) / 1000;
                impl_->samples.push_back(stats);
                if (impl_->samples.size() > 3600) {
                    impl_->samples.pop_front();
                }
            }
            break;
        }
        case StreamEventType::input_ready:
            active.input_ready = active.rtc && active.rtc->InputReady();
            active.events->SetDesktopInput(InputReady() ? active.desktop.get() : nullptr);
            if (!active.input_ready) {
                active.ResetInput();
                if (impl_->host && active.cursor_separate && active.capture) {
                    if (active.dxgi_capture) {
                        impl_->StartWgc(active, true);
                        impl_->detail = active.SharingStatus();
                    } else {
                        active.capture->SetCursorCapture(true);
                    }
                }
                active.cursor_requested = active.cursor_separate = active.cursor_sent = false;
                DestroyDecodedCursor(std::exchange(active.remote_cursor, nullptr));
            }
            break;
        case StreamEventType::cursor:
            if (!impl_->host && active.cursor_requested && local_cursor) {
                DestroyDecodedCursor(std::exchange(active.remote_cursor, DecodeCursor(event.payload)));
                active.cursor_visible = event.payload.size() != 1;
                active.cursor_separate = true;
            }
            break;
        case StreamEventType::video_settings: {
            if (impl_->state != StreamState::streaming || active.transport_close_deadline
                    || !impl_->connection.SupportsQualitySelection()) break;
            const auto& settings = event.input;
            const auto resolution = static_cast<StreamQuality>(settings.code);
            const auto preset = static_cast<VideoBitratePreset>(settings.x);
            if (impl_->host) {
                if (settings.kind != InputKind::set_video_settings) break;
                auto reply = settings;
                reply.kind = InputKind::video_settings_reply;
                reply.y = impl_->ApplyVideoSettings(active, resolution, VideoBitrateLimit(preset)) ? 0 : 1;
                active.rtc->SendVideoSettings(reply);
            } else {
                if (settings.kind != InputKind::video_settings_reply || !active.video_request) break;
                const auto& request = active.video_request->packet;
                if (settings.sequence != request.sequence || settings.code != request.code || settings.x != request.x) break;
                active.video_request.reset();
                impl_->stats.video_settings_mode = VideoSettingsMode::independent;
                if (!settings.y) {
                    active.quality = resolution;
                    impl_->stats.video_bitrate_limit_bps = VideoBitrateLimit(preset);
                    active.rejected_video_settings.reset();
                    impl_->detail = L"正在接收视频";
                } else {
                    active.rejected_video_settings = std::pair{resolution, preset};
                    impl_->detail = L"画质设置未生效";
                }
            }
            break;
        }
        case StreamEventType::input: {
            if (!impl_->host || active.input_mode == StreamInputMode::view_only || !InputReady()) {
                break;
            }
            const auto& input = event.input;
            if (input.kind == InputKind::cursor_ready) {
                if (local_cursor && active.input_mode == StreamInputMode::desktop) {
                    active.cursor_requested = true;
                }
                break;
            }
            if (input.kind == InputKind::set_quality) {
                if (!impl_->connection.SupportsQualitySelection()) break;
                const auto quality = static_cast<StreamQuality>(input.code);
                if (!impl_->ApplyVideoSettings(active, quality, LegacyVideoBitrate(quality))) {
                    impl_->detail = L"正在共享所选显示器 · 画质调整失败";
                }
                break;
            }
            if (active.desktop) break;
            if (const auto accepted = active.input_state.Accept(input, GetTickCount64())) {
                if (!accepted->sequence) active.ResetInput();
                else active.input.push_back(*accepted);
            }
            break;
        }
        }
    }
    UpdateVideoSettings();
}

bool StreamSession::InputReady() const noexcept {
    return impl_->state == StreamState::streaming && impl_->active && impl_->active->input_ready
        && !impl_->active->transport_close_deadline;
}

bool StreamSession::LocalCursor(HCURSOR& cursor) const noexcept {
    if (impl_->host || !InputReady() || !impl_->active->cursor_separate) return false;
    const auto& active = *impl_->active;
    cursor = active.cursor_visible
        ? (active.remote_cursor ? active.remote_cursor : LoadCursorW(nullptr, IDC_ARROW)) : nullptr;
    return true;
}

void StreamSession::PollCursor() {
    if (!impl_->connection.SupportsLocalCursor() || !InputReady()) return;
    auto& active = *impl_->active;
    if (!impl_->host) {
        if (!impl_->connection.video_cursor && !active.cursor_requested) {
            active.cursor_requested = SubmitInput({InputKind::cursor_ready});
        }
        return;
    }
    if (!active.cursor_requested) return;
    if (impl_->connection.desktop_duplication && !active.dxgi_attempted) {
        active.dxgi_attempted = true;
        try {
            active.capture = StartDesktopDuplication(active.monitor, active.device.get());
            active.dxgi_capture = true;
            active.cursor_separate = true;
            active.next_capture = 0;
            impl_->detail = active.SharingStatus();
        } catch (const winrt::hresult_error&) {
            // Keep WGC for unsupported displays and D3D devices.
        }
    }
    if (!active.cursor_separate) {
        active.cursor_separate = active.capture->SetCursorCapture(false);
        if (!active.cursor_separate) {
            active.cursor_requested = false;
            return;
        }
    }

    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) return;
    const bool visible = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor;
    if (active.cursor_sent && visible == active.cursor_visible && (!visible || info.hCursor == active.last_cursor)) return;
    auto packet = visible ? EncodeCursor(info.hCursor) : std::string(1, cursor_marker);
    if (packet.empty()) packet = {cursor_marker, 'A'};
    if (active.rtc->SendCursor(packet)) {
        active.cursor_sent = true;
        active.cursor_visible = visible;
        active.last_cursor = info.hCursor;
    }
}

bool StreamSession::DesktopControl() const noexcept {
    return (impl_->active && impl_->active->input_mode == StreamInputMode::desktop)
        || (impl_->reconnect && impl_->reconnect->desktop);
}

bool StreamSession::SubmitInput(InputPacket input) {
    if (impl_->host || !InputReady()) {
        return false;
    }
    auto& active = *impl_->active;
    input.sequence = ++active.outgoing_input_sequence;
    input.sent_ms = GetTickCount64();
    if (const auto& trace = active.events->Trace()) trace->Pointer("input_generated", input);
    return active.rtc->SendInput(input);
}

std::vector<InputPacket> StreamSession::TakeInput() {
    return impl_->active ? std::exchange(impl_->active->input, {}) : std::vector<InputPacket>{};
}

void StreamSession::Capture() {
    auto& active = *impl_->active;
    if (active.capture->IsSourceClosed()) {
        if (active.dxgi_capture) {
            impl_->StartWgc(active, !active.cursor_separate);
            impl_->detail = active.SharingStatus();
            return;
        }
        DisplayChanged();
        Stop(L"Windows 已关闭采集源");
        return;
    }
    impl_->stats.captured += active.capture->Poll().acquired;
    const auto frame = active.capture->TakeFrame();
    if (!frame) {
        return;
    }
    const auto& trace = active.events->Trace();
    if (trace) {
        if (frame->metadata.source_qpc) {
            trace->Record("frame_source", frame->metadata.frame_id, 0, 0, 0, frame->metadata.source_qpc);
        }
        trace->Record("frame_captured", frame->metadata.frame_id, 0, 0, 0, frame->metadata.capture_qpc);
    }
    const auto stamp = frame->metadata.source_qpc ? frame->metadata.source_qpc : frame->metadata.capture_qpc;
    if (const auto interval = active.capture_interval) {
        const auto tolerance = interval / 20;
        if (stamp + tolerance < active.next_capture) return;
        if (!active.next_capture || stamp - active.next_capture > interval || stamp <= active.next_capture + tolerance) {
            active.next_capture = stamp;
        }
        active.next_capture += interval;
    }
    if (!active.ShouldSubmit(*frame)) return;
    const auto captured_at = std::min(stamp, frame->metadata.capture_qpc);
    const auto timestamp_us = std::max(active.last_frame_timestamp_us + 1,
        webrtc::TimeMicros() - static_cast<std::int64_t>(QpcMilliseconds(QpcNow() - captured_at) * 1000));
    active.last_frame_timestamp_us = timestamp_us;

    const auto limits = LimitsFor(active.quality);
    const bool hardware = active.rtc->UsesHardwareEncoder();
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> pixels;
    if (hardware) {
        const auto [width, height] = ScaledSize(frame->metadata.width, frame->metadata.height, limits);
        if (auto texture = active.ConvertOnGpu(frame->texture.get(), 0, frame->metadata.width, frame->metadata.height,
                width, height, DXGI_FORMAT_NV12)) {
            pixels = webrtc::make_ref_counted<Nv12Frame>(std::move(texture), 0, width, height);
        }
    }
    impl_->stats.hardware_video = hardware;
    impl_->stats.gpu_conversion = !!pixels;
    if (!pixels) pixels = active.pixels->Convert(*frame, limits);
    const webrtc::ColorSpace color(webrtc::ColorSpace::PrimaryID::kBT709, webrtc::ColorSpace::TransferID::kBT709,
        webrtc::ColorSpace::MatrixID::kBT709, webrtc::ColorSpace::RangeID::kLimited);
    const auto video = webrtc::VideoFrame::Builder().set_video_frame_buffer(pixels)
        .set_timestamp_us(timestamp_us).set_color_space(color).build();
    if (active.dynamic_cadence) {
        active.last_video = video;
        active.last_video_quality = active.quality;
        active.last_submit_qpc = QpcNow();
    }
    active.quality_refresh_due = 0;
    if (trace) trace->Record("frame_submitted", frame->metadata.frame_id);
    active.rtc->Submit(video);

    auto& stats = impl_->stats;
    stats.width = static_cast<unsigned>(video.width());
    stats.height = static_cast<unsigned>(video.height());
    ++stats.submitted;
    impl_->capture_to_submit_total += QpcMilliseconds(QpcNow() - frame->metadata.capture_qpc);
    stats.capture_to_submit_ms = impl_->capture_to_submit_total / static_cast<double>(stats.submitted - active.idle_repeats);
}

void StreamSession::Present() {
    auto& active = *impl_->active;
    const auto frame = active.rtc->TakeFrame();
    const auto begin = QpcNow();
    active.renderer->ResizeToWindow();
    const auto buffer = frame ? frame->video.video_frame_buffer() : active.last_native;
    bool direct = false;
    if (buffer) {
        const auto limits = LimitsFor(impl_->connection.SupportsQualitySelection()
            ? StreamQuality::p1440 : StreamQuality::p720);
        const auto width = buffer->width();
        const auto height = buffer->height();
        if (width <= 0 || height <= 0 || width > limits.width || height > limits.height) {
            throw std::runtime_error("收到超出尺寸上限的视频帧");
        }
        const auto format = buffer->type();
        const bool hardware = format == webrtc::VideoFrameBuffer::Type::kNative;
        winrt::com_ptr<ID3D11Texture2D> texture;
        bool nv12_uploaded = false;
        if (hardware) {
            const auto* native = static_cast<const Nv12Frame*>(buffer.get());
            direct = active.WriteBackBuffer(*native);
            if (!direct) {
                texture = active.ConvertOnGpu(native->texture(), native->subresource(), width, height,
                    width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
            }
        } else if (format == webrtc::VideoFrameBuffer::Type::kNV12) {
            const auto* nv12 = buffer->GetNV12();
            nv12_uploaded = active.renderer->UploadNv12(nv12->DataY(), nv12->StrideY(), nv12->DataUV(),
                nv12->StrideUV(), width, height);
        }
        active.last_native = direct ? buffer : nullptr;
        if (texture) {
            active.renderer->UploadTexture(texture.get(), width, height);
        } else if (!direct && !nv12_uploaded) {
            const auto i420 = buffer->ToI420();
            active.bgra.resize(static_cast<std::size_t>(width) * height * 4);
            if (!i420 || libyuv::H420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
                    i420->StrideV(), active.bgra.data(), width * 4, width, height) != 0) {
                throw std::runtime_error("无法转换解码帧颜色格式");
            }
            active.renderer->UploadBgra(active.bgra.data(), width * 4, width, height);
        }
        impl_->stats.hardware_video = hardware;
        impl_->stats.gpu_conversion = direct || !!texture || nv12_uploaded;
        impl_->stats.gpu_backbuffer = direct;
        impl_->stats.width = static_cast<unsigned>(width);
        impl_->stats.height = static_cast<unsigned>(height);
    }
    if (!direct) active.renderer->Draw(impl_->requested_quality == StreamQuality::p720);
    const auto path = impl_->stats.ice_path;
    const bool minirtc_direct_udp = active.minirtc && impl_->stats.ice_protocol == "udp"
        && (path == IcePath::host || path == IcePath::nat);
    const bool immediate = impl_->connection.mode == ConnectionMode::lan_direct || minirtc_direct_udp
        || impl_->connection.test_present_immediate;
    const auto& trace = active.events->Trace();
    const auto present_call = frame && trace ? QpcNow() : 0;
    if (active.renderer->Present(immediate ? 0 : 1) && frame) {
        ++impl_->stats.presented;
        const auto present_return = QpcNow();
        if (trace) {
            const auto id = active.minirtc ? static_cast<std::uint64_t>(frame->queued_qpc) : frame->video.rtp_timestamp();
            trace->Record("frame_present_begin", id, 0, 0, 0, begin);
            trace->Record("frame_present_called", id, 0, 0, 0, present_call);
            trace->Record("frame_present_submitted", id, 0, 0, 0, present_return);
        }
        impl_->draw_to_present_total += QpcMilliseconds(present_return - begin);
        impl_->stats.draw_to_present_ms = impl_->draw_to_present_total / static_cast<double>(impl_->stats.presented);
        impl_->decoded_frame_wait_ticks += begin - frame->queued_qpc;
        impl_->stats.decoded_frame_wait_ms = QpcMilliseconds(impl_->decoded_frame_wait_ticks)
            / static_cast<double>(impl_->stats.presented);
    }
    active.redraw = false;
}

void StreamSession::Tick() {
    const auto now_ms = GetTickCount64();
    if (auto& retry = impl_->reconnect) {
        if (now_ms >= retry->deadline) {
            Stop(L"自动重连超时 · " + impl_->detail);
            return;
        }
        if (impl_->state == StreamState::reconnecting) {
            if (now_ms >= retry->next_attempt) {
                ++retry->attempts;
                Open(retry->monitor, retry->surface, {}, retry->frame_rate);
            }
            return;
        }
    }
    if (impl_->state == StreamState::negotiating) {
        const auto internet = impl_->connection.mode == ConnectionMode::internet;
        const auto timeout_seconds = internet ? 25 : 15;
        if (QpcNow() - impl_->active->negotiation_start > QpcFrequency() * timeout_seconds) {
            Recover(impl_->active->minirtc ? L"MiniRTC 建连超时" : L"WebRTC 建连超时");
        } else if (internet) {
            impl_->active->rtc->RequestStats(true);
        }
    } else if (impl_->state == StreamState::streaming) {
        auto& active = *impl_->active;
        if (active.transport_close_deadline) {
            if (now_ms >= active.transport_close_deadline) Recover(impl_->detail);
            return;
        }
        const auto now = QpcNow();
        if (active.quality_refresh_due && now >= active.quality_refresh_due) {
            active.quality_refresh_due = 0;
            // WGC may be quiet on a static desktop. A new capture session delivers its current image.
            try {
                impl_->StartWgc(active, !active.cursor_separate);
            } catch (const winrt::hresult_error&) {
                // Keep the existing capture. Its next frame will use the new quality.
            }
        }
        if (active.last_video && active.last_video_quality == active.quality
            && now - active.last_submit_qpc >= QpcFrequency() * 9 / 10) {
            auto video = *active.last_video;
            active.last_frame_timestamp_us = std::max(active.last_frame_timestamp_us + 1, webrtc::TimeMicros());
            video.set_timestamp_us(active.last_frame_timestamp_us);
            active.last_submit_qpc = now;
            ++active.idle_repeats;
            ++impl_->stats.submitted;
            if (const auto& trace = active.events->Trace()) trace->Record("frame_idle_repeat", active.idle_repeats);
            active.rtc->Submit(video);
        }
        UpdateVideoSettings();
        active.rtc->RequestStats();
    }
}

StreamState StreamSession::State() const noexcept { return impl_->state; }
std::wstring StreamSession::Status() const {
    if (const auto& retry = impl_->reconnect) {
        if (impl_->state == StreamState::reconnecting) {
            const auto now = GetTickCount64();
            const auto seconds = retry->next_attempt > now ? (retry->next_attempt - now + 999) / 1'000 : 0;
            return L"将在 " + std::to_wstring(seconds) + L" 秒后重连 · " + impl_->detail;
        }
        return L"自动重连第 " + std::to_wstring(retry->attempts) + L" 次 · " + impl_->detail;
    }
    return impl_->detail;
}
std::wstring StreamSession::Code() const { return impl_->code; }
StreamStatistics StreamSession::Statistics() const { return impl_->stats; }

void StreamSession::ExportCsv(const std::filesystem::path& path) const {
    std::ofstream csv(path, std::ios::binary);
    csv.imbue(std::locale::classic());
    csv << "seconds,width,height,captured,submitted,encoded,received,decoded,presented,"
           "capture_to_submit_mean_ms,encode_mean_ms,decode_mean_ms,draw_to_present_mean_ms,"
           "target_fps,packet_send_queue_mean_ms,jitter_buffer_mean_ms,decoded_frame_wait_mean_ms,"
           "rtc_frames_dropped,decoded_replaced,ice_path,ice_rtt_ms,ice_protocol,relay_protocol,"
           "video_packets_received,video_packets_lost,ice_local_srflx_peak,ice_remote_srflx_peak,"
           "ice_direct_udp_pairs_peak,ice_direct_udp_succeeded_peak,ice_direct_udp_requests_peak,"
           "ice_direct_udp_responses_peak,upload_bps,download_bps,video_loss_fraction,srtp_active,video_bitrate_limit_bps\r\n";
    csv << std::fixed << std::setprecision(3);
    const auto write_row = [&csv](const StreamStatistics& row) {
        csv << row.seconds << ',' << row.width << ',' << row.height << ',' << row.captured << ',' << row.submitted
            << ',' << row.encoded << ',' << row.received << ',' << row.decoded << ',' << row.presented << ','
            << row.capture_to_submit_ms << ',' << row.encode_ms << ',' << row.decode_ms << ','
            << row.draw_to_present_ms << ',';
        if (row.target_fps) csv << row.target_fps;
        csv << ',';
        if (row.packet_send_queue_ms) csv << *row.packet_send_queue_ms;
        csv << ',';
        if (row.jitter_buffer_ms) csv << *row.jitter_buffer_ms;
        csv << ',';
        if (row.decoded_frame_wait_ms) csv << *row.decoded_frame_wait_ms;
        csv << ',' << row.rtc_frames_dropped << ',' << row.decoded_replaced << ',' << IcePathName(row.ice_path) << ',';
        if (row.ice_rtt_ms) csv << *row.ice_rtt_ms;
        csv << ',' << row.ice_protocol << ',' << row.relay_protocol << ','
            << row.video_packets_received << ',' << row.video_packets_lost;
        if (const auto& ice = row.ice_diagnostics) {
            csv << ',' << ice->local_srflx << ',' << ice->remote_srflx << ',' << ice->direct_udp_pairs
                << ',' << ice->direct_udp_succeeded << ',' << ice->direct_udp_requests << ',' << ice->direct_udp_responses;
        } else {
            csv << ",,,,,,";
        }
        csv << ',';
        if (row.upload_bps) csv << *row.upload_bps;
        csv << ',';
        if (row.download_bps) csv << *row.download_bps;
        csv << ',';
        if (row.video_loss_fraction) csv << *row.video_loss_fraction;
        csv << ',';
        if (row.minirtc) csv << row.srtp_active;
        csv << ',';
        if (row.video_bitrate_limit_bps) csv << *row.video_bitrate_limit_bps;
        csv << "\r\n";
    };
    for (const auto& sample : impl_->samples) write_row(sample);
    if (impl_->samples.empty() && impl_->stats.ice_diagnostics) write_row(impl_->stats);
    csv.close();
    if (!csv) {
        throw std::runtime_error("无法写入串流统计 CSV");
    }
}

} // namespace p2premote
