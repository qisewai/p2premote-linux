#include "d3d11_preview.h"

#include <p2premote/video_viewport.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <utility>

namespace p2premote {
namespace {

constexpr auto format = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT swap_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

constexpr char preview_shader[] = R"hlsl(
Texture2D image : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState linear_clamp : register(s0);

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vs_main(uint id : SV_VertexID) {
    Vertex vertex;
    vertex.uv = float2((id << 1) & 2, id & 2);
    vertex.position = float4(vertex.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return vertex;
}

float4 ps_main(Vertex vertex) : SV_Target {
    return float4(image.Sample(linear_clamp, vertex.uv).rgb, 1);
}

float4 Nv12Color(float luma, float2 chroma_value) {
    // Match the limited-range BT.709 coefficients used by the libyuv fallback.
    float y = (luma * 255.0 - 16.0) / 219.0;
    float2 uv = (chroma_value - 128.0 / 255.0) / 64.0;
    float3 rgb = y + float3(115.0 * uv.y, dot(uv, float2(-14.0, -34.0)), 128.0 * uv.x);
    return float4(saturate(rgb), 1);
}

float4 ps_nv12(Vertex vertex) : SV_Target {
    return Nv12Color(image.Sample(linear_clamp, vertex.uv).r, chroma.Sample(linear_clamp, vertex.uv));
}

float EnhanceLuma(float center, float4 neighbors) {
    float detail = center - (neighbors.x + neighbors.y + neighbors.z + neighbors.w) * 0.25;
    float boost = min(0.07, 0.65 * max(0.0, abs(detail) - 2.0 / 255.0));
    // Bound the result by the local range to avoid bright or dark outlines.
    float low = min(center, min(min(neighbors.x, neighbors.y), min(neighbors.z, neighbors.w)));
    float high = max(center, max(max(neighbors.x, neighbors.y), max(neighbors.z, neighbors.w)));
    return clamp(center + sign(detail) * boost, low, high);
}

float4 ps_nv12_text(Vertex vertex) : SV_Target {
    float center = image.Sample(linear_clamp, vertex.uv).r;
    float4 neighbors = image.GatherRed(linear_clamp, vertex.uv);
    return Nv12Color(EnhanceLuma(center, neighbors), chroma.Sample(linear_clamp, vertex.uv));
}

float Luma(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float4 ps_main_text(Vertex vertex) : SV_Target {
    float3 rgb = image.Sample(linear_clamp, vertex.uv).rgb;
    float center = Luma(rgb);
    float4 neighbors = float4(Luma(image.Sample(linear_clamp, vertex.uv, int2(-1, 0)).rgb),
        Luma(image.Sample(linear_clamp, vertex.uv, int2(1, 0)).rgb),
        Luma(image.Sample(linear_clamp, vertex.uv, int2(0, -1)).rgb),
        Luma(image.Sample(linear_clamp, vertex.uv, int2(0, 1)).rgb));
    return float4(saturate(rgb + (EnhanceLuma(center, neighbors) - center)), 1);
}
)hlsl";

winrt::com_ptr<ID3DBlob> CompileShader(const char* entry, const char* profile) {
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::check_hresult(D3DCompile(preview_shader, sizeof(preview_shader) - 1, "preview.hlsl",
        nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.put(), nullptr));
    return bytecode;
}

std::pair<unsigned, unsigned> ClientSize(HWND window) {
    RECT rect{};
    winrt::check_bool(GetClientRect(window, &rect));
    return {static_cast<unsigned>(std::max(1L, rect.right - rect.left)),
        static_cast<unsigned>(std::max(1L, rect.bottom - rect.top))};
}

} // namespace

winrt::com_ptr<ID3D11Device> CreateD3d11Device(bool video) {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    const auto flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (video ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0);
    const auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    if (video && FAILED(result)) return CreateD3d11Device();
    winrt::check_hresult(result);
    context.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    return device;
}

D3d11Preview::D3d11Preview(HWND window, winrt::com_ptr<ID3D11Device> device)
    : window_(window), device_(device ? std::move(device) : CreateD3d11Device()) {
    device_->GetImmediateContext(context_.put());

    const auto dxgi_device = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgi_device->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

    const auto [width, height] = ClientSize(window_);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = swap_flags;

    winrt::com_ptr<IDXGISwapChain1> swap_chain;
    winrt::check_hresult(factory->CreateSwapChainForHwnd(device_.get(), window_, &desc,
        nullptr, nullptr, swap_chain.put()));
    winrt::check_hresult(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER));
    swap_chain_ = swap_chain.as<IDXGISwapChain2>();
    winrt::check_hresult(swap_chain_->SetMaximumFrameLatency(1));
    present_event_.attach(swap_chain_->GetFrameLatencyWaitableObject());
    if (!present_event_) {
        throw winrt::hresult_error(E_FAIL, L"无法获取 DXGI 帧等待对象。");
    }
    target_width_ = width;
    target_height_ = height;
    CreateTarget();

    const auto vertex_bytecode = CompileShader("vs_main", "vs_5_0");
    winrt::check_hresult(device_->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), nullptr, vertex_shader_.put()));
    const auto pixel_bytecode = CompileShader("ps_main", "ps_5_0");
    winrt::check_hresult(device_->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
        pixel_bytecode->GetBufferSize(), nullptr, pixel_shader_.put()));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device_->CreateSamplerState(&sampler, sampler_.put()));
}

