#include "connection_overlay.h"

#include <p2premote/video_viewport.h>
#include <commctrl.h>
#include <cstdio>
#include <stdexcept>

using namespace p2premote;
using namespace p2premote::viewer;

namespace {

void Check(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

RECT RelativeBounds(HWND child, HWND parent) {
    RECT bounds{};
    Check(GetWindowRect(child, &bounds), "Could not read overlay bounds");
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}

LRESULT CALLBACK CountWindowChanges(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR ref) {
    if (message == WM_WINDOWPOSCHANGING || message == WM_SHOWWINDOW) ++*reinterpret_cast<unsigned*>(ref);
    return DefSubclassProc(window, message, wparam, lparam);
}

} // namespace

int main() {
    try {
        const RECT area{0, 0, 160, 100};
        const auto left = FitOverlay(area, {100, 30}, OverlayAnchor::top_left, 6, {});
        Check(left.has_value(), "Top-left layout failed");
        const auto center = FitOverlay(area, {100, 30}, OverlayAnchor::top_center, 6, std::span(&*left, 1));
        Check(center && center->top == 36, "Overlapping horizontal anchors did not stack");
        const RECT taken[]{*left, *center};
        const auto bottom = FitOverlay(area, {100, 30}, OverlayAnchor::bottom_right, 6, taken);
        Check(!bottom, "A colliding bottom anchor exceeded the viewport");

        const auto parent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
            -8000, -8000, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(parent != nullptr, "Could not create native test surface");
        struct Parent { HWND value; ~Parent() { DestroyWindow(value); } } test_surface{parent};
        const auto video = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 640, 360, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(video != nullptr, "Could not create sibling video window");

        OverlayManager manager;
        PluginOverlayItem status;
        status.id = L"connection-status:default";
        status.text = L"RTT 100.0 ms";
        manager.Render(parent, 1920, 1080, std::span(&status, 1));
        const auto native = FindWindowExW(parent, nullptr, L"P2PRemote.ConnectionOverlay", nullptr);
        Check(native && (GetWindowLongW(native, GWL_STYLE) & WS_VISIBLE), "Layered image was not presented");
        Check(GetTopWindow(parent) == native, "Overlay is underneath the video window");
        Check((GetWindowLongPtrW(native, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))
            == (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE), "Overlay can capture input");
        const auto first = RelativeBounds(native, parent);
        const int margin = MulDiv(8, GetDpiForWindow(parent), 96);
        Check(first.right == 640 - margin && first.bottom == 360 - margin, "Native overlay was not anchored to the video");

        unsigned changes{};
        Check(SetWindowSubclass(native, CountWindowChanges, 1, reinterpret_cast<DWORD_PTR>(&changes)),
            "Could not observe window changes");
        for (int i = 0; i < 10; ++i) manager.Render(parent, 1920, 1080, std::span(&status, 1));
        Check(!changes, "Identical statistics changed layered window visibility or geometry");
        status.text = L"RTT 9.0 ms";
        manager.Render(parent, 1920, 1080, std::span(&status, 1));
        const auto updated = RelativeBounds(native, parent);
        Check(EqualRect(&first, &updated), "Numeric text changed the stable overlay geometry");
        RemoveWindowSubclass(native, CountWindowChanges, 1);

        Check(SetWindowPos(parent, nullptr, 0, 0, 400, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
            "Could not resize the test surface");
        manager.Render(parent, 1920, 1080, std::span(&status, 1));
        const auto viewport = FitVideo(400, 360, 1920, 1080);
        const auto resized = RelativeBounds(native, parent);
        Check(resized.right == static_cast<int>(viewport.left + viewport.width) - margin
            && resized.bottom == static_cast<int>(viewport.top + viewport.height) - margin,
            "Resizing moved the overlay into the letterbox");

        manager.Hide();
        Check(!(GetWindowLongW(native, GWL_STYLE) & WS_VISIBLE), "Hide all did not hide the overlay");
        Check(SetWindowPos(video, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE),
            "Could not raise sibling video window");
        manager.Render(parent, 1920, 1080, std::span(&status, 1));
        Check(GetWindowLongW(native, GWL_STYLE) & WS_VISIBLE, "Previously rendered bitmap was not shown again");
        Check(GetTopWindow(parent) == native, "Reshown overlay remained underneath the video window");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
