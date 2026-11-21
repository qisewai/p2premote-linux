#pragma once

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace p2premote {

enum class StreamState { idle, waiting, approval, negotiating, streaming, stopped };
enum class StreamFrameRate { fps30 = 30, fps60 = 60 };
enum class StreamInputMode { view_only, test_pad, desktop };

struct StreamStatistics {
    std::uint64_t captured{};
    std::uint64_t submitted{};
    std::uint64_t encoded{};
    std::uint64_t received{};
    std::uint64_t decoded{};
    std::uint64_t presented{};
    std::uint64_t decoded_replaced{};
    std::uint64_t rtc_frames_dropped{};
    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::wstring video_codec;
    bool hardware_video{};
    bool gpu_conversion{};
    bool gpu_backbuffer{};
    unsigned width{};
    unsigned height{};
    unsigned target_fps{};
    double seconds{};
    double capture_to_submit_ms{};
    double encode_ms{};
    double decode_ms{};
    double draw_to_present_ms{};
    std::optional<double> packet_send_queue_ms;
    std::optional<double> jitter_buffer_ms;
    std::optional<double> decoded_frame_wait_ms;
    IcePath ice_path{};
    std::optional<double> ice_rtt_ms;
};

// The caller owns the window and pumps the three wait handles on that thread.
class StreamSession {
public:
    explicit StreamSession(bool host, ConnectionOptions connection = {});
    ~StreamSession();

    void Start(HMONITOR monitor, HWND surface, const std::wstring& code,
        StreamFrameRate frame_rate = StreamFrameRate::fps30);
    void Approve(StreamInputMode input = StreamInputMode::view_only);
    void SetQuality(StreamQuality quality) noexcept;
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
    std::vector<InputPacket> TakeInput();

    StreamState State() const noexcept;
    std::wstring Status() const;
    std::wstring Code() const;
    StreamStatistics Statistics() const;
    void ExportCsv(const std::filesystem::path& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
