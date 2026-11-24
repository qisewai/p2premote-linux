#include "connection_overlay.h"

#include <p2premote/video_viewport.h>

#include <algorithm>
#include <tuple>

namespace p2premote::viewer {
namespace {

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateOverlay(HWND parent) {
    static const ATOM registered = [] {
        WNDCLASSW cls{};
        cls.lpfnWndProc = OverlayProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"P2PRemote.ConnectionOverlay";
        return RegisterClassW(&cls);
    }();
    if (!registered) return nullptr;
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, MAKEINTATOM(registered), L"",
        WS_CHILD | WS_DISABLED, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

COLORREF GdiColor(std::uint32_t rgb, COLORREF fallback) {
    return rgb == overlay_default_color ? fallback : RGB(rgb >> 16, rgb >> 8, rgb);
}

HFONT CreateOverlayFont(const PluginOverlayItem& item, UINT dpi) {
    return CreateFontW(-MulDiv(std::clamp(item.font_size, 9, 20), dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
        item.monospace ? L"Consolas" : L"Segoe UI");
}

SIZE Measure(HDC dc, const PluginOverlayItem& item, UINT dpi) {
    const auto font = CreateOverlayFont(item, dpi);
    const auto previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT bounds{};
    DrawTextW(dc, item.text.data(), static_cast<int>(item.text.size()), &bounds, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, previous);
    if (font) DeleteObject(font);
    return {bounds.right + 2 * MulDiv(7, dpi, 96) + 1, bounds.bottom + 2 * MulDiv(5, dpi, 96)};
}

bool PresentOverlay(HWND window, HDC dc, const RECT& bounds, const PluginOverlayItem& item, UINT dpi) {
    SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = size.cx;
    format.bmiHeader.biHeight = -size.cy;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    void* data{};
    const auto bitmap = CreateDIBSection(dc, &format, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!bitmap) return false;
    const auto previous = SelectObject(dc, bitmap);

    const RECT frame{0, 0, size.cx, size.cy};
    const int diameter = MulDiv(8, dpi, 96);
    const auto clip = CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, diameter, diameter);
    if (clip) SelectClipRgn(dc, clip);
    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, GdiColor(item.bg_rgb, RGB(39, 41, 45)));
    FillRect(dc, &frame, brush);
    const auto font = CreateOverlayFont(item, dpi);
    const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GdiColor(item.text_rgb, RGB(255, 255, 255)));
    auto text = frame;
    InflateRect(&text, -MulDiv(7, dpi, 96), -MulDiv(5, dpi, 96));
    DrawTextW(dc, item.text.data(), static_cast<int>(item.text.size()), &text, DT_TOP | DT_LEFT | DT_NOPREFIX);
    GdiFlush();
    auto* pixels = static_cast<std::uint32_t*>(data);
    for (LONG y = 0; y < size.cy; ++y) {
        for (LONG x = 0; x < size.cx; ++x, ++pixels) {
            const bool corner = (x < diameter || x >= size.cx - diameter)
                && (y < diameter || y >= size.cy - diameter);
            *pixels = !clip || !corner || PtInRegion(clip, x, y) ? (*pixels | 0xff000000) : 0;
        }
    }
    // The layer is a child window, so its destination is relative to the video surface.
    POINT destination{bounds.left, bounds.top};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, item.opacity, AC_SRC_ALPHA};
    const bool ok = UpdateLayeredWindow(window, nullptr, &destination, &size, dc, &origin, 0, &blend, ULW_ALPHA);

    SelectObject(dc, old_font);
    if (font) DeleteObject(font);
    SelectClipRgn(dc, nullptr);
    if (clip) DeleteObject(clip);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    return ok;
}

} // namespace

std::optional<RECT> FitOverlay(const RECT& area, SIZE size, OverlayAnchor anchor, int gap,
    std::span<const RECT> occupied) {
    if (size.cx <= 0 || size.cy <= 0 || size.cx > area.right - area.left || size.cy > area.bottom - area.top)
        return std::nullopt;
    const auto index = static_cast<int>(anchor);
    const auto column = index % 3;
    const bool top = index < 3;
    const LONG x = column == 0 ? area.left :
        (column == 1 ? (area.left + area.right - size.cx) / 2 : area.right - size.cx);
    LONG y = top ? area.top : area.bottom - size.cy;
    while (y >= area.top && y + size.cy <= area.bottom) {
        const RECT candidate{x, y, x + size.cx, y + size.cy};
        const auto conflict = std::ranges::find_if(occupied, [&](const RECT& rect) {
            return rect.left < candidate.right + gap && rect.right + gap > candidate.left
                && rect.top < candidate.bottom + gap && rect.bottom + gap > candidate.top;
        });
        if (conflict == occupied.end()) return candidate;
        y = top ? conflict->bottom + gap : conflict->top - gap - size.cy;
    }
    return std::nullopt;
}

