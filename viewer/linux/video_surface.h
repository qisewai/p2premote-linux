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

QWidget* createVideoSurface(std::function<void()> fullscreen, QWidget* parent);
void attachVideoSession(QWidget* widget, ViewerSession* session);
void setVideoCursor(QWidget* widget, DecodedCursor cursor);
void setVideoOverlays(QWidget* widget, std::vector<p2premote::PluginOverlayItem> overlays);
