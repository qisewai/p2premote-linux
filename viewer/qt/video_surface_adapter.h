#pragma once

#include <p2premote/core_api_v2.h>
#include <p2premote/overlay.h>

#include <functional>
#include <memory>
#include <vector>

class QWidget;

class VideoSurfaceAdapter {
public:
    virtual ~VideoSurfaceAdapter() = default;
    virtual QWidget* widget() const = 0;
    virtual p2p_render_target_v2 target() = 0;
    virtual void setFirstFrameCallback(std::function<void()> callback) = 0;
    virtual void renderOverlays(unsigned width, unsigned height, std::vector<p2premote::PluginOverlayItem> items) = 0;
    virtual void hideOverlays() = 0;
};

std::unique_ptr<VideoSurfaceAdapter> createVideoSurfaceAdapter(std::function<void()> fullscreen, QWidget* parent);
