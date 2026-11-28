#include "../qt/video_surface_adapter.h"
#include "video_surface.h"

#include <utility>

namespace {

class LinuxVideoSurface final : public VideoSurfaceAdapter {
public:
    LinuxVideoSurface(std::function<void()> fullscreen, QWidget* parent)
        : widget_(createVideoSurface(parent)), fullscreen_(std::move(fullscreen)) {}

    QWidget* widget() const override { return widget_; }
    p2p_render_target_v2 target() override {
        return {P2P_RENDER_TARGET_QT_VIDEO_SURFACE, reinterpret_cast<uintptr_t>(widget_), requestFullscreen, this};
    }
    void setFirstFrameCallback(std::function<void()> callback) override {
        setVideoFirstFrameCallback(widget_, std::move(callback));
    }
    void renderOverlays(unsigned, unsigned, std::vector<p2premote::PluginOverlayItem> items) override {
        setVideoOverlays(widget_, std::move(items));
        overlays_visible_ = true;
    }
    void hideOverlays() override {
        if (overlays_visible_) {
            setVideoOverlays(widget_, {});
            overlays_visible_ = false;
        }
    }

private:
    static void requestFullscreen(void* context) {
        const auto& surface = *static_cast<LinuxVideoSurface*>(context);
        if (surface.fullscreen_) surface.fullscreen_();
    }

    QWidget* widget_; // Owned by its Qt parent.
    std::function<void()> fullscreen_;
    bool overlays_visible_{};
};

} // namespace

std::unique_ptr<VideoSurfaceAdapter> createVideoSurfaceAdapter(std::function<void()> fullscreen, QWidget* parent) {
    return std::make_unique<LinuxVideoSurface>(std::move(fullscreen), parent);
}
