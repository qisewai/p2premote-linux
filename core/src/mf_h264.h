#pragma once

#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

#include <atomic>
#include <memory>

struct ID3D11Device;

namespace p2premote {

class LatencyTrace;

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory(ID3D11Device* device,
    std::shared_ptr<std::atomic_bool> hardware_active, bool dynamic_cadence = false,
    std::shared_ptr<LatencyTrace> trace = {});
std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory(ID3D11Device* device,
    std::shared_ptr<LatencyTrace> trace = {});

} // namespace p2premote
