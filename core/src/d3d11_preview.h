#pragma once

#include "wgc_capturer.h"

#include <p2premote/video_viewport.h>

#include <dxgi1_3.h>

namespace p2premote {

winrt::com_ptr<ID3D11Device> CreateD3d11Device(bool video = false);

class D3d11Preview {
public:
    explicit D3d11Preview(HWND window, winrt::com_ptr<ID3D11Device> device = {});
    ~D3d11Preview();

    [[nodiscard]] ID3D11Device* Device() const noexcept { return device_.get(); }
    [[nodiscard]] HANDLE PresentEvent() const noexcept { return present_event_.get(); }
    [[nodiscard]] bool HasImage() const noexcept { return source_view_ != nullptr; }
    [[nodiscard]] ID3D11Texture2D* BackBuffer() const noexcept { return back_buffer_.get(); }
    [[nodiscard]] VideoViewport Viewport(unsigned width, unsigned height) const {
        return FitVideo(target_width_, target_height_, width, height);
    }

    void ResizeToWindow();
    void Upload(const CapturedFrame& frame);
    void UploadTexture(ID3D11Texture2D* texture, unsigned width, unsigned height);
    void UploadBgra(const std::uint8_t* pixels, unsigned stride, unsigned width, unsigned height);
    [[nodiscard]] bool UploadNv12(const std::uint8_t* y, unsigned y_stride, const std::uint8_t* uv,
        unsigned uv_stride, unsigned width, unsigned height);
    [[nodiscard]] bool UploadNv12Texture(ID3D11Texture2D* texture, unsigned subresource,
        unsigned width, unsigned height);
    void Draw(bool enhance_text = false);
    [[nodiscard]] bool Present(unsigned sync_interval = 1);

private:
    void CreateTarget();
    void EnsureSource(unsigned width, unsigned height, bool nv12 = false);
    ID3D11PixelShader* SelectPixelShader(bool enhance_text);

    HWND window_{};
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGISwapChain2> swap_chain_;
    winrt::handle present_event_;
    winrt::com_ptr<ID3D11Texture2D> back_buffer_;
    winrt::com_ptr<ID3D11RenderTargetView> target_;
    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> pixel_shader_;
    winrt::com_ptr<ID3D11PixelShader> nv12_shader_;
    winrt::com_ptr<ID3D11PixelShader> text_shader_;
    winrt::com_ptr<ID3D11PixelShader> nv12_text_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Texture2D> source_;
    winrt::com_ptr<ID3D11ShaderResourceView> source_view_;
    winrt::com_ptr<ID3D11Texture2D> chroma_;
    winrt::com_ptr<ID3D11ShaderResourceView> chroma_view_;
    bool nv12_unavailable_{};
    bool nv12_texture_unavailable_{};
    bool text_filter_unavailable_{};
    unsigned target_width_{};
    unsigned target_height_{};
    unsigned source_width_{};
    unsigned source_height_{};
};

} // namespace p2premote
