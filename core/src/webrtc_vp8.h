#pragma once

#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"

#include <algorithm>

namespace p2premote {

class Vp8EncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        auto formats = builtin_->GetSupportedFormats();
        std::erase_if(formats, [](const auto& format) { return format.name != "VP8"; });
        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& environment,
                                               const webrtc::SdpVideoFormat& format) override {
        return format.name == "VP8" ? builtin_->Create(environment, format) : nullptr;
    }

private:
    std::unique_ptr<webrtc::VideoEncoderFactory> builtin_ = webrtc::CreateBuiltinVideoEncoderFactory();
};

class Vp8DecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        auto formats = builtin_->GetSupportedFormats();
        std::erase_if(formats, [](const auto& format) { return format.name != "VP8"; });
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& environment,
                                               const webrtc::SdpVideoFormat& format) override {
        return format.name == "VP8" ? builtin_->Create(environment, format) : nullptr;
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> builtin_ = webrtc::CreateBuiltinVideoDecoderFactory();
};

} // namespace p2premote
