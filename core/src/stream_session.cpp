#include <p2premote/stream_session.h>

#include "audio_engine.h"
#include "cursor_shape.h"
#include "d3d11_preview.h"
#include "d3d11_video.h"
#include "desktop_input.h"
#include "desktop_switch.h"
#include "dxgi_capturer.h"
#include "minirtc_call.h"
#include "plugin_host.h"
#include "qpc.h"
#include "signal_socket.h"
#include "stream_events.h"
#include "video_frame.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <utility>
#include <vector>

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
    return {1920, 1080};
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

std::uint8_t ClampByte(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void ConvertBgraToNv12Cpu(const std::uint8_t* src, int src_pitch, int src_w, int src_h,
    std::uint8_t* dst_y, std::uint8_t* dst_uv, int dst_w, int dst_h) noexcept {
    const auto map_x = [src_w, dst_w](int x) noexcept {
        return std::min(src_w - 1, static_cast<int>((static_cast<std::int64_t>(x) * src_w) / dst_w));
    };
    const auto map_y = [src_h, dst_h](int y) noexcept {
        return std::min(src_h - 1, static_cast<int>((static_cast<std::int64_t>(y) * src_h) / dst_h));
    };
    for (int y = 0; y < dst_h; ++y) {
        const auto* row = src + static_cast<std::size_t>(map_y(y)) * src_pitch;
        auto* out_y = dst_y + static_cast<std::size_t>(y) * dst_w;
        for (int x = 0; x < dst_w; ++x) {
            const auto* p = row + static_cast<std::size_t>(map_x(x)) * 4;
            const int b = p[0], g = p[1], r = p[2];
            out_y[x] = static_cast<std::uint8_t>(std::clamp(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16, 16, 235));
        }
    }
    for (int y = 0; y < dst_h; y += 2) {
        const auto* row0 = src + static_cast<std::size_t>(map_y(y)) * src_pitch;
        const auto* row1 = src + static_cast<std::size_t>(map_y(y + 1)) * src_pitch;
        auto* out_uv = dst_uv + static_cast<std::size_t>(y / 2) * dst_w;
        for (int x = 0; x < dst_w; x += 2) {
            const auto sx0 = static_cast<std::size_t>(map_x(x)) * 4;
            const auto sx1 = static_cast<std::size_t>(map_x(x + 1)) * 4;
            const int b = (row0[sx0] + row0[sx1] + row1[sx0] + row1[sx1] + 2) >> 2;
            const int g = (row0[sx0 + 1] + row0[sx1 + 1] + row1[sx0 + 1] + row1[sx1 + 1] + 2) >> 2;
            const int r = (row0[sx0 + 2] + row0[sx1 + 2] + row1[sx0 + 2] + row1[sx1 + 2] + 2) >> 2;
            out_uv[x] = static_cast<std::uint8_t>(std::clamp(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128, 16, 240));
            out_uv[x + 1] = static_cast<std::uint8_t>(std::clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128, 16, 240));
        }
    }
}

void ConvertNv12ToBgraCpu(const DecodedFrame& frame, std::vector<std::uint8_t>& bgra) {
    bgra.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    for (int y = 0; y < frame.height; ++y) {
        const auto* y_row = frame.y_plane + static_cast<std::size_t>(y) * frame.stride_y;
        const auto* uv_row = frame.uv_plane + static_cast<std::size_t>(y / 2) * frame.stride_uv;
        auto* dst = bgra.data() + static_cast<std::size_t>(y) * frame.width * 4;
        for (int x = 0; x < frame.width; ++x) {
            const int c = static_cast<int>(y_row[x]) - 16;
            const int d = static_cast<int>(uv_row[x & ~1]) - 128;
            const int e = static_cast<int>(uv_row[(x & ~1) + 1]) - 128;
            dst[x * 4] = ClampByte((298 * c + 541 * d + 128) >> 8);
            dst[x * 4 + 1] = ClampByte((298 * c - 55 * d - 136 * e + 128) >> 8);
            dst[x * 4 + 2] = ClampByte((298 * c + 459 * e + 128) >> 8);
            dst[x * 4 + 3] = 255;
        }
    }
}

class CapturePixels {
public:
    explicit CapturePixels(ID3D11Device* device) : device_(device) { device->GetImmediateContext(context_.put()); }
    ~CapturePixels() {
        staging_ = nullptr;
        ResetGpuNv12();
        context_->ClearState();
        context_->Flush();
    }

    void ResetGpuNv12() noexcept {
        gpu_nv12_ = nullptr;
        staging_nv12_ = nullptr;
        nv12_width_ = nv12_height_ = 0;
    }

