#pragma once

#include <p2premote/video_viewport.h>

#include <windows.h>
#include <array>

namespace p2premote::viewer {

inline LRESULT CALLBACK ConnectionOverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message != WM_PAINT) return DefWindowProcW(window, message, wparam, lparam);

    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const auto dpi = GetDpiForWindow(window);
    const auto px = [dpi](int n) { return MulDiv(n, dpi, 96); };
    // Color-key the corners. The layered child lets the live desktop show through.
    SetDCBrushColor(dc, RGB(255, 0, 255));
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, RGB(39, 41, 45));
    RoundRect(dc, 0, 0, bounds.right, bounds.bottom, px(9), px(9));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    const auto font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Consolas");
    const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::array<wchar_t, 512> text{};
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    InflateRect(&bounds, -px(8), -px(6));
    DrawTextW(dc, text.data(), -1, &bounds, DT_LEFT | DT_TOP | DT_NOPREFIX);
    SelectObject(dc, old_font);
    if (font) DeleteObject(font);
    EndPaint(window, &paint);
    return 0;
}

inline HWND CreateConnectionOverlay(HWND parent) {
    static const ATOM registered = [] {
        WNDCLASSW cls{};
        cls.lpfnWndProc = ConnectionOverlayProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"P2PRemote.ConnectionOverlay";
        return RegisterClassW(&cls);
    }();
    if (!registered) return nullptr;
    const auto window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, MAKEINTATOM(registered), L"",
        WS_CHILD | WS_DISABLED, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window && !SetLayeredWindowAttributes(window, RGB(255, 0, 255), 190, LWA_ALPHA | LWA_COLORKEY)) {
        DestroyWindow(window);
        return nullptr;
    }
    return window;
}

inline void PlaceConnectionOverlay(HWND window, unsigned frame_width, unsigned frame_height,
        int logical_width, int logical_height) {
    RECT client{};
    GetClientRect(GetParent(window), &client);
    const auto viewport = FitVideo(client.right, client.bottom, frame_width, frame_height);
    const auto dpi = GetDpiForWindow(window);
    const int margin = MulDiv(8, dpi, 96);
    const int width = MulDiv(logical_width, dpi, 96), height = MulDiv(logical_height, dpi, 96);
    if (static_cast<int>(viewport.width) < width + 2 * margin || static_cast<int>(viewport.height) < height + 2 * margin) {
        ShowWindow(window, SW_HIDE);
        return;
    }
    SetWindowPos(window, HWND_TOP, static_cast<int>(viewport.left + viewport.width) - width - margin,
        static_cast<int>(viewport.top + viewport.height) - height - margin, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace p2premote::viewer
