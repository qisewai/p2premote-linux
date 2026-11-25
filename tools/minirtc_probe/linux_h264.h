#pragma once

#include <memory>

#include "clock/system_clock.h"
#include "media_codec.h"

namespace minirtc {

std::unique_ptr<MediaCodec> CreateLinuxH264Decoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration);

}  // namespace minirtc
