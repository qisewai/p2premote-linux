#include "wgc_capturer.h"

#include "qpc.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <utility>

namespace p2premote {
namespace {

using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

constexpr int pool_buffers = 2;
constexpr auto pixel_format = DirectXPixelFormat::B8G8R8A8UIntNormalized;

struct CaptureSignals {
    CaptureSignals() {
        if (!event) {
            winrt::throw_last_error();
        }
    }

    winrt::handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic_bool source_closed{};
};

class WgcCapturer final : public IVideoCapturer {
public:
    void Start(const GraphicsCaptureItem& item, ID3D11Device* device, bool capture_cursor) {
        item_ = item;
        winrt::com_ptr<IDXGIDevice> dxgi_device;
        winrt::check_hresult(device->QueryInterface(dxgi_device.put()));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
        winrt_device_ = inspectable.as<Direct3D11::IDirect3DDevice>();

        size_ = item_.Size();
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(winrt_device_, pixel_format, pool_buffers, size_);
        session_ = pool_.CreateCaptureSession(item_);
        cursor_toggle_ = ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
        if (cursor_toggle_) {
            session_.IsCursorCaptureEnabled(capture_cursor);
        }

        item_closed_ = item_.Closed(winrt::auto_revoke, [signals = signals_](const auto&, const auto&) noexcept {
            signals->source_closed.store(true);
            SetEvent(signals->event.get());
        });
        frame_arrived_ = pool_.FrameArrived(winrt::auto_revoke, [signals = signals_](const auto&, const auto&) noexcept {
            SetEvent(signals->event.get());
        });
        session_.StartCapture();
    }

    ~WgcCapturer() override {
        frame_arrived_.revoke();
        item_closed_.revoke();
        pending_.reset();
        try {
            if (session_) {
                session_.Close();
            }
        } catch (const winrt::hresult_error&) {
            // The capture source can already have closed.
        }
        try {
            if (pool_) {
                pool_.Close();
            }
        } catch (const winrt::hresult_error&) {
            // Leave the remaining resources to their RAII owners.
        }
    }

    HANDLE ReadyEvent() const noexcept override { return signals_->event.get(); }
    bool IsSourceClosed() const noexcept override { return signals_->source_closed.load(); }
    bool HasFrame() const noexcept override { return pending_.has_value(); }
    bool SetCursorCapture(bool enabled) noexcept override {
        if (!cursor_toggle_) return false;
        try {
            session_.IsCursorCaptureEnabled(enabled);
            return true;
        } catch (const winrt::hresult_error&) {
            return false;
        }
    }

    CaptureProgress Poll() override {
        CaptureProgress progress{};
        for (int index = 0; index < pool_buffers; ++index) {
            auto frame = pool_.TryGetNextFrame();
            if (!frame) {
                break;
            }
            const auto capture_qpc = QpcNow();
            ++progress.acquired;
            const auto frame_id = ++frame_id_;
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) {
                ++progress.dropped;
                continue;
            }
            if (size != size_) {
                // The changing frame still belongs to the old texture pool.
                ++progress.dropped;
                if (pending_) {
                    pending_.reset();
                    ++progress.dropped;
                }
                frame = nullptr;
                pool_.Recreate(winrt_device_, pixel_format, pool_buffers, size);
                size_ = size;
                break;
            }

            CapturedFrame next;
            next.metadata = {frame_id, capture_qpc,
                static_cast<unsigned>(size.Width), static_cast<unsigned>(size.Height)};
            next.metadata.source_qpc = ToQpc(frame.SystemRelativeTime().count());
            const auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), next.texture.put_void()));
            next.lease = std::move(frame);
            next.ready_qpc = QpcNow();
            if (pending_) {
                ++progress.dropped;
            }
            pending_ = std::move(next);
        }
        progress.width = static_cast<unsigned>(size_.Width);
        progress.height = static_cast<unsigned>(size_.Height);
        return progress;
    }

    std::optional<CapturedFrame> TakeFrame() noexcept override {
        return std::exchange(pending_, std::nullopt);
    }

private:
    std::shared_ptr<CaptureSignals> signals_ = std::make_shared<CaptureSignals>();
    GraphicsCaptureItem item_{nullptr};
    Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    bool cursor_toggle_{};
    GraphicsCaptureItem::Closed_revoker item_closed_;
    Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived_;
    SizeInt32 size_{};
    std::optional<CapturedFrame> pending_;
    std::uint64_t frame_id_{};
};

} // namespace

GraphicsCaptureItem CreateMonitorCaptureItem(HMONITOR monitor) {
    if (!GraphicsCaptureSession::IsSupported()) {
        throw winrt::hresult_error(E_NOTIMPL, L"当前桌面不支持 Windows Graphics Capture。");
    }
    const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

std::unique_ptr<IVideoCapturer> StartMonitorCapture(const GraphicsCaptureItem& item, ID3D11Device* device, bool capture_cursor) {
    auto capture = std::make_unique<WgcCapturer>();
    capture->Start(item, device, capture_cursor);
    return capture;
}

std::unique_ptr<IVideoCapturer> StartMonitorCapture(HMONITOR monitor, ID3D11Device* device) {
    return StartMonitorCapture(CreateMonitorCaptureItem(monitor), device);
}

} // namespace p2premote
