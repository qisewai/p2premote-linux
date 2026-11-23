#pragma once

#include "media_codec.h"

namespace minirtc {

std::unique_ptr<MediaCodec> CreateWindowsH264Encoder(std::shared_ptr<SystemClock> clock, void* device);
std::unique_ptr<MediaCodec> CreateWindowsH264Decoder(std::shared_ptr<SystemClock> clock, void* device);

} // namespace minirtc
