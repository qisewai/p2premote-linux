#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace p2premote {

enum class PixelFormat { bgra8 };
enum class ColorSpace { sdr_rgb_full };

struct FrameMetadata {
    std::uint64_t frame_id{};
    std::int64_t capture_qpc{};
    unsigned width{};
    unsigned height{};
    PixelFormat pixel_format{PixelFormat::bgra8};
    ColorSpace color_space{ColorSpace::sdr_rgb_full};
    std::int64_t source_qpc{};
};

struct Percentiles {
    double p50_ms{};
    double p95_ms{};
};

struct PreviewStatistics {
    double elapsed_seconds{};
    std::uint64_t captured{};
    std::uint64_t presented{};
    std::uint64_t dropped{};
    unsigned queue_depth{};
    unsigned max_queue_depth{};
    unsigned source_width{};
    unsigned source_height{};
    Percentiles capture_to_ready;
    Percentiles ready_to_draw;
    Percentiles draw_to_submit;
    Percentiles present_call;
    Percentiles capture_to_present_return;
};

// All methods are called from the thread that owns the preview HWND.
class PreviewSession {
public:
    PreviewSession();
    ~PreviewSession();

    void Start(HMONITOR monitor, HWND preview_window);
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;

    // Include non-null handles in the HWND thread's message wait.
    [[nodiscard]] HANDLE CaptureEvent() const noexcept;
    [[nodiscard]] HANDLE PresentEvent() const noexcept;
    // Call when CaptureEvent signals. Returns false when Windows closes the source.
    bool ProcessCapture();
    // Call when PresentEvent signals.
    void Present();
    void OnPreviewResize(bool drawable) noexcept;

    [[nodiscard]] PreviewStatistics Statistics() const;
    void ExportCsv(const std::filesystem::path& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
