#include "dxgi_capturer.h"

#include "qpc.h"

#include <dxgi1_2.h>

#include <utility>

namespace p2premote {
namespace {

winrt::com_ptr<IDXGIOutputDuplication> DuplicateOutput(HMONITOR monitor, ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(dxgi.put()));
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
    for (UINT index = 0;; ++index) {
        winrt::com_ptr<IDXGIOutput> output;
        const auto found = adapter->EnumOutputs(index, output.put());
        if (found == DXGI_ERROR_NOT_FOUND) break;
        winrt::check_hresult(found);
        DXGI_OUTPUT_DESC desc{};
        winrt::check_hresult(output->GetDesc(&desc));
        if (desc.Monitor != monitor || !desc.AttachedToDesktop) continue;
        if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) break;
        winrt::com_ptr<IDXGIOutputDuplication> duplication;
        winrt::check_hresult(output.as<IDXGIOutput1>()->DuplicateOutput(device, duplication.put()));
        return duplication;
    }
    winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
}

struct FrameLease {
    explicit FrameLease(const winrt::com_ptr<IDXGIOutputDuplication>& value) : duplication(value) {}
    FrameLease(FrameLease&&) = default;
    ~FrameLease() { if (duplication) duplication->ReleaseFrame(); }
    winrt::com_ptr<IDXGIOutputDuplication> duplication;
};

class DuplicationCapturer final : public IVideoCapturer {
public:
    DuplicationCapturer(HMONITOR monitor, ID3D11Device* device)
        : monitor_(monitor), duplication_(DuplicateOutput(monitor, device)) {
        winrt::check_pointer(timer_.get());
        LARGE_INTEGER first_poll{};
        first_poll.QuadPart = -10'000;
        winrt::check_bool(SetWaitableTimer(timer_.get(), &first_poll, 1, nullptr, nullptr, FALSE));
    }

    HANDLE ReadyEvent() const noexcept override { return timer_.get(); }
    bool IsSourceClosed() const noexcept override { return failed_; }
    bool HasFrame() const noexcept override { return pending_.has_value(); }
    bool SetCursorCapture(bool enabled) noexcept override { return !enabled; }

    CaptureProgress Poll() override {
        pending_.reset();
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        // Avoid blocking the shared D3D device while the hardware encoder is active.
        const auto result = duplication_->AcquireNextFrame(0, &info, resource.put());
        if (result == DXGI_ERROR_WAIT_TIMEOUT) return {};
        if (FAILED(result)) {
            failed_ = true;
            return {};
        }
        FrameLease lease(duplication_);
        // The Viewer can draw the local pointer only when it is separable from the desktop.
        CURSORINFO cursor{sizeof(cursor)};
        if (info.LastMouseUpdateTime.QuadPart && !info.PointerPosition.Visible && GetCursorInfo(&cursor)
                && (cursor.flags & CURSOR_SHOWING) && MonitorFromPoint(cursor.ptScreenPos, MONITOR_DEFAULTTONULL) == monitor_) {
            failed_ = true;
            return {};
        }
        if (!info.LastPresentTime.QuadPart) return {};
        CapturedFrame frame;
        frame.texture = resource.as<ID3D11Texture2D>();
        D3D11_TEXTURE2D_DESC desc{};
        frame.texture->GetDesc(&desc);
        frame.metadata = {++frame_id_, QpcNow(), desc.Width, desc.Height};
        frame.metadata.source_qpc = info.LastPresentTime.QuadPart;
        frame.ready_qpc = frame.metadata.capture_qpc;
        frame.desktop_lease = std::make_shared<FrameLease>(std::move(lease));
        pending_ = std::move(frame);
        return {1, info.AccumulatedFrames ? info.AccumulatedFrames - 1 : 0, desc.Width, desc.Height};
    }

    std::optional<CapturedFrame> TakeFrame() noexcept override {
        return std::exchange(pending_, std::nullopt);
    }

private:
    HMONITOR monitor_;
    winrt::com_ptr<IDXGIOutputDuplication> duplication_;
    winrt::handle timer_{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE)};
    std::optional<CapturedFrame> pending_;
    std::uint64_t frame_id_{};
    bool failed_{};
};

} // namespace

std::unique_ptr<IVideoCapturer> StartDesktopDuplication(HMONITOR monitor, ID3D11Device* device) {
    return std::make_unique<DuplicationCapturer>(monitor, device);
}

} // namespace p2premote
