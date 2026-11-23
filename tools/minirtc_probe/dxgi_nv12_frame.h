#pragma once

#include "minirtc.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <atomic>
#include <cstring>
#include <utility>

namespace p2premote::native_minirtc {

class DxgiNv12Frame {
public:
    DxgiNv12Frame(ID3D11Texture2D* texture, UINT subresource, UINT width, UINT height,
        winrt::com_ptr<IUnknown> sample = {}) : sample_(std::move(sample)) {
        texture_.copy_from(texture);
        descriptor = {sizeof(descriptor), MiniRtcNativeVideoFrameD3d11Nv12, width, height,
            {}, this, &Retain, &Release, &Copy};
        descriptor.payload.d3d11_nv12 = {texture, subresource};
    }

    MiniRtcNativeVideoFrame descriptor{};

    static void Retain(void* owner) noexcept {
        static_cast<DxgiNv12Frame*>(owner)->refs_.fetch_add(1, std::memory_order_relaxed);
    }

    static void Release(void* owner) noexcept {
        auto* frame = static_cast<DxgiNv12Frame*>(owner);
        if (frame->refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete frame;
    }

    static int Copy(void* owner, uint8_t* data, size_t size) noexcept {
        auto& frame = *static_cast<DxgiNv12Frame*>(owner);
        const auto width = frame.descriptor.width;
        const auto height = frame.descriptor.height;
        if (!data || !width || !height || ((width | height) & 1u) ||
            size < static_cast<size_t>(width) * height * 3 / 2) return -1;
        D3D11_TEXTURE2D_DESC source{};
        frame.texture_->GetDesc(&source);
        if (source.Format != DXGI_FORMAT_NV12 || width > source.Width || height > source.Height ||
            source.MipLevels != 1 || frame.descriptor.payload.d3d11_nv12.subresource >= source.ArraySize) return -1;
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        frame.texture_->GetDevice(device.put());
        device->GetImmediateContext(context.put());
        const D3D11_TEXTURE2D_DESC desc{source.Width, source.Height, 1, 1, DXGI_FORMAT_NV12,
            {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        winrt::com_ptr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) return -1;
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, frame.texture_.get(),
            frame.descriptor.payload.d3d11_nv12.subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1;
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (UINT row = 0; row < height; ++row)
            std::memcpy(data + static_cast<size_t>(row) * width, src + static_cast<size_t>(row) * mapped.RowPitch, width);
        src += static_cast<size_t>(mapped.RowPitch) * source.Height;
        data += static_cast<size_t>(width) * height;
        for (UINT row = 0; row < height / 2; ++row)
            std::memcpy(data + static_cast<size_t>(row) * width, src + static_cast<size_t>(row) * mapped.RowPitch, width);
        context->Unmap(staging.get(), 0);
        return 0;
    }

private:
    std::atomic_uint refs_{1};
    winrt::com_ptr<IUnknown> sample_;
    winrt::com_ptr<ID3D11Texture2D> texture_;
};

} // namespace p2premote::native_minirtc
