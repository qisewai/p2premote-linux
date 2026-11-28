#pragma once

#include "cursor_shape.h"

#include <p2premote/overlay.h>

#include <QByteArray>

#include <chrono>
#include <functional>
#include <vector>

class QWidget;
class ViewerSession;

struct VideoFrame {
    int width{};
    int height{};
    QByteArray nv12;
    std::chrono::steady_clock::time_point decoded_at;
};

QWidget* createVideoSurface(QWidget* parent);
void setVideoFullscreenCallback(QWidget* widget, std::function<void()> callback);
void attachVideoSession(QWidget* widget, ViewerSession* session);
void setVideoCursor(QWidget* widget, DecodedCursor cursor);
void setVideoOverlays(QWidget* widget, std::vector<p2premote::PluginOverlayItem> overlays);
void setVideoFirstFrameCallback(QWidget* widget, std::function<void()> callback);
void notifyVideoFrameReady(QWidget* widget);
