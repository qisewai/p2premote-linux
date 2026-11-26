#include "dxgi_capturer.h"

#include "qpc.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

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
    DuplicationCapturer(HMONITOR monitor, ID3D11Device* device, bool capture_cursor)
        : monitor_(monitor),
          duplication_(DuplicateOutput(monitor, device)),
          capture_cursor_(capture_cursor) {
        device_.copy_from(device);
        device_->GetImmediateContext(context_.put());
        winrt::check_pointer(timer_.get());
        LARGE_INTEGER first_poll{};
        first_poll.QuadPart = -10'000;
        winrt::check_bool(SetWaitableTimer(timer_.get(), &first_poll, 1, nullptr, nullptr, FALSE));
    }

    HANDLE ReadyEvent() const noexcept override { return timer_.get(); }
    bool IsSourceClosed() const noexcept override { return failed_; }
    HRESULT SourceError() const noexcept override { return source_error_; }

    bool SetCursorCapture(bool enabled) noexcept override {
        if (capture_cursor_ == enabled) return true;
        capture_cursor_ = enabled;
        pending_.reset();
        refresh_desktop_ = !enabled && has_desktop_copy_;
        if (!enabled) {
            composed_ = nullptr;
            patch_staging_ = nullptr;
            pointer_drawn_ = false;
        }
        return true;
    }

    CaptureProgress Poll() override {
        pending_.reset();
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        // Avoid blocking the shared D3D device while the hardware encoder is active.
        const auto result = duplication_->AcquireNextFrame(0, &info, resource.put());
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            return !capture_cursor_ ? TakeCleanDesktopCopy() : CaptureProgress{};
        }
        if (FAILED(result)) {
            failed_ = true;
            source_error_ = result;
            return {};
        }
        FrameLease lease(duplication_);

        bool pointer_changed{};
        if (info.LastMouseUpdateTime.QuadPart) {
            const bool visible = info.PointerPosition.Visible != FALSE;
            if (visible != pointer_visible_
                || (visible && (info.PointerPosition.Position.x != pointer_pos_.x
                    || info.PointerPosition.Position.y != pointer_pos_.y))) {
                pointer_changed = true;
            }
            pointer_visible_ = visible;
            pointer_pos_ = info.PointerPosition.Position;
        }
        if (info.PointerShapeBufferSize > 0) {
            if (!UpdatePointerShape(info.PointerShapeBufferSize)) return {};
            pointer_changed = true;
        }

        const bool desktop_updated = info.LastPresentTime.QuadPart != 0;
        if (!capture_cursor_) {
            if (!desktop_updated) return TakeCleanDesktopCopy();
            refresh_desktop_ = false;
            desktop_copy_ = nullptr;
            desktop_desc_ = {};
            has_desktop_copy_ = false;
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

        winrt::com_ptr<ID3D11Texture2D> desktop_tex;
        if (desktop_updated) {
            desktop_tex = resource.as<ID3D11Texture2D>();
            D3D11_TEXTURE2D_DESC desc{};
            desktop_tex->GetDesc(&desc);
            if (!EnsureDesktopCopy(desc)) {
                // Fall back to direct uncomposited texture on allocation failure.
                CapturedFrame frame;
                frame.texture = desktop_tex;
                frame.metadata = {++frame_id_, QpcNow(), desc.Width, desc.Height};
                frame.metadata.source_qpc = info.LastPresentTime.QuadPart;
                frame.ready_qpc = frame.metadata.capture_qpc;
                frame.desktop_lease = std::make_shared<FrameLease>(std::move(lease));
                pending_ = std::move(frame);
                return {1, info.AccumulatedFrames ? info.AccumulatedFrames - 1 : 0, desc.Width, desc.Height};
            }
            context_->CopyResource(desktop_copy_.get(), desktop_tex.get());
            has_desktop_copy_ = true;
        } else if (!has_desktop_copy_ || !pointer_changed || (!pointer_visible_ && !pointer_drawn_)) {
            return {};
        }

        const auto source_qpc = desktop_updated ? info.LastPresentTime.QuadPart : info.LastMouseUpdateTime.QuadPart;
        winrt::com_ptr<ID3D11Texture2D> output = desktop_copy_;
        pointer_drawn_ = false;
        if (pointer_visible_ && !pointer_shape_.empty() && desktop_desc_.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            if (CompositePointer()) {
                output = composed_;
                pointer_drawn_ = true;
            }
        }

        CapturedFrame frame;
        frame.texture = std::move(output);
        frame.metadata = {++frame_id_, QpcNow(), desktop_desc_.Width, desktop_desc_.Height};
        frame.metadata.source_qpc = source_qpc;
        frame.ready_qpc = frame.metadata.capture_qpc;
        pending_ = std::move(frame);
        return {1, info.AccumulatedFrames ? info.AccumulatedFrames - 1 : 0, desktop_desc_.Width, desktop_desc_.Height};
    }

    std::optional<CapturedFrame> TakeFrame() noexcept override {
        return std::exchange(pending_, std::nullopt);
    }

