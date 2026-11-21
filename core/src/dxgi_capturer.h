#pragma once

#include "wgc_capturer.h"

namespace p2premote {

std::unique_ptr<IVideoCapturer> StartDesktopDuplication(HMONITOR monitor, ID3D11Device* device);

} // namespace p2premote
