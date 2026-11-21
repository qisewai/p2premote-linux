#include "d3d11_video.h"

#include "api/video/i420_buffer.h"
#include "libyuv/convert.h"

namespace p2premote {

Nv12Frame::Nv12Frame(winrt::com_ptr<ID3D11Texture2D> texture, UINT subresource, int width, int height,
    winrt::com_ptr<IUnknown> lease)
    : lease_(std::move(lease)), texture_(std::move(texture)), subresource_(subresource), width_(width), height_(height) {}

webrtc::scoped_refptr<webrtc::I420BufferInterface> Nv12Frame::ToI420() {
    winrt::com_ptr<ID3D11Device> device;
    texture_->GetDevice(device.put());
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    D3D11_TEXTURE2D_DESC desc{};
    texture_->GetDesc(&desc);
    desc.ArraySize = desc.MipLevels = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, readback.put()))) return nullptr;
    context->CopySubresourceRegion(readback.get(), 0, 0, 0, 0, texture_.get(), subresource_, nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &map))) return nullptr;
    const auto* y = static_cast<const uint8_t*>(map.pData);
    const auto* uv = y + static_cast<std::size_t>(map.RowPitch) * desc.Height;
    auto output = webrtc::I420Buffer::Create(width_, height_);
    const auto pitch = static_cast<int>(map.RowPitch);
    const auto converted = libyuv::NV12ToI420(y, pitch, uv, pitch, output->MutableDataY(), output->StrideY(),
        output->MutableDataU(), output->StrideU(), output->MutableDataV(), output->StrideV(), width_, height_);
    context->Unmap(readback.get(), 0);
    if (converted) return nullptr;
    return output;
}

D3d11VideoConverter::D3d11VideoConverter(ID3D11Device* device) {
    device_.copy_from(device);
    video_device_ = device_.as<ID3D11VideoDevice>();
    winrt::com_ptr<ID3D11DeviceContext> context;
    device_->GetImmediateContext(context.put());
    video_context_ = context.as<ID3D11VideoContext1>();
}

winrt::com_ptr<ID3D11Texture2D> D3d11VideoConverter::Convert(ID3D11Texture2D* source, UINT subresource,
    UINT source_width, UINT source_height, UINT width, UINT height, DXGI_FORMAT format) {
    const D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET, 0, 0};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, texture.put()));
    const RECT destination{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    Blit(source, subresource, source_width, source_height, texture.get(), destination);
    return texture;
}

void D3d11VideoConverter::Blit(ID3D11Texture2D* source, UINT subresource, UINT source_width, UINT source_height,
    ID3D11Texture2D* target, const RECT& destination) {
    D3D11_TEXTURE2D_DESC target_desc{};
    target->GetDesc(&target_desc);
    const auto width = target_desc.Width;
    const auto height = target_desc.Height;
    if (!processor_ || source_width_ != source_width || source_height_ != source_height || width_ != width || height_ != height) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = desc.OutputFrameRate = {60, 1};
        desc.InputWidth = source_width;
        desc.InputHeight = source_height;
        desc.OutputWidth = width;
        desc.OutputHeight = height;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        processor_ = nullptr;
        enumerator_ = nullptr;
        winrt::check_hresult(video_device_->CreateVideoProcessorEnumerator(&desc, enumerator_.put()));
        winrt::check_hresult(video_device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()));
        source_width_ = source_width;
        source_height_ = source_height;
        width_ = width;
        height_ = height;
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = subresource % source_desc.MipLevels;
    input_desc.Texture2D.ArraySlice = subresource / source_desc.MipLevels;
    winrt::com_ptr<ID3D11VideoProcessorInputView> input;
    winrt::check_hresult(video_device_->CreateVideoProcessorInputView(source, enumerator_.get(), &input_desc, input.put()));

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> output;
    winrt::check_hresult(video_device_->CreateVideoProcessorOutputView(target, enumerator_.get(), &output_desc, output.put()));

    const RECT input_rect{0, 0, static_cast<LONG>(source_width), static_cast<LONG>(source_height)};
    const RECT output_rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    video_context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &input_rect);
    video_context_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &destination);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.get(), TRUE, &output_rect);
    constexpr auto rgb = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    constexpr auto yuv = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    video_context_->VideoProcessorSetStreamColorSpace1(processor_.get(), 0, source_desc.Format == DXGI_FORMAT_NV12 ? yuv : rgb);
    video_context_->VideoProcessorSetOutputColorSpace1(processor_.get(), target_desc.Format == DXGI_FORMAT_NV12 ? yuv : rgb);
    if (target_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.get(), FALSE, &background);
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.get();
    winrt::check_hresult(video_context_->VideoProcessorBlt(processor_.get(), output.get(), 0, 1, &stream));
}

} // namespace p2premote
