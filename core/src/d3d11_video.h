#pragma once

#include <d3d11_1.h>
#include <winrt/base.h>

#include "api/video/video_frame_buffer.h"

namespace p2premote {

class Nv12Frame : public webrtc::VideoFrameBuffer {
public:
    Nv12Frame(winrt::com_ptr<ID3D11Texture2D> texture, UINT subresource, int width, int height,
        winrt::com_ptr<IUnknown> lease = {});

    Type type() const override { return Type::kNative; }
    int width() const override { return width_; }
    int height() const override { return height_; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
    ID3D11Texture2D* texture() const { return texture_.get(); }
    UINT subresource() const { return subresource_; }

private:
    winrt::com_ptr<IUnknown> lease_;
    winrt::com_ptr<ID3D11Texture2D> texture_;
    UINT subresource_;
    int width_;
    int height_;
};

class D3d11VideoConverter {
public:
    explicit D3d11VideoConverter(ID3D11Device* device);
    winrt::com_ptr<ID3D11Texture2D> Convert(ID3D11Texture2D* source, UINT subresource, UINT source_width,
        UINT source_height, UINT width, UINT height, DXGI_FORMAT format);
    void Blit(ID3D11Texture2D* source, UINT subresource, UINT source_width, UINT source_height,
        ID3D11Texture2D* target, const RECT& destination);

private:
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11VideoDevice> video_device_;
    winrt::com_ptr<ID3D11VideoContext1> video_context_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    UINT source_width_{};
    UINT source_height_{};
    UINT width_{};
    UINT height_{};
};

} // namespace p2premote
