#pragma once

#include <p2premote/overlay.h>

#include <functional>
#include <vector>

class QImage;
class QWidget;
class ViewerSession;

QWidget* createVideoSurface(std::function<void()> fullscreen, QWidget* parent);
void attachVideoSession(QWidget* widget, ViewerSession* session);
void setVideoFrame(QWidget* widget, QImage frame);
void setVideoOverlays(QWidget* widget, std::vector<p2premote::PluginOverlayItem> overlays);
