#include "video_surface_adapter.h"
#include "../connection_overlay.h"

#include <QMetaObject>
#include <QWidget>

#include <utility>

namespace {

class VideoContainer final : public QWidget {
public:
    VideoContainer(std::function<void()> fullscreen, QWidget* parent)
        : QWidget(parent), fullscreen_(std::move(fullscreen)) {
        setObjectName(QStringLiteral("video"));
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
        setFocusPolicy(Qt::NoFocus);
    }

    static void requestFullscreen(void* context) {
        auto* widget = static_cast<VideoContainer*>(context);
        QMetaObject::invokeMethod(widget, [widget] {
            if (widget->fullscreen_) widget->fullscreen_();
        }, Qt::QueuedConnection);
    }

private:
    std::function<void()> fullscreen_;
};

class WinVideoSurface final : public VideoSurfaceAdapter {
public:
    WinVideoSurface(std::function<void()> fullscreen, QWidget* parent)
        : widget_(new VideoContainer(std::move(fullscreen), parent)) {}

    QWidget* widget() const override { return widget_; }
    p2p_render_target_v2 target() override {
        return {P2P_RENDER_TARGET_WIN32_WINDOW, static_cast<uintptr_t>(widget_->winId()),
            VideoContainer::requestFullscreen, widget_};
    }

    // Native presentation publishes its first frame through the stream snapshot.
    void setFirstFrameCallback(std::function<void()>) override {}
    void renderOverlays(unsigned width, unsigned height, std::vector<p2premote::PluginOverlayItem> items) override {
        overlays_.Render(reinterpret_cast<HWND>(widget_->winId()), width, height, items);
    }
    void hideOverlays() override { overlays_.Hide(); }

private:
    QWidget* widget_; // Owned by its Qt parent; the stream is destroyed before that parent.
    p2premote::viewer::OverlayManager overlays_;
};

} // namespace

std::unique_ptr<VideoSurfaceAdapter> createVideoSurfaceAdapter(std::function<void()> fullscreen, QWidget* parent) {
    return std::make_unique<WinVideoSurface>(std::move(fullscreen), parent);
}