OverlayManager::~OverlayManager() = default;

OverlayManager::Tip::~Tip() {
    if (IsWindow(window)) DestroyWindow(window);
}

void OverlayManager::Hide() {
    for (auto& [id, tip] : tips_) {
        if (tip.shown) ShowWindow(tip.window, SW_HIDE);
        tip.shown = false;
    }
}

void OverlayManager::Render(HWND parent, unsigned width, unsigned height, std::span<const PluginOverlayItem> items) {
    if (!parent || !width || !height) {
        Hide();
        return;
    }
    std::erase_if(tips_, [&](const auto& entry) {
        return std::ranges::find(items, entry.first, &PluginOverlayItem::id) == items.end()
            || (entry.second.window && (!IsWindow(entry.second.window) || GetParent(entry.second.window) != parent));
    });
    RECT client{};
    GetClientRect(parent, &client);
    const auto viewport = FitVideo(client.right, client.bottom, width, height);
    const auto dpi = GetDpiForWindow(parent);
    const int gap = MulDiv(6, dpi, 96);
    RECT area{static_cast<LONG>(viewport.left), static_cast<LONG>(viewport.top),
        static_cast<LONG>(viewport.left + viewport.width), static_cast<LONG>(viewport.top + viewport.height)};
    InflateRect(&area, -MulDiv(8, dpi, 96), -MulDiv(8, dpi, 96));
    const auto dc = CreateCompatibleDC(nullptr);
    if (!dc) return;
    struct DcOwner { HDC dc; ~DcOwner() { DeleteDC(dc); } } dc_owner{dc};

    std::vector<const PluginOverlayItem*> ordered;
    for (const auto& item : items) {
        if (item.visible && !item.text.empty() && item.opacity) ordered.push_back(&item);
        else if (const auto it = tips_.find(item.id); it != tips_.end() && it->second.shown) {
            ShowWindow(it->second.window, SW_HIDE);
            it->second.shown = false;
        }
    }
    std::ranges::sort(ordered, [](const auto* left, const auto* right) {
        return std::tie(left->priority, left->id) < std::tie(right->priority, right->id);
    });
    std::vector<RECT> occupied;
    for (const auto* item : ordered) {
        auto& tip = tips_[item->id];
        const bool font_changed = tip.dpi != dpi || tip.item.font_size != item->font_size
            || tip.item.monospace != item->monospace;
        if (font_changed) tip.stable_width = 0;
        if (font_changed || tip.item.text != item->text) tip.text_size = Measure(dc, *item, dpi);
        tip.stable_width = std::max(tip.stable_width, tip.text_size.cx);
        auto size = tip.text_size;
        if (tip.stable_width <= area.right - area.left) size.cx = tip.stable_width;
        const auto position = FitOverlay(area, size, item->anchor, gap, occupied);
        if (!position) {
            if (tip.shown) ShowWindow(tip.window, SW_HIDE);
            tip.shown = false;
            continue;
        }
        if (!tip.window) tip.window = CreateOverlay(parent);
        if (!tip.window) continue;
        const bool moved = !tip.presented || !EqualRect(&*tip.presented, &*position);
        const bool raster_changed = !tip.presented || font_changed || tip.item.text != item->text
            || tip.item.bg_rgb != item->bg_rgb || tip.item.text_rgb != item->text_rgb || tip.item.opacity != item->opacity
            || tip.presented->right - tip.presented->left != size.cx || tip.presented->bottom - tip.presented->top != size.cy;
        if (raster_changed) {
            if (!PresentOverlay(tip.window, dc, *position, *item, dpi)) continue;
            tip.dpi = dpi;
            tip.item = *item;
        } else if (moved) {
            if (!SetWindowPos(tip.window, nullptr, position->left, position->top, 0, 0,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE)) continue;
        }
        tip.presented = position;
        // The Qt viewer's swap chain is a sibling HWND. Raise the tip when showing it,
        // then preserve its z-order during text and geometry updates.
        if (!tip.shown) {
            if (!SetWindowPos(tip.window, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) continue;
            tip.shown = true;
        }
        occupied.push_back(*position);
    }
}

} // namespace p2premote::viewer
