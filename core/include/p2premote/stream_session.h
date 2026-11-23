#pragma once

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace p2premote {

enum class StreamState { idle, waiting, approval, negotiating, streaming, reconnecting, stopped };
enum class StreamFrameRate { fps30 = 30, fps60 = 60 };
enum class StreamInputMode { view_only, desktop };

struct StreamStatistics {
    std::uint64_t captured{};
    std::uint64_t submitted{};
    std::uint64_t received{};
    std::uint64_t decoded{};
    std::uint64_t presented{};
    std::uint64_t decoded_replaced{};
    std::uint64_t video_packets_received{};
    std::wstring video_codec;
    bool gpu_conversion{};
    bool gpu_native{};
    unsigned width{};
    unsigned height{};
    unsigned target_fps{};
    double seconds{};
    double capture_to_submit_ms{};
    double draw_to_present_ms{};
    std::optional<double> packet_send_queue_ms;
    std::optional<double> decoded_frame_wait_ms;
    IcePath ice_path{};
    std::string ice_protocol;
    std::optional<double> ice_rtt_ms;
    std::optional<std::uint32_t> upload_bps;
    std::optional<std::uint32_t> download_bps;
    std::optional<double> video_loss_fraction;
    bool srtp_active{};
    std::optional<int> video_bitrate_limit_bps;
};

// The caller owns the window and pumps the three wait handles on that thread.
class StreamSession {
public:
    explicit StreamSession(bool host, ConnectionOptions connection = {});
    ~StreamSession();

    void Start(HMONITOR monitor, HWND surface, StreamFrameRate frame_rate = StreamFrameRate::fps30);
    void Approve(StreamInputMode input = StreamInputMode::view_only);
    void SetQuality(StreamQuality quality);
    void SetVideoBitratePreset(VideoBitratePreset preset);
    void Stop(std::wstring status);
    void Resize(bool drawable) noexcept;
    void DisplayChanged() noexcept;
    void Tick();
    void PollCursor();
    bool LocalCursor(HCURSOR& cursor) const noexcept;

    HANDLE Event() const noexcept;
    HANDLE CaptureEvent() const noexcept;
    HANDLE PresentEvent() const;
    void ProcessEvents();
    void Capture();
    void Present();

    bool InputReady() const noexcept;
    bool DesktopControl() const noexcept;
    bool SubmitInput(InputPacket input);

    StreamState State() const noexcept;
    std::wstring Status() const;
    StreamStatistics Statistics() const;
    void ExportCsv(const std::filesystem::path& path) const;

private:
    void Open(HMONITOR monitor, HWND surface, StreamFrameRate frame_rate);
    void Close(std::wstring status, bool intentional);
    void Recover(std::wstring status);
    void UpdateVideoSettings();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
