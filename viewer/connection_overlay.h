#pragma once

#include <p2premote/overlay.h>

#include <windows.h>

#include <map>
#include <span>

namespace p2premote::viewer {

std::optional<RECT> FitOverlay(const RECT& area, SIZE size, OverlayAnchor anchor, int gap,
    std::span<const RECT> occupied);

class OverlayManager {
public:
    OverlayManager() = default;
    ~OverlayManager();
    OverlayManager(const OverlayManager&) = delete;
    OverlayManager& operator=(const OverlayManager&) = delete;

    void Render(HWND parent, unsigned width, unsigned height, std::span<const PluginOverlayItem> items);
    void Hide();

private:
    struct Tip {
        ~Tip();
        HWND window{};
        UINT dpi{};
        SIZE text_size{};
        LONG stable_width{};
        PluginOverlayItem item;
        std::optional<RECT> presented;
        bool shown{};
    };
    std::map<std::wstring, Tip> tips_;
};

} // namespace p2premote::viewer
