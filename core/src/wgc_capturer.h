#pragma once

#include <d3d11.h>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace p2premote {

struct FrameMetadata {
    std::uint64_t frame_id{};
    std::int64_t capture_qpc{};
    unsigned width{};
    unsigned height{};
    std::int64_t source_qpc{};
};

struct CapturedFrame {
    FrameMetadata metadata;
    std::int64_t ready_qpc{};
    // Keep the captured texture valid until its GPU work is queued.
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame lease{nullptr};
    std::shared_ptr<void> desktop_lease;
    winrt::com_ptr<ID3D11Texture2D> texture;
};

struct CaptureProgress {
    unsigned acquired{};
    unsigned dropped{};
    unsigned width{};
    unsigned height{};
};

class IVideoCapturer {
public:
    virtual ~IVideoCapturer() = default;
    [[nodiscard]] virtual HANDLE ReadyEvent() const noexcept = 0;
    [[nodiscard]] virtual bool IsSourceClosed() const noexcept = 0;
    [[nodiscard]] virtual bool PreferFallbackCapture() const noexcept { return false; }
    [[nodiscard]] virtual HRESULT SourceError() const noexcept { return S_OK; }
    [[nodiscard]] virtual bool CanRestoreDxgi() const noexcept { return false; }
    [[nodiscard]] virtual bool HasFrame() const noexcept = 0;
    virtual bool SetCursorCapture(bool enabled) noexcept = 0;
    virtual CaptureProgress Poll() = 0;
    virtual std::optional<CapturedFrame> TakeFrame() noexcept = 0;
};

std::unique_ptr<IVideoCapturer> StartMonitorCapture(HMONITOR monitor, ID3D11Device* device);
winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateMonitorCaptureItem(HMONITOR monitor);
std::unique_ptr<IVideoCapturer> StartMonitorCapture(
    const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item, ID3D11Device* device, bool capture_cursor = true);

} // namespace p2premote