    std::shared_ptr<Nv12Frame> ConvertNv12OnGpu(const CapturedFrame& frame,
        D3d11VideoConverter& converter, int output_width, int output_height) {
        if (nv12_width_ != output_width || nv12_height_ != output_height || !gpu_nv12_ || !staging_nv12_) {
            ResetGpuNv12();
            const D3D11_TEXTURE2D_DESC gpu_desc{static_cast<UINT>(output_width), static_cast<UINT>(output_height),
                1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
            winrt::check_hresult(device_->CreateTexture2D(&gpu_desc, nullptr, gpu_nv12_.put()));
            const D3D11_TEXTURE2D_DESC staging_desc{static_cast<UINT>(output_width), static_cast<UINT>(output_height),
                1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            winrt::check_hresult(device_->CreateTexture2D(&staging_desc, nullptr, staging_nv12_.put()));
            nv12_width_ = output_width;
            nv12_height_ = output_height;
        }
        const RECT destination{0, 0, output_width, output_height};
        converter.Blit(frame.texture.get(), 0, frame.metadata.width, frame.metadata.height, gpu_nv12_.get(), destination);
        context_->CopyResource(staging_nv12_.get(), gpu_nv12_.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging_nv12_.get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto output = Nv12Frame::Create(output_width, output_height);
        const auto row_pitch = static_cast<std::size_t>(mapped.RowPitch);
        const auto width = static_cast<std::size_t>(output_width);
        const auto height = static_cast<std::size_t>(output_height);
        const bool valid_pitch = row_pitch >= width;
        if (valid_pitch) {
            const auto* src_y = static_cast<const std::uint8_t*>(mapped.pData);
            const auto* src_uv = src_y + row_pitch * height;
            if (row_pitch == width) {
                std::memcpy(output->MutableDataY(), src_y, width * height * 3 / 2);
            } else {
                auto* dst_y = output->MutableDataY();
                for (std::size_t row = 0; row < height; ++row) {
                    std::memcpy(dst_y + row * width, src_y + row * row_pitch, width);
                }
                auto* dst_uv = output->MutableDataUV();
                for (std::size_t row = 0; row < height / 2; ++row) {
                    std::memcpy(dst_uv + row * width, src_uv + row * row_pitch, width);
                }
            }
        }
        context_->Unmap(staging_nv12_.get(), 0);
        return valid_pitch ? output : nullptr;
    }

    std::shared_ptr<Nv12Frame> ConvertNv12Cpu(const CapturedFrame& frame, int output_width, int output_height) {
        EnsureStaging(frame.metadata.width, frame.metadata.height);
        const D3D11_BOX content{0, 0, 0, frame.metadata.width, frame.metadata.height, 1};
        context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, frame.texture.get(), 0, &content);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto output = Nv12Frame::Create(output_width, output_height);
        ConvertBgraToNv12Cpu(static_cast<const std::uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
            static_cast<int>(frame.metadata.width), static_cast<int>(frame.metadata.height),
            output->MutableDataY(), output->MutableDataUV(), output_width, output_height);
        context_->Unmap(staging_.get(), 0);
        return output;
    }

private:
    void EnsureStaging(UINT width, UINT height) {
        if (staging_ && staging_width_ == width && staging_height_ == height) return;
        D3D11_TEXTURE2D_DESC spec{};
        spec.Width = width;
        spec.Height = height;
        spec.MipLevels = spec.ArraySize = spec.SampleDesc.Count = 1;
        spec.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        spec.Usage = D3D11_USAGE_STAGING;
        spec.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_ = nullptr;
        winrt::check_hresult(device_->CreateTexture2D(&spec, nullptr, staging_.put()));
        staging_width_ = width;
        staging_height_ = height;
    }

    ID3D11Device* device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    UINT staging_width_{};
    UINT staging_height_{};
    winrt::com_ptr<ID3D11Texture2D> gpu_nv12_;
    winrt::com_ptr<ID3D11Texture2D> staging_nv12_;
    int nv12_width_{};
    int nv12_height_{};
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
        if (audio_capture) audio_capture->Stop();
        audio_capture.reset();
        if (rtc) rtc->SetAudioReceiver({});
        if (audio_player) audio_player->Stop();
        audio_player.reset();
        events->SetDesktopInput(nullptr);
        DestroyDecodedCursor(remote_cursor);
    }

    std::shared_ptr<StreamEvents> events;
    std::unique_ptr<SignalSocket> signal;
    std::unique_ptr<MiniRtcCall> rtc;
    std::unique_ptr<AudioCapture> audio_capture;
    std::unique_ptr<AudioPlayer> audio_player;
    bool remote_speaker_enabled{true};
    bool remote_mic_enabled{false};
    bool remote_mic_fallback{true};
    std::unique_ptr<DesktopInput> desktop;
    winrt::com_ptr<ID3D11Device> device;
    std::unique_ptr<IVideoCapturer> capture;
    std::unique_ptr<CapturePixels> pixels;
    std::unique_ptr<D3d11FrameDifference> frame_difference;
    std::shared_ptr<Nv12Frame> last_video;
    StreamQuality last_video_quality{};
    std::int64_t last_submit_qpc{};
    std::int64_t last_change_qpc{};
    std::int64_t quality_refresh_due{};
    unsigned refinement_left{};
    std::uint64_t idle_repeats{};
    bool dynamic_cadence{};
    std::unique_ptr<D3d11VideoConverter> converter;
    bool gpu_conversion_failed{};
    bool gpu_texture_conversion_failed{};
    std::unique_ptr<D3d11Preview> renderer;
    std::vector<std::uint8_t> bgra;
    HMONITOR monitor{};
    HWND surface{};
    std::int64_t negotiation_start{};
    std::int64_t next_capture{};
    std::int64_t capture_interval{};
    bool dxgi_capture{};
    bool dxgi_attempted{};
    bool dxgi_fallback_wgc{};
    ULONGLONG capture_retry_due{};
    ULONGLONG last_desktop_check{};
    bool redraw{};
    bool host_registered{};
    ULONGLONG transport_close_deadline{};
    StreamQuality quality{StreamQuality::p1080};
    std::optional<VideoRequest> video_request;
    VideoBitratePreset plugin_bitrate{VideoBitratePreset::automatic};
    std::wstring plugin_notice;
    ULONGLONG plugin_notice_until{};
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
        if (!frame_difference || dxgi_capture) return true;
        const auto width = frame.metadata.width;
        const auto height = frame.metadata.height;
        std::optional<UINT> changed;
        try {
            changed = frame_difference->ChangedTiles(frame.texture.get(), width, height, desktop && desktop->DragRecentlyActive());
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
        if (desktop) desktop->Submit({InputKind::reset});
    }

    std::shared_ptr<Nv12Frame> ConvertToGpuNv12(const CapturedFrame& frame, int width, int height) {
        if (gpu_texture_conversion_failed) return {};
        try {
            if (!converter) converter = std::make_unique<D3d11VideoConverter>(device.get());
            auto nv12 = std::make_shared<Nv12Frame>();
            nv12->width = width;
            nv12->height = height;
            nv12->texture = converter->Convert(frame.texture.get(), 0, frame.metadata.width, frame.metadata.height,
                width, height, DXGI_FORMAT_NV12);
            return nv12;
        } catch (const winrt::hresult_error&) {
            gpu_texture_conversion_failed = true;
            return {};
        }
    }

    std::shared_ptr<Nv12Frame> ConvertOnGpuToCpuNv12(const CapturedFrame& frame, int width, int height) {
        if (gpu_conversion_failed || !pixels) return {};
        try {
            if (!converter) converter = std::make_unique<D3d11VideoConverter>(device.get());
            return pixels->ConvertNv12OnGpu(frame, *converter, width, height);
        } catch (const winrt::hresult_error&) {
            converter.reset();
            pixels->ResetGpuNv12();
            gpu_conversion_failed = true;
            return {};
        }
    }
};

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

