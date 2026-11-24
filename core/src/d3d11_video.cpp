#include "d3d11_video.h"
#include "qpc.h"

#include <d3dcompiler.h>
#include <utility>

namespace p2premote {
namespace {

constexpr char compare_shader[] = R"(
Texture2D<float4> current : register(t0);
Texture2D<float4> previous : register(t1);
RWBuffer<uint> result : register(u0);
groupshared uint tile_changed;

[numthreads(16, 16, 1)]
void main(uint3 thread : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index == 0) tile_changed = 0;
    GroupMemoryBarrierWithGroupSync();
    uint width, height;
    current.GetDimensions(width, height);
    uint2 pixel = thread.xy * 2;
    bool changed = false;
    [unroll]
    for (uint y = 0; y < 2; ++y) {
        [unroll]
        for (uint x = 0; x < 2; ++x) {
            uint2 p = pixel + uint2(x, y);
            if (p.x < width && p.y < height) {
                changed = changed || any(current.Load(int3(p, 0)).rgb != previous.Load(int3(p, 0)).rgb);
            }
        }
    }
    if (changed) InterlockedOr(tile_changed, 1);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && tile_changed != 0) InterlockedAdd(result[0], 1);
}
)";

} // namespace

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

D3d11FrameDifference::D3d11FrameDifference(ID3D11Device* device) {
    device_.copy_from(device);
    device->GetImmediateContext(context_.put());
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::check_hresult(D3DCompile(compare_shader, sizeof(compare_shader) - 1, nullptr, nullptr, nullptr,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.put(), nullptr));
    winrt::check_hresult(device_->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
        nullptr, shader_.put()));

    const D3D11_BUFFER_DESC output{sizeof(UINT), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
    winrt::check_hresult(device_->CreateBuffer(&output, nullptr, result_.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_UINT;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = 1;
    winrt::check_hresult(device_->CreateUnorderedAccessView(result_.get(), &view, result_view_.put()));
    const D3D11_BUFFER_DESC staging{sizeof(UINT), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    winrt::check_hresult(device_->CreateBuffer(&staging, nullptr, readback_.put()));
}

void D3d11FrameDifference::Resize(UINT width, UINT height) {
    const D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
        D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    for (auto* slot : {&current_, &previous_}) {
        *slot = {};
        winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, slot->texture.put()));
        winrt::check_hresult(device_->CreateShaderResourceView(slot->texture.get(), nullptr, slot->view.put()));
    }
    width_ = width;
    height_ = height;
    has_previous_ = false;
}

UINT D3d11FrameDifference::TileCount(UINT width, UINT height) noexcept {
    return ((width + 31) / 32) * ((height + 31) / 32);
}

std::optional<UINT> D3d11FrameDifference::ChangedTiles(ID3D11Texture2D* source, UINT width, UINT height, bool active_drag) {
    // During an active drag, avoid a GPU readback on the path to the encoder.
    // The next comparison must start from the first frame after the drag.
    if (active_drag) {
        has_previous_ = false;
        return TileCount(width, height);
    }
    const auto deadline = QpcNow() + QpcFrequency() * 2 / 1000;
    if (width_ != width || height_ != height) Resize(width, height);
    const D3D11_BOX content{0, 0, 0, width, height, 1};
    context_->CopySubresourceRegion(current_.texture.get(), 0, 0, 0, 0, source, 0, &content);
    std::swap(current_, previous_);
    if (!std::exchange(has_previous_, true)) return TileCount(width, height);

    const UINT zero[4]{};
    context_->ClearUnorderedAccessViewUint(result_view_.get(), zero);
    ID3D11ShaderResourceView* inputs[]{previous_.view.get(), current_.view.get()};
    ID3D11UnorderedAccessView* output = result_view_.get();
    context_->CSSetShader(shader_.get(), nullptr, 0);
    context_->CSSetShaderResources(0, 2, inputs);
    context_->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context_->Dispatch((width + 31) / 32, (height + 31) / 32, 1);
    ID3D11ShaderResourceView* no_inputs[2]{};
    output = nullptr;
    context_->CSSetShaderResources(0, 2, no_inputs);
    context_->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);
    context_->CopyResource(readback_.get(), result_.get());
    context_->Flush();

    // Send the current frame if a busy GPU cannot answer in time.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    auto status = context_->Map(readback_.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    while (status == DXGI_ERROR_WAS_STILL_DRAWING && QpcNow() < deadline) {
        YieldProcessor();
        status = context_->Map(readback_.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    }
    if (status == DXGI_ERROR_WAS_STILL_DRAWING) return std::nullopt;
    winrt::check_hresult(status);
    const auto changed = *static_cast<const UINT*>(mapped.pData);
    context_->Unmap(readback_.get(), 0);
    return changed;
}

} // namespace p2premote