private:
    CaptureProgress TakeCleanDesktopCopy() noexcept {
        if (!std::exchange(refresh_desktop_, false)) return {};
        CapturedFrame frame;
        frame.texture = std::move(desktop_copy_);
        frame.metadata = {++frame_id_, QpcNow(), desktop_desc_.Width, desktop_desc_.Height};
        frame.ready_qpc = frame.metadata.capture_qpc;
        pending_ = std::move(frame);
        has_desktop_copy_ = false;
        return {1, 0, desktop_desc_.Width, desktop_desc_.Height};
    }

    bool UpdatePointerShape(UINT buffer_size) {
        pointer_shape_.resize(buffer_size);
        UINT required{};
        auto hr = duplication_->GetFramePointerShape(buffer_size, pointer_shape_.data(), &required, &pointer_info_);
        if (hr == DXGI_ERROR_MORE_DATA && required > buffer_size) {
            pointer_shape_.resize(required);
            hr = duplication_->GetFramePointerShape(required, pointer_shape_.data(), &required, &pointer_info_);
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            failed_ = true;
            source_error_ = hr;
            return false;
        }
        if (FAILED(hr)) {
            pointer_shape_.clear();
            pointer_info_ = {};
        }
        return true;
    }

    bool EnsureDesktopCopy(const D3D11_TEXTURE2D_DESC& desc) noexcept {
        if (desktop_copy_ && desktop_desc_.Width == desc.Width && desktop_desc_.Height == desc.Height
                && desktop_desc_.Format == desc.Format) {
            return true;
        }
        desktop_copy_ = nullptr;
        composed_ = nullptr;
        has_desktop_copy_ = false;
        D3D11_TEXTURE2D_DESC copy_desc{};
        copy_desc.Width = desc.Width;
        copy_desc.Height = desc.Height;
        copy_desc.MipLevels = 1;
        copy_desc.ArraySize = 1;
        copy_desc.Format = desc.Format;
        copy_desc.SampleDesc = {1, 0};
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&copy_desc, nullptr, desktop_copy_.put()))) return false;
        if (FAILED(device_->CreateTexture2D(&copy_desc, nullptr, composed_.put()))) {
            desktop_copy_ = nullptr;
            return false;
        }
        desktop_desc_ = copy_desc;
        return true;
    }

    bool EnsurePatchStaging(UINT width, UINT height) noexcept {
        if (patch_staging_ && patch_width_ >= width && patch_height_ >= height) return true;
        patch_staging_ = nullptr;
        patch_width_ = std::max(width, 128u);
        patch_height_ = std::max(height, 128u);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = patch_width_;
        desc.Height = patch_height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, patch_staging_.put()))) {
            patch_width_ = patch_height_ = 0;
            return false;
        }
        return true;
    }

    bool CompositePointer() noexcept {
        if (!desktop_copy_ || !composed_ || !pointer_info_.Width || !pointer_info_.Height || !pointer_info_.Pitch) {
            return false;
        }
        const bool mono = pointer_info_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const bool color = pointer_info_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
        const bool masked = pointer_info_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
        if (!mono && !color && !masked) return false;
        if (mono && pointer_info_.Height < 2) return false;
        if (static_cast<std::size_t>(pointer_info_.Pitch) * pointer_info_.Height > pointer_shape_.size()) {
            return false;
        }

        const int shape_w = static_cast<int>(pointer_info_.Width);
        const int shape_h = mono ? static_cast<int>(pointer_info_.Height / 2) : static_cast<int>(pointer_info_.Height);
        const int x0 = std::max(0, static_cast<int>(pointer_pos_.x));
        const int y0 = std::max(0, static_cast<int>(pointer_pos_.y));
        const int x1 = std::min(static_cast<int>(desktop_desc_.Width), static_cast<int>(pointer_pos_.x) + shape_w);
        const int y1 = std::min(static_cast<int>(desktop_desc_.Height), static_cast<int>(pointer_pos_.y) + shape_h);
        if (x0 >= x1 || y0 >= y1) return false;

        const UINT src_x0 = static_cast<UINT>(x0 - static_cast<int>(pointer_pos_.x));
        const UINT src_y0 = static_cast<UINT>(y0 - static_cast<int>(pointer_pos_.y));
        const UINT cw = static_cast<UINT>(x1 - x0);
        const UINT ch = static_cast<UINT>(y1 - y0);
        if (mono ? ((src_x0 + cw + 7) / 8 > pointer_info_.Pitch) : ((src_x0 + cw) * 4 > pointer_info_.Pitch)) {
            return false;
        }
        if (!EnsurePatchStaging(cw, ch)) return false;

        context_->CopyResource(composed_.get(), desktop_copy_.get());
        const D3D11_BOX box{static_cast<UINT>(x0), static_cast<UINT>(y0), 0,
            static_cast<UINT>(x1), static_cast<UINT>(y1), 1};
        context_->CopySubresourceRegion(patch_staging_.get(), 0, 0, 0, 0, desktop_copy_.get(), 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(patch_staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        patch_buffer_.resize(static_cast<std::size_t>(cw) * ch * 4);
        const auto* mapped_bytes = static_cast<const std::uint8_t*>(mapped.pData);
        for (UINT row = 0; row < ch; ++row) {
            std::memcpy(patch_buffer_.data() + static_cast<std::size_t>(row) * cw * 4,
                mapped_bytes + static_cast<std::size_t>(row) * mapped.RowPitch, static_cast<std::size_t>(cw) * 4);
        }
        context_->Unmap(patch_staging_.get(), 0);

        for (UINT row = 0; row < ch; ++row) {
            auto* dst_row = reinterpret_cast<std::uint32_t*>(patch_buffer_.data() + static_cast<std::size_t>(row) * cw * 4);
            const UINT sy = src_y0 + row;
            if (mono) {
                const auto* and_row = pointer_shape_.data() + static_cast<std::size_t>(sy) * pointer_info_.Pitch;
                const auto* xor_row = pointer_shape_.data() + static_cast<std::size_t>(sy + shape_h) * pointer_info_.Pitch;
                for (UINT col = 0; col < cw; ++col) {
                    const UINT sx = src_x0 + col;
                    const std::uint8_t bit = static_cast<std::uint8_t>(0x80u >> (sx & 7u));
                    const bool and_set = (and_row[sx >> 3] & bit) != 0;
                    const bool xor_set = (xor_row[sx >> 3] & bit) != 0;
                    const std::uint32_t and_mask = and_set ? 0xFFFFFFFFu : 0xFF000000u;
                    const std::uint32_t xor_mask = xor_set ? 0x00FFFFFFu : 0x00000000u;
                    dst_row[col] = (dst_row[col] & and_mask) ^ xor_mask;
                }
            } else if (masked) {
                const auto* src_row = reinterpret_cast<const std::uint32_t*>(
                    pointer_shape_.data() + static_cast<std::size_t>(sy) * pointer_info_.Pitch);
                for (UINT col = 0; col < cw; ++col) {
                    const std::uint32_t sp = src_row[src_x0 + col];
                    if (sp & 0xFF000000u) {
                        dst_row[col] = (dst_row[col] ^ sp) | 0xFF000000u;
                    } else {
                        dst_row[col] = sp | 0xFF000000u;
                    }
                }
            } else {
                const auto* src_bytes = pointer_shape_.data() + static_cast<std::size_t>(sy) * pointer_info_.Pitch;
                auto* dst_bytes = reinterpret_cast<std::uint8_t*>(dst_row);
                for (UINT col = 0; col < cw; ++col) {
                    const auto* sp = src_bytes + static_cast<std::size_t>(src_x0 + col) * 4;
                    auto* dp = dst_bytes + static_cast<std::size_t>(col) * 4;
                    const unsigned alpha = sp[3];
                    if (alpha == 0) continue;
                    if (alpha == 255) {
                        dp[0] = sp[0];
                        dp[1] = sp[1];
                        dp[2] = sp[2];
                        dp[3] = 255;
                    } else {
                        const unsigned inv = 255u - alpha;
                        dp[0] = static_cast<std::uint8_t>((sp[0] * alpha + dp[0] * inv + 127u) / 255u);
                        dp[1] = static_cast<std::uint8_t>((sp[1] * alpha + dp[1] * inv + 127u) / 255u);
                        dp[2] = static_cast<std::uint8_t>((sp[2] * alpha + dp[2] * inv + 127u) / 255u);
                        dp[3] = 255;
                    }
                }
            }
        }

        context_->UpdateSubresource(composed_.get(), 0, &box, patch_buffer_.data(), cw * 4, 0);
        return true;
    }

    HMONITOR monitor_;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGIOutputDuplication> duplication_;
    winrt::handle timer_{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE)};
    winrt::com_ptr<ID3D11Texture2D> desktop_copy_;
    winrt::com_ptr<ID3D11Texture2D> composed_;
    winrt::com_ptr<ID3D11Texture2D> patch_staging_;
    D3D11_TEXTURE2D_DESC desktop_desc_{};
    UINT patch_width_{};
    UINT patch_height_{};
    std::vector<std::uint8_t> patch_buffer_;
    std::vector<std::uint8_t> pointer_shape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_info_{};
    POINT pointer_pos_{};
    bool pointer_visible_{};
    bool pointer_drawn_{};
    bool has_desktop_copy_{};
    bool capture_cursor_{true};
    bool refresh_desktop_{};
    std::optional<CapturedFrame> pending_;
    std::uint64_t frame_id_{};
    bool failed_{};
    HRESULT source_error_{S_OK};
};

} // namespace

std::unique_ptr<IVideoCapturer> StartDesktopDuplication(HMONITOR monitor, ID3D11Device* device, bool capture_cursor) {
    return std::make_unique<DuplicationCapturer>(monitor, device, capture_cursor);
}

} // namespace p2premote