    Impl(bool is_host, ConnectionOptions options) : host(is_host), connection(std::move(options)) {
        if (connection.signal_url.empty()) connection.signal_url = L"ws://127.0.0.1:8787/signal";
    }

    ~Impl() {
        if (attached_desktop) {
            CloseDesktop(attached_desktop);
            attached_desktop = nullptr;
        }
    }

    bool PreferDxgi(const ActiveStream& stream) const noexcept {
        return connection.desktop_duplication && !stream.dxgi_fallback_wgc;
    }

    bool TryStartDxgi(ActiveStream& stream, bool capture_cursor) noexcept {
        stream.dxgi_attempted = true;
        if (stream.dxgi_capture) {
            // DXGI allows only one duplication per output in this process.
            stream.capture.reset();
            stream.dxgi_capture = false;
        }
        try {
            stream.capture = StartDesktopDuplication(stream.monitor, stream.device.get(), capture_cursor);
            stream.dxgi_capture = true;
            stream.next_capture = 0;
            stream.capture_retry_due = 0;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool StartBestCapture(ActiveStream& stream, bool capture_cursor, bool prefer_dxgi) noexcept {
        SyncThreadToInputDesktop(attached_desktop, desktop_name);
        const bool secure = IsSecureDesktopName(desktop_name);
        if (prefer_dxgi || secure) {
            if (TryStartDxgi(stream, capture_cursor)) return true;
        }
        if (!secure) {
            try {
                StartWgc(stream, capture_cursor);
                stream.capture_retry_due = 0;
                return true;
            } catch (...) {
                capture_item = nullptr;
                capture_monitor = nullptr;
            }
        }
        if (!prefer_dxgi && !secure && TryStartDxgi(stream, capture_cursor)) return true;
        stream.capture.reset();
        stream.dxgi_capture = false;
        stream.capture_retry_due = GetTickCount64() + 200;
        return false;
    }

    void StartWgc(ActiveStream& stream, bool capture_cursor) {
        if (!stream.frame_difference) stream.StartFrameComparison();
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
            stream.quality_refresh_due = QpcNow() + QpcFrequency();
        }
        detail = stream.SharingStatus();
        return true;
    }

    bool host;
    ConnectionOptions connection;
    bool drawable{true};
    StreamState state{StreamState::idle};
    std::wstring detail;
    HMONITOR capture_monitor{};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item{nullptr};
    HDESK attached_desktop{};
    std::wstring desktop_name;
    std::unique_ptr<ActiveStream> active;
    std::optional<Reconnect> reconnect;
    StreamStatistics stats;
    StreamQuality requested_quality{StreamQuality::p1080};
    VideoBitratePreset requested_bitrate{VideoBitratePreset::automatic};
    StreamAudioOptions audio_options{};
    std::int64_t start{};
    double capture_to_submit_total{};
    double draw_to_present_total{};
    std::int64_t decoded_frame_wait_ticks{};
    std::deque<StreamStatistics> samples;
    std::deque<PluginUiCommand> pending_ui_commands;
};

StreamSession::StreamSession(bool host, ConnectionOptions connection)
    : impl_(std::make_unique<Impl>(host, std::move(connection))) {}
StreamSession::~StreamSession() = default;

void StreamSession::Start(HMONITOR monitor, HWND surface, StreamFrameRate frame_rate) {
    if (impl_->active) return;
    impl_->reconnect.reset();
    Open(monitor, surface, frame_rate);
}

void StreamSession::Open(HMONITOR monitor, HWND surface, StreamFrameRate frame_rate) {
    const auto& connection = impl_->connection;
    if (!MiniRtcCall::Available(connection)) {
        throw std::runtime_error("MiniRTC 模块不可用或包含不支持的 turns: 地址");
    }
    auto trace = connection.latency_trace_dir.empty() ? nullptr
        : std::make_shared<LatencyTrace>(connection.latency_trace_dir, impl_->host);
    auto active = std::make_unique<ActiveStream>(std::move(trace));
    active->monitor = monitor;
    active->surface = surface;
    active->dynamic_cadence = impl_->host && connection.DynamicCadenceEnabled();
    const auto target_fps = static_cast<unsigned>(frame_rate);
    active->capture_interval = impl_->host && NeedsCaptureRateLimit(monitor, target_fps) ? QpcFrequency() / target_fps : 0;
    const auto target = !impl_->host ? winrt::to_string(connection.remote_device) : std::string{};
    active->signal = std::make_unique<SignalSocket>(active->events, connection.signal_url,
        impl_->host ? "listen_internet" : "join_internet", target, connection.access_token);
    impl_->samples.clear();
    impl_->stats = {};
    impl_->stats.target_fps = impl_->host ? target_fps : 0;
    impl_->capture_to_submit_total = impl_->draw_to_present_total = 0;
    impl_->decoded_frame_wait_ticks = 0;
    impl_->active = std::move(active);
    impl_->detail = L"正在连接信令服务 · " + connection.signal_url;
    impl_->state = StreamState::waiting;
}

void StreamSession::Approve(StreamInputMode input) {
    if (!impl_->host || impl_->state != StreamState::approval) return;
    auto& active = *impl_->active;
    active.input_mode = input;
    if (input == StreamInputMode::desktop) active.desktop = std::make_unique<DesktopInput>(active.monitor, active.events->Trace().get());
    active.negotiation_start = QpcNow();
    impl_->state = StreamState::negotiating;
    impl_->detail = L"已授权，正在建立视频连接";
    const bool allow_input = input == StreamInputMode::desktop;
    active.device = CreateD3d11Device(true);
    active.rtc = std::make_unique<MiniRtcCall>(true, active.events, impl_->connection, allow_input,
        impl_->stats.target_fps, active.device.get());
    active.signal->Send("accept", allow_input ? "control" : "view");
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

void StreamSession::SetAudioOptions(StreamAudioOptions options) {
    options.speaker_volume = std::clamp(options.speaker_volume, 0.0f, 1.0f);
    impl_->audio_options = options;
    ApplyAudioOptions();
}

StreamAudioOptions StreamSession::AudioOptions() const noexcept {
    return impl_->audio_options;
}

void StreamSession::ApplyAudioOptions() {
    if (impl_->host || impl_->state != StreamState::streaming || !impl_->active) return;
    auto& active = *impl_->active;
    if (!active.rtc || active.transport_close_deadline) return;
    const auto& opt = impl_->audio_options;

    if (active.audio_player) {
        active.audio_player->SetMuted(!opt.speaker_enabled);
        active.audio_player->SetVolume(opt.speaker_volume);
    }
    if (opt.mic_enabled) {
        if (!active.audio_capture) {
            active.audio_capture = std::make_unique<AudioCapture>(AudioCapture::Mode::microphone,
                [rtc = active.rtc.get()](const char* data, std::size_t size) {
                    rtc->SendAudio(data, size, "mic_audio");
                });
        }
        active.audio_capture->SetPaused(false);
        active.rtc->SetAudioSendPaused(false);
    } else {
        if (active.audio_capture) {
            active.audio_capture->Stop();
            active.audio_capture.reset();
        }
        active.rtc->SetAudioSendPaused(true);
    }

    std::uint16_t flags{};
    if (opt.speaker_enabled) flags |= audio_flag_speaker;
    if (opt.mic_enabled) flags |= audio_flag_mic;
    if (opt.mic_speaker_fallback) flags |= audio_flag_mic_fallback;
    const InputPacket packet{InputKind::set_audio_settings, flags, 0, 0,
        ++active.outgoing_input_sequence, GetTickCount64()};
    active.rtc->SendAudioSettings(packet);
}

void StreamSession::UpdateVideoSettings() {
    if (impl_->host || impl_->state != StreamState::streaming) return;
    auto& active = *impl_->active;
    if (active.transport_close_deadline) return;
    auto& stats = impl_->stats;
    const auto now = GetTickCount64();
    const auto resolution = impl_->requested_quality;
    const auto preset = impl_->requested_bitrate == VideoBitratePreset::automatic ? active.plugin_bitrate : impl_->requested_bitrate;

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
    if (active.rtc->SendVideoSettings(request)) {
        active.video_request = ActiveStream::VideoRequest{request, now + video_settings_timeout_ms};
    }
}

void StreamSession::Stop(std::wstring status) {
    impl_->reconnect.reset();
    Close(std::move(status), true);
}

void StreamSession::Close(std::wstring status, bool intentional) {
    if (auto active = std::move(impl_->active)) {
        if (active->audio_capture) active->audio_capture->Stop();
        active->audio_capture.reset();
        if (active->rtc) {
            active->rtc->SetAudioReceiver({});
            active->rtc->SetPluginsConnected(false);
            if (!impl_->host) {
                if (auto* plugins = active->rtc->Plugins()) {
                    plugins->Stop();
                    for (auto& command : plugins->TakeUiCommands()) {
                        impl_->pending_ui_commands.push_back(std::move(command));
                    }
                }
            }
        }
        if (active->audio_player) active->audio_player->Stop();
        active->audio_player.reset();
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
}

void StreamSession::Recover(std::wstring status) {
    auto& retry = impl_->reconnect;
    const auto& active = *impl_->active;
    if (!retry && impl_->state != StreamState::streaming && !active.host_registered) {
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
    if (impl_->active) impl_->active->redraw = true;
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
        if (active.rtc->HasFrame() || (active.redraw && active.renderer->HasImage())) {
            return active.renderer->PresentEvent();
        }
    }
    return nullptr;
}

bool StreamSession::CanPresent() const noexcept {
    return impl_->active && impl_->active->renderer && impl_->active->renderer->HasImage();
}

void StreamSession::ProcessEvents() {
    bool poll_cursor{};
    for (auto& event : impl_->active->events->Drain()) {
        auto& active = *impl_->active;
        switch (event.type) {
        case StreamEventType::signal_lost:
        case StreamEventType::media_lost:
            if (impl_->state == StreamState::streaming) {
                if (!active.transport_close_deadline) {
                    active.transport_close_deadline = GetTickCount64() + close_grace_ms;
                    if (active.rtc) active.rtc->SetPluginsConnected(false);
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
                Start(monitor, nullptr, frame_rate);
                return;
            }
            if (event.name == "error" || event.name == "ended") {
                Stop(winrt::to_hstring(event.payload).c_str());
                return;
            }
            if (impl_->host && impl_->state == StreamState::waiting && event.name == "ready") {
                active.host_registered = true;
                impl_->reconnect.reset();
                impl_->detail = L"等待同账号设备连接";
                if (impl_->connection.auto_control) impl_->detail += L" · 自动允许整桌面控制";
            } else if (impl_->host && impl_->state == StreamState::waiting && event.name == "request") {
                impl_->state = StreamState::approval;
                if (impl_->connection.auto_control) Approve(StreamInputMode::desktop);
                else impl_->detail = L"收到同账号设备连接请求，请确认授权";
            } else if (!impl_->host && event.name == "waiting" && impl_->state == StreamState::waiting) {
                impl_->detail = L"等待 Host 确认";
            } else if (!impl_->host && event.name == "accepted" && impl_->state == StreamState::waiting) {
                impl_->state = StreamState::negotiating;
                impl_->detail = L"Host 已允许，正在建立视频连接";
                active.negotiation_start = QpcNow();
                active.device = CreateD3d11Device(true);
                active.rtc = std::make_unique<MiniRtcCall>(false, active.events, impl_->connection,
                    event.payload == "control", 60, active.device.get());
            } else if (active.rtc && event.name == "minirtc" &&
                (impl_->state == StreamState::negotiating || impl_->state == StreamState::streaming)) {
                active.rtc->ReceiveSignal(event.payload);
            } else {
                Stop(L"收到与当前会话状态不符的信令");
                return;
            }
            break;
        case StreamEventType::minirtc_signal:
            if (MiniRtcCall::IsSessionSignal(event.payload)) {
                active.signal->Send("minirtc", std::move(event.payload));
            }
            break;
        case StreamEventType::connected:
            if (impl_->state == StreamState::negotiating) {
                if (impl_->host) {
                    if (!active.device) active.device = CreateD3d11Device(true);
                    active.pixels = std::make_unique<CapturePixels>(active.device.get());
                    impl_->ApplyVideoSettings(active, active.quality, VideoBitrateLimit(VideoBitratePreset::automatic));
                    if (!impl_->StartBestCapture(active, true, impl_->PreferDxgi(active))) {
                        DisplayChanged();
                    }
                } else {
                    active.renderer = std::make_unique<D3d11Preview>(active.surface, active.device);
                }
                impl_->start = QpcNow();
                impl_->state = StreamState::streaming;
                impl_->reconnect.reset();
                if (active.input_ready) active.events->SetDesktopInput(active.desktop.get());
                impl_->detail = impl_->host ? active.SharingStatus() : L"正在接收视频";
                if (impl_->host) {
                    active.audio_capture = std::make_unique<AudioCapture>(AudioCapture::Mode::desktop_loopback,
                        [rtc = active.rtc.get()](const char* data, std::size_t size) {
                            rtc->SendAudio(data, size, "desktop_audio");
                        });
                    active.audio_capture->SetPaused(!active.remote_speaker_enabled);
                } else {
                    active.audio_player = std::make_unique<AudioPlayer>(AudioPlayer::Target::default_speaker);
                    active.rtc->SetAudioReceiver([player = active.audio_player.get()](
                        std::string_view source, const char* data, std::size_t size) {
                        if (source.empty() || source == "desktop_audio") player->PushPcm(data, size);
                    });
                    ApplyAudioOptions();
                }
                active.rtc->SetPluginsConnected(true);
            }
            break;
        case StreamEventType::stats: {
            const auto& rtc = event.counters;
            auto& stats = impl_->stats;
            stats.received = rtc.received;
            stats.decoded = rtc.decoded;
            stats.video_packets_received = rtc.video_packets_received;
            stats.upload_bps = rtc.upload_bps;
            stats.download_bps = rtc.download_bps;
            stats.video_loss_fraction = rtc.video_loss_fraction;
            stats.srtp_active = rtc.srtp_active;
            if (!rtc.video_codec.empty()) {
                stats.video_codec = winrt::to_hstring(rtc.video_codec);
            }
            if (rtc.sent_packets) {
                stats.packet_send_queue_ms = rtc.packet_send_queue_seconds * 1000 / static_cast<double>(rtc.sent_packets);
            }
            stats.ice_path = rtc.ice_path;
            stats.ice_protocol = rtc.ice_protocol;
            stats.ice_rtt_ms = rtc.ice_rtt_ms;
            if (const auto* call = active.rtc.get(); call && call->Plugins()) {
                call->Plugins()->UpdateMetrics({stats.ice_path, stats.ice_rtt_ms, stats.video_loss_fraction, stats.download_bps});
            }
            if (impl_->state == StreamState::streaming) {
                if (!impl_->host) stats.decoded_replaced = active.rtc->ReplacedFrames();
                stats.seconds = QpcMilliseconds(QpcNow() - impl_->start) / 1000;
                impl_->samples.push_back(stats);
                if (impl_->samples.size() > 3600) impl_->samples.pop_front();
            }
            break;
        }
        case StreamEventType::input_ready:
            active.input_ready = active.rtc && active.rtc->InputReady();
            active.events->SetDesktopInput(InputReady() ? active.desktop.get() : nullptr);
            if (!active.input_ready) {
                active.ResetInput();
                if (impl_->host && active.cursor_separate && active.capture) {
                    if (!active.capture->SetCursorCapture(true)) {
                        if (impl_->StartBestCapture(active, true, impl_->PreferDxgi(active))) {
                            impl_->detail = active.SharingStatus();
                        }
                    }
                }
                active.cursor_requested = active.cursor_separate = active.cursor_sent = false;
                DestroyDecodedCursor(std::exchange(active.remote_cursor, nullptr));
            }
            break;
        case StreamEventType::cursor:
            if (!impl_->host && active.cursor_requested) {
                DestroyDecodedCursor(std::exchange(active.remote_cursor, DecodeCursor(event.payload)));
                active.cursor_visible = event.payload.size() != 1;
                active.cursor_separate = true;
            }
            break;
        case StreamEventType::video_settings: {
            if (impl_->state != StreamState::streaming || active.transport_close_deadline) break;
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
        case StreamEventType::audio_settings: {
            if (!impl_->host || impl_->state != StreamState::streaming || active.transport_close_deadline) break;
            const auto flags = event.input.code;
            active.remote_speaker_enabled = (flags & audio_flag_speaker) != 0;
            const bool want_mic = (flags & audio_flag_mic) != 0 && active.input_mode == StreamInputMode::desktop;
            const bool want_fallback = (flags & audio_flag_mic_fallback) != 0;
            if (active.audio_capture) active.audio_capture->SetPaused(!active.remote_speaker_enabled);
            if (active.rtc) active.rtc->SetAudioSendPaused(!active.remote_speaker_enabled);
            if (want_mic) {
                if (!active.audio_player || active.remote_mic_fallback != want_fallback) {
                    active.rtc->SetAudioReceiver({});
                    active.audio_player.reset();
                    active.audio_player = std::make_unique<AudioPlayer>(
                        AudioPlayer::Target::virtual_mic_or_speaker, want_fallback);
                    active.rtc->SetAudioReceiver([player = active.audio_player.get()](
                        std::string_view source, const char* data, std::size_t size) {
                        if (source.empty() || source == "mic_audio") player->PushPcm(data, size);
                    });
                }
            } else {
                if (active.rtc) active.rtc->SetAudioReceiver({});
                active.audio_player.reset();
            }
            active.remote_mic_enabled = want_mic;
            active.remote_mic_fallback = want_fallback;
            break;
        }
        case StreamEventType::input:
            if (impl_->host && active.input_mode == StreamInputMode::desktop && InputReady()
                    && event.input.kind == InputKind::cursor_ready) {
                active.cursor_requested = true;
                poll_cursor = true;
            }
            break;
        }
    }
    if (poll_cursor) PollCursor();
    if (impl_->active && impl_->active->rtc) impl_->active->rtc->ProbeWorkspace();
    ProcessPluginActions();
    UpdateVideoSettings();
}

bool StreamSession::HasPluginMenu() const {
    if (impl_->host || !InputReady()) return false;
    const auto* plugins = impl_->active->rtc->Plugins();
    return plugins && plugins->HasMenu();
}

std::vector<PluginMenuItem> StreamSession::PluginMenu() const {
    if (impl_->host || !InputReady()) return {};
    const auto* plugins = impl_->active->rtc->Plugins();
    return plugins ? plugins->Menu() : std::vector<PluginMenuItem>{};
}

void StreamSession::InvokePluginMenu(unsigned id) {
    if (impl_->host || !InputReady()) return;
    if (auto* plugins = impl_->active->rtc->Plugins()) plugins->Invoke(id);
}

std::vector<PluginUiCommand> StreamSession::TakePluginUiCommands() {
    if (!impl_->host && impl_->active && impl_->active->rtc) {
        if (auto* plugins = impl_->active->rtc->Plugins()) {
            for (auto& command : plugins->TakeUiCommands()) {
                impl_->pending_ui_commands.push_back(std::move(command));
            }
        }
    }
    std::vector<PluginUiCommand> result;
    result.reserve(impl_->pending_ui_commands.size());
    while (!impl_->pending_ui_commands.empty()) {
        result.push_back(std::move(impl_->pending_ui_commands.front()));
        impl_->pending_ui_commands.pop_front();
    }
    return result;
}

void StreamSession::SendPluginUiEvent(PluginUiEvent event) {
    if (impl_->host || !InputReady()) return;
    if (auto* plugins = impl_->active->rtc->Plugins()) plugins->SendUiEvent(std::move(event));
}

void StreamSession::UpdatePluginLockState(bool caps, bool num) {
    if (impl_->host || !InputReady()) return;
    if (auto* plugins = impl_->active->rtc->Plugins()) plugins->UpdateLockState(caps, num);
}

bool StreamSession::PluginCapturesLockKeys() const noexcept {
    if (impl_->host || !InputReady()) return false;
    const auto* plugins = impl_->active->rtc->Plugins();
    return plugins && plugins->CapturesLockKeys();
}

std::wstring_view StreamSession::PluginNotice() const noexcept {
    const auto* active = impl_->active.get();
    if (active && GetTickCount64() < active->plugin_notice_until) return active->plugin_notice;
    return {};
}

void StreamSession::ProcessPluginActions() {
    if (impl_->host || !InputReady()) return;
    auto& active = *impl_->active;
    auto* plugins = active.rtc->Plugins();
    if (!plugins) return;
    for (auto& action : plugins->TakeActions()) {
        switch (action.kind) {
        case PluginAction::Kind::notice:
            active.plugin_notice = std::move(action.notice);
            active.plugin_notice_until = GetTickCount64() + 4'000;
            break;
        case PluginAction::Kind::input:
            action.input.sent_ms = GetTickCount64();
            if (!active.rtc->SendInputCommand(action.input)) {
                active.plugin_notice = L"未能发送快捷输入，请确认对端支持办公插件";
                active.plugin_notice_until = GetTickCount64() + 4'000;
            }
            break;
        case PluginAction::Kind::bitrate:
            if (active.plugin_bitrate != action.bitrate) {
                active.plugin_bitrate = action.bitrate;
                if (impl_->requested_bitrate == VideoBitratePreset::automatic) active.rejected_video_settings.reset();
            }
            break;
        }
    }
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
    if (!InputReady()) return;
    auto& active = *impl_->active;
    if (!impl_->host) {
        if (!impl_->connection.video_cursor && !active.cursor_requested) {
            active.cursor_requested = SubmitInput({InputKind::cursor_ready});
        }
        return;
    }
    if (!active.cursor_requested) return;
    if (!active.dxgi_capture && impl_->PreferDxgi(active) && !active.dxgi_attempted) {
        if (impl_->TryStartDxgi(active, false)) {
            active.cursor_separate = true;
            impl_->detail = active.SharingStatus();
        }
    }
    if (!active.cursor_separate) {
        active.cursor_separate = active.capture && active.capture->SetCursorCapture(false);
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
    if (impl_->host || !InputReady()) return false;
    auto& active = *impl_->active;
    input.sequence = ++active.outgoing_input_sequence;
    input.sent_ms = GetTickCount64();
    if (const auto& trace = active.events->Trace()) trace->Pointer("input_generated", input);
    return active.rtc->SendInput(input);
}

void StreamSession::Capture() {
    auto& active = *impl_->active;
    if (!active.capture) return;
    if (active.capture->IsSourceClosed()) {
        if (active.dxgi_capture && active.capture->PreferFallbackCapture()) {
            active.dxgi_fallback_wgc = true;
        }
        if (impl_->StartBestCapture(active, !active.cursor_separate, impl_->PreferDxgi(active))) {
            impl_->detail = active.SharingStatus();
        } else {
            impl_->detail = L"正在切换桌面采集源…";
        }
        return;
    }
    if (active.dxgi_capture && active.capture_interval && active.next_capture) {
        const auto tolerance = active.capture_interval / 8;
        if (QpcNow() + tolerance < active.next_capture) return;
    }
    impl_->stats.captured += active.capture->Poll().acquired;
    const auto frame = active.capture->TakeFrame();
    if (!frame) {
        if (active.dxgi_capture && active.dynamic_cadence && active.last_video
            && active.last_video_quality == active.quality && active.refinement_left > 0) {
            const auto now = QpcNow();
            if (now - active.last_submit_qpc >= QpcFrequency() / 15) {
                active.last_submit_qpc = now;
                --active.refinement_left;
                ++active.idle_repeats;
                ++impl_->stats.submitted;
                if (const auto& trace = active.events->Trace()) trace->Record("frame_idle_repeat", active.idle_repeats);
                active.rtc->Submit(*active.last_video);
            }
        }
        return;
    }
    const auto& trace = active.events->Trace();
    if (trace) {
        if (frame->metadata.source_qpc) {
            trace->Record("frame_source", frame->metadata.frame_id, 0, 0, 0, frame->metadata.source_qpc);
        }
        trace->Record("frame_captured", frame->metadata.frame_id, 0, 0, 0, frame->metadata.capture_qpc);
    }
    const auto stamp = (!active.dxgi_capture && frame->metadata.source_qpc)
        ? frame->metadata.source_qpc : frame->metadata.capture_qpc;
    if (const auto interval = active.capture_interval) {
        const auto tolerance = interval / 8;
        if (stamp + tolerance < active.next_capture) return;
        if (!active.next_capture || stamp - active.next_capture > interval || stamp <= active.next_capture + tolerance) {
            active.next_capture = stamp;
        }
        active.next_capture += interval;
    }
    if (!active.ShouldSubmit(*frame)) return;

    const auto limits = LimitsFor(active.quality);
    const auto [width, height] = ScaledSize(static_cast<int>(frame->metadata.width), static_cast<int>(frame->metadata.height), limits);
    auto nv12 = active.ConvertToGpuNv12(*frame, width, height);
    if (!nv12) nv12 = active.ConvertOnGpuToCpuNv12(*frame, width, height);
    const bool gpu_converted = static_cast<bool>(nv12);
    if (!nv12) nv12 = active.pixels->ConvertNv12Cpu(*frame, width, height);
    impl_->stats.gpu_conversion = gpu_converted;
    impl_->stats.gpu_native = nv12->texture != nullptr;
    if (active.dynamic_cadence) {
        active.last_video = nv12;
        active.last_video_quality = active.quality;
        active.last_submit_qpc = QpcNow();
        if (active.dxgi_capture) active.refinement_left = 4;
    }
    active.quality_refresh_due = 0;
    if (trace) trace->Record("frame_submitted", frame->metadata.frame_id);
    active.rtc->Submit(*nv12);

    auto& stats = impl_->stats;
    stats.width = static_cast<unsigned>(width);
    stats.height = static_cast<unsigned>(height);
    ++stats.submitted;
    impl_->capture_to_submit_total += QpcMilliseconds(QpcNow() - frame->metadata.capture_qpc);
    stats.capture_to_submit_ms = impl_->capture_to_submit_total / static_cast<double>(stats.submitted - active.idle_repeats);
}

void StreamSession::Present() {
    auto& active = *impl_->active;
    auto frame = active.rtc->TakeFrame();
    const auto begin = QpcNow();
    active.renderer->ResizeToWindow();
    if (frame) {
        const auto limits = LimitsFor(StreamQuality::p1440);
        if (frame->width <= 0 || frame->height <= 0 || frame->width > limits.width || frame->height > limits.height) {
            throw std::runtime_error("收到超出尺寸上限的视频帧");
        }
        const bool native_uploaded = frame->texture && active.renderer->UploadNv12Texture(frame->texture,
            frame->subresource, static_cast<unsigned>(frame->width), static_cast<unsigned>(frame->height));
        std::shared_ptr<Nv12Frame> readback;
        if (frame->texture && !native_uploaded) {
            readback = Nv12Frame::Create(frame->width, frame->height);
            if (frame->copy_to_nv12(frame->owner.get(), readback->data.data(), readback->data.size()) != 0) return;
            frame->y_plane = readback->DataY();
            frame->uv_plane = readback->DataUV();
            frame->stride_y = readback->stride_y;
            frame->stride_uv = readback->stride_uv;
        }
        const bool nv12_uploaded = native_uploaded || active.renderer->UploadNv12(frame->y_plane,
            static_cast<unsigned>(frame->stride_y), frame->uv_plane, static_cast<unsigned>(frame->stride_uv),
            static_cast<unsigned>(frame->width), static_cast<unsigned>(frame->height));
        if (!nv12_uploaded) {
            ConvertNv12ToBgraCpu(*frame, active.bgra);
            active.renderer->UploadBgra(active.bgra.data(), static_cast<unsigned>(frame->width * 4),
                static_cast<unsigned>(frame->width), static_cast<unsigned>(frame->height));
        }
        impl_->stats.gpu_conversion = nv12_uploaded;
        impl_->stats.gpu_native = native_uploaded;
        impl_->stats.width = static_cast<unsigned>(frame->width);
        impl_->stats.height = static_cast<unsigned>(frame->height);
    }
    active.renderer->Draw(impl_->requested_quality == StreamQuality::p720);
    const auto path = impl_->stats.ice_path;
    const bool direct_udp = impl_->stats.ice_protocol == "udp" && (path == IcePath::host || path == IcePath::nat);
    const bool immediate = direct_udp || impl_->connection.test_present_immediate;
    const auto& trace = active.events->Trace();
    const auto present_call = frame && trace ? QpcNow() : 0;
    if (active.renderer->Present(immediate ? 0 : 1) && frame) {
        ++impl_->stats.presented;
        const auto present_return = QpcNow();
        if (trace) {
            const auto id = static_cast<std::uint64_t>(frame->queued_qpc);
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
                Open(retry->monitor, retry->surface, retry->frame_rate);
            }
            return;
        }
    }
    if (impl_->state == StreamState::negotiating) {
        if (QpcNow() - impl_->active->negotiation_start > QpcFrequency() * 25) {
            Recover(L"MiniRTC 建连超时");
        }
    } else if (impl_->state == StreamState::streaming) {
        auto& active = *impl_->active;
        if (active.transport_close_deadline) {
            if (now_ms >= active.transport_close_deadline) Recover(impl_->detail);
            return;
        }
        const auto now = QpcNow();
        if (impl_->host && now_ms - active.last_desktop_check >= 250) {
            active.last_desktop_check = now_ms;
            if (SyncThreadToInputDesktop(impl_->attached_desktop, impl_->desktop_name)) {
                impl_->capture_item = nullptr;
                impl_->capture_monitor = nullptr;
                active.dxgi_fallback_wgc = false;
                active.dxgi_attempted = false;
                if (impl_->StartBestCapture(active, !active.cursor_separate, impl_->PreferDxgi(active))) {
                    impl_->detail = active.SharingStatus();
                } else {
                    impl_->detail = L"正在切换桌面采集源…";
                }
            }
        }
        if (impl_->host && !active.capture && active.capture_retry_due && now_ms >= active.capture_retry_due) {
            if (impl_->StartBestCapture(active, !active.cursor_separate, impl_->PreferDxgi(active))) {
                impl_->detail = active.SharingStatus();
            }
        }
        if (active.quality_refresh_due && now >= active.quality_refresh_due) {
            active.quality_refresh_due = 0;
            if (impl_->StartBestCapture(active, !active.cursor_separate, impl_->PreferDxgi(active))) {
                impl_->detail = active.SharingStatus();
            }
        }
        if (active.last_video && active.last_video_quality == active.quality
            && now - active.last_submit_qpc >= QpcFrequency() * 9 / 10) {
            active.last_submit_qpc = now;
            ++active.idle_repeats;
            ++impl_->stats.submitted;
            if (const auto& trace = active.events->Trace()) trace->Record("frame_idle_repeat", active.idle_repeats);
            active.rtc->Submit(*active.last_video);
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

StreamStatistics StreamSession::Statistics() const { return impl_->stats; }

void StreamSession::VideoDimensions(unsigned& width, unsigned& height) const noexcept {
    width = impl_->stats.width;
    height = impl_->stats.height;
}

void StreamSession::ExportCsv(const std::filesystem::path& path) const {
    std::ofstream csv(path, std::ios::binary);
    csv.imbue(std::locale::classic());
    csv << "seconds,width,height,captured,submitted,received,decoded,presented,"
           "capture_to_submit_mean_ms,draw_to_present_mean_ms,target_fps,"
           "packet_send_queue_mean_ms,decoded_frame_wait_mean_ms,decoded_replaced,"
           "ice_path,ice_rtt_ms,ice_protocol,video_packets_received,"
           "upload_bps,download_bps,video_loss_fraction,srtp_active,video_bitrate_limit_bps,gpu_native\r\n";
    csv << std::fixed << std::setprecision(3);
    const auto write_row = [&csv](const StreamStatistics& row) {
        csv << row.seconds << ',' << row.width << ',' << row.height << ',' << row.captured << ',' << row.submitted
            << ',' << row.received << ',' << row.decoded << ',' << row.presented << ','
            << row.capture_to_submit_ms << ',' << row.draw_to_present_ms << ',';
        if (row.target_fps) csv << row.target_fps;
        csv << ',';
        if (row.packet_send_queue_ms) csv << *row.packet_send_queue_ms;
        csv << ',';
        if (row.decoded_frame_wait_ms) csv << *row.decoded_frame_wait_ms;
        csv << ',' << row.decoded_replaced << ',' << IcePathName(row.ice_path) << ',';
        if (row.ice_rtt_ms) csv << *row.ice_rtt_ms;
        csv << ',' << row.ice_protocol << ',' << row.video_packets_received << ',';
        if (row.upload_bps) csv << *row.upload_bps;
        csv << ',';
        if (row.download_bps) csv << *row.download_bps;
        csv << ',';
        if (row.video_loss_fraction) csv << *row.video_loss_fraction;
        csv << ',' << row.srtp_active << ',';
        if (row.video_bitrate_limit_bps) csv << *row.video_bitrate_limit_bps;
        csv << ',' << row.gpu_native << "\r\n";
    };
    for (const auto& row : impl_->samples) write_row(row);
    if (impl_->samples.empty() && impl_->stats.seconds > 0) write_row(impl_->stats);
    if (!csv) throw std::runtime_error("无法写入串流统计 CSV");
}

} // namespace p2premote
