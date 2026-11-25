#pragma once

#include "cursor_shape.h"

#include <p2premote/overlay.h>

#include <QByteArray>

#include <functional>
#include <vector>

class QWidget;
class ViewerSession;

struct VideoFrame {
    int width{};
    int height{};
    QByteArray nv12;
};

QWidget* createVideoSurface(std::function<void()> fullscreen, QWidget* parent);
void attachVideoSession(QWidget* widget, ViewerSession* session);
void setVideoFrame(QWidget* widget, VideoFrame& frame);
void setVideoCursor(QWidget* widget, DecodedCursor cursor);
void setVideoOverlays(QWidget* widget, std::vector<p2premote::PluginOverlayItem> overlays);
