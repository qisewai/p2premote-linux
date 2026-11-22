#pragma once

#include "minirtc.h"

namespace p2premote {

inline void ConfigureMiniRtcDesktopVideo(Params& params, unsigned frame_rate) noexcept {
    params.video_content_type = VideoContentType::ScreenContent;
    params.video_quality = QualityHigh;
    params.video_frame_rate = frame_rate;
    // Keep text legible when the captured desktop runs below the frame-rate target.
    params.video_degradation_preference = VideoDegradationPreference::MaintainResolution;
}

} // namespace p2premote
