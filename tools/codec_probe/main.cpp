#include "d3d11_preview.h"
#include "d3d11_video.h"
#include "mf_h264.h"

#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_common.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

struct EncoderResult final : webrtc::EncodedImageCallback {
    Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
        data = image;
        return Result(Result::OK);
    }
    void OnFrameDropped(uint32_t, int, bool) override {}
    std::optional<webrtc::EncodedImage> data;
};

struct ErrorLog final : webrtc::LogSink {
    void OnLogMessage(const std::string& message) override { std::cerr << message; }
};

struct DecoderResult final : webrtc::DecodedImageCallback {
    int32_t Decoded(webrtc::VideoFrame& frame) override {
        data = frame.video_frame_buffer();
        return 0;
    }
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> data;
};

void Check(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

void CheckPixels(ID3D11Device* device, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> readback;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, readback.put()));
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    context->CopyResource(readback.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto* row = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.RowPitch) * (desc.Height / 2));
    const auto left = row[desc.Width / 4];
    const auto right = row[3 * desc.Width / 4];
    context->Unmap(readback.get(), 0);
    Check(((left >> 16) & 255) > 180 && (left & 255) < 65 && (right & 255) > 180
        && ((right >> 16) & 255) < 65, "GPU conversion or codec changed RGB bars");
}

} // namespace

int main() {
    static ErrorLog log;
    webrtc::LogMessage::AddLogToStream(&log, webrtc::LS_ERROR);
    try {
        winrt::init_apartment();
        webrtc::LogMessage::LogToDebug(webrtc::LS_ERROR);
        auto device = p2premote::CreateD3d11Device(true);
        auto active = std::make_shared<std::atomic_bool>(false);
        auto encode_factory = p2premote::CreateVideoEncoderFactory(device.get(), active);
        auto decode_factory = p2premote::CreateVideoDecoderFactory(device.get());
        const auto encode_formats = encode_factory->GetSupportedFormats();
        const auto decode_formats = decode_factory->GetSupportedFormats();
        Check(!encode_formats.empty() && encode_formats.front().name == "H264", "Hardware H.264 encoder unavailable");
        Check(!decode_formats.empty() && decode_formats.front().name == "H264", "DXVA H.264 decoder unavailable");
        const auto env = webrtc::CreateEnvironment();
        EncoderResult encoded;
        DecoderResult decoded;
        auto encoder = encode_factory->Create(env, encode_formats.front());
        auto decoder = decode_factory->Create(env, decode_formats.front());
        Check(encoder && decoder, "Could not create codec instances");
        encoder->RegisterEncodeCompleteCallback(&encoded);
        decoder->RegisterDecodeCompleteCallback(&decoded);
        Check(decoder->Configure({}), "Could not configure decoder");
        webrtc::VideoCodec codec{};
        codec.codecType = webrtc::kVideoCodecH264;
        codec.width = 1280;
        codec.height = 720;
        codec.maxFramerate = 60;
        codec.startBitrate = 4500;
        const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 2, 1200);
        Check(encoder->InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_OK && active->load(), "Could not configure hardware encoder");

        constexpr unsigned source_width = 1920;
        constexpr unsigned source_height = 1080;
        std::vector<uint32_t> pixels(source_width * source_height);
        for (unsigned y = 0; y < source_height; ++y) {
            for (unsigned x = 0; x < source_width; ++x) {
                pixels[y * source_width + x] = x < source_width / 2 ? 0xffff2020 : 0xff2020ff;
            }
        }
        D3D11_TEXTURE2D_DESC desc{source_width, source_height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
            D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_SUBRESOURCE_DATA initial{pixels.data(), source_width * 4, 0};
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(device->CreateTexture2D(&desc, &initial, source.put()));
        p2premote::D3d11VideoConverter to_nv12(device.get());
        p2premote::D3d11VideoConverter to_bgra(device.get());
        int sequence = 0;
        for (const auto& [width, height] : {std::pair{1280, 720}, std::pair{1920, 1080}}) {
            const auto start = std::chrono::steady_clock::now();
            int rendered = 0;
            for (int i = 0; i < 24; ++i) {
                auto nv12 = to_nv12.Convert(source.get(), 0, source_width, source_height, width, height, DXGI_FORMAT_NV12);
                auto video = webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(webrtc::make_ref_counted<p2premote::Nv12Frame>(std::move(nv12), 0, width, height))
                    .set_timestamp_us(static_cast<int64_t>(++sequence) * 16'667)
                    .set_timestamp_rtp(static_cast<uint32_t>(sequence) * 1500).build();
                const std::vector types{i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta};
                encoded.data.reset();
                Check(encoder->Encode(video, &types) == WEBRTC_VIDEO_CODEC_OK && encoded.data.has_value(), "Hardware encode failed");
                decoded.data = nullptr;
                Check(decoder->Decode(*encoded.data, 0) >= 0, "DXVA decode failed");
                if (!decoded.data) continue;
                Check(decoded.data->type() == webrtc::VideoFrameBuffer::Type::kNative, "Decoded frame is not a GPU texture");
                Check(decoded.data->width() == width && decoded.data->height() == height, "Unexpected decoded size");
                const auto* frame = static_cast<const p2premote::Nv12Frame*>(decoded.data.get());
                auto bgra = to_bgra.Convert(frame->texture(), frame->subresource(), width, height,
                    width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
                if (!rendered) CheckPixels(device.get(), bgra.get());
                ++rendered;
            }
            Check(rendered >= 23, "Decoder delayed more than one frame");
            const std::chrono::duration<double, std::milli> time = std::chrono::steady_clock::now() - start;
            std::cout << width << "x" << height << " " << rendered << "/24 GPU frames, "
                << time.count() / 24 << " ms/frame (transform submission and one sample readback)" << '\n';
        }
        Check(decoder->GetDecoderInfo().is_hardware_accelerated, "Hardware decode inactive");
        encoder->Release();
        decoder->Release();
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "Graphics or Media Foundation error " << std::hex << error.code().value << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