D3d11Preview::~D3d11Preview() {
    context_->ClearState();
    target_ = nullptr;
    back_buffer_ = nullptr;
    swap_chain_ = nullptr;
    // Flip-model swap chains can be destroyed asynchronously by D3D11.
    context_->Flush();
}

void D3d11Preview::CreateTarget() {
    winrt::check_hresult(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer_.put_void()));
    winrt::check_hresult(device_->CreateRenderTargetView(back_buffer_.get(), nullptr, target_.put()));
}

void D3d11Preview::ResizeToWindow() {
    const auto [width, height] = ClientSize(window_);
    if (width == target_width_ && height == target_height_) {
        return;
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    target_ = nullptr;
    back_buffer_ = nullptr;
    winrt::check_hresult(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swap_flags));
    target_width_ = width;
    target_height_ = height;
    CreateTarget();
}

void D3d11Preview::EnsureSource(unsigned width, unsigned height, bool nv12) {
    if (width == source_width_ && height == source_height_ && (chroma_ != nullptr) == nv12) return;

    const auto create = [this](unsigned w, unsigned h, DXGI_FORMAT pixel_format) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = w;
        texture.Height = h;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = pixel_format;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        winrt::check_hresult(device_->CreateTexture2D(&texture, nullptr, source.put()));
        winrt::check_hresult(device_->CreateShaderResourceView(source.get(), nullptr, view.put()));
        return std::pair{std::move(source), std::move(view)};
    };

    auto [source, view] = create(width, height, nv12 ? DXGI_FORMAT_R8_UNORM : format);
    decltype(create(width, height, format)) chroma;
    if (nv12) {
        chroma = create(width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM);
    }
    source_ = std::move(source);
    source_view_ = std::move(view);
    chroma_ = std::move(chroma.first);
    chroma_view_ = std::move(chroma.second);
    source_width_ = width;
    source_height_ = height;
}

void D3d11Preview::Upload(const CapturedFrame& frame) {
    UploadTexture(frame.texture.get(), frame.metadata.width, frame.metadata.height);
}

void D3d11Preview::UploadTexture(ID3D11Texture2D* texture, unsigned width, unsigned height) {
    EnsureSource(width, height);
    const D3D11_BOX content{0, 0, 0, width, height, 1};
    context_->CopySubresourceRegion(source_.get(), 0, 0, 0, 0, texture, 0, &content);
}

void D3d11Preview::UploadBgra(const std::uint8_t* pixels, unsigned stride, unsigned width, unsigned height) {
    EnsureSource(width, height);
    context_->UpdateSubresource(source_.get(), 0, nullptr, pixels, stride, 0);
}

bool D3d11Preview::UploadNv12(const std::uint8_t* y, unsigned y_stride, const std::uint8_t* uv,
    unsigned uv_stride, unsigned width, unsigned height) {
    if (nv12_unavailable_ || !y || !uv || !width || !height || ((width | height) & 1u)
            || y_stride < width || uv_stride < width) return false;
    try {
        if (!nv12_shader_) {
            const auto bytecode = CompileShader("ps_nv12", "ps_5_0");
            winrt::check_hresult(device_->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                nullptr, nv12_shader_.put()));
        }
        EnsureSource(width, height, true);
    } catch (const winrt::hresult_error&) {
        nv12_unavailable_ = true;
        return false;
    }
    context_->UpdateSubresource(source_.get(), 0, nullptr, y, y_stride, 0);
    context_->UpdateSubresource(chroma_.get(), 0, nullptr, uv, uv_stride, 0);
    return true;
}

ID3D11PixelShader* D3d11Preview::SelectPixelShader(bool enhance_text) {
    auto* standard = chroma_view_ ? nv12_shader_.get() : pixel_shader_.get();
    if (!enhance_text || text_filter_unavailable_ || source_width_ > 1280 || source_height_ > 720) return standard;
    auto& shader = chroma_view_ ? nv12_text_shader_ : text_shader_;
    if (!shader) {
        try {
            const auto bytecode = CompileShader(chroma_view_ ? "ps_nv12_text" : "ps_main_text", "ps_5_0");
            winrt::check_hresult(device_->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                nullptr, shader.put()));
        } catch (const winrt::hresult_error&) {
            text_filter_unavailable_ = true;
            return standard;
        }
    }
    return shader.get();
}

void D3d11Preview::Draw(bool enhance_text) {
    const auto fitted = Viewport(source_width_, source_height_);
    const D3D11_VIEWPORT viewport{
        static_cast<float>(fitted.left), static_cast<float>(fitted.top),
        static_cast<float>(fitted.width), static_cast<float>(fitted.height), 0, 1};

    constexpr float black[] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(target_.get(), black);
    auto* target = target_.get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
    context_->PSSetShader(SelectPixelShader(enhance_text), nullptr, 0);
    auto* sampler = sampler_.get();
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* views[] = {source_view_.get(), chroma_view_.get()};
    context_->PSSetShaderResources(0, 2, views);
    context_->Draw(3, 0);
    views[0] = views[1] = nullptr;
    context_->PSSetShaderResources(0, 2, views);
}

bool D3d11Preview::Present(unsigned sync_interval) {
    const auto result = swap_chain_->Present(sync_interval, 0);
    winrt::check_hresult(result);
    return result != DXGI_STATUS_OCCLUDED;
}

} // namespace p2premote
