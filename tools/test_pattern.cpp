#include <windows.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwchar>

namespace {

constexpr wchar_t pattern_class[] = L"P2PRemote.TestPattern";
constexpr UINT_PTR animation_timer = 1;

void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Paint(HWND window) {
    static unsigned long long frame_number{};
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    const auto milliseconds = ticks.QuadPart / frequency.QuadPart * 1000
        + ticks.QuadPart % frequency.QuadPart * 1000 / frequency.QuadPart;

    PAINTSTRUCT paint{};
    const auto window_dc = BeginPaint(window, &paint);
    RECT rect{};
    GetClientRect(window, &rect);
    HDC buffer_dc{};
    const auto buffer = BeginBufferedPaint(window_dc, &rect, BPBF_COMPATIBLEBITMAP, nullptr, &buffer_dc);
    const auto dc = buffer ? buffer_dc : window_dc;

    Fill(dc, rect, RGB(22, 28, 39));
    const auto band = std::max(1L, rect.bottom / 6);
    Fill(dc, {0, 0, rect.right / 3, band}, RGB(255, 0, 0));
    Fill(dc, {rect.right / 3, 0, rect.right * 2 / 3, band}, RGB(0, 255, 0));
    Fill(dc, {rect.right * 2 / 3, 0, rect.right, band}, RGB(0, 0, 255));
    const auto flash = milliseconds / 250 % 2 ? RGB(245, 245, 245) : RGB(40, 40, 40);
    Fill(dc, {0, band, rect.right, 2 * band}, flash);

    const auto block_width = std::max(1L, rect.right / 8);
    const auto step = static_cast<LONG>(milliseconds % 2000);
    const auto block_x = (rect.right - block_width) * step / 2000;
    Fill(dc, {block_x, 4 * band, block_x + block_width, 5 * band}, RGB(255, 205, 45));

    wchar_t label[64]{};
    swprintf_s(label, L"FRAME %010llu", ++frame_number);
    const auto font = CreateFontW(-std::max(1L, band * 2 / 3), 0, 0, 0, FW_BOLD, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
        L"Consolas");
    const auto old_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT label_rect{0, 2 * band, rect.right, 4 * band};
    DrawTextW(dc, label, -1, &label_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font) {
        SelectObject(dc, old_font);
    }
    if (font) {
        DeleteObject(font);
    }

    if (buffer) {
        EndBufferedPaint(buffer, TRUE);
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        return SetTimer(window, animation_timer, 16, nullptr) ? 0 : -1;
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint(window);
        return 0;
    case WM_DPICHANGED: {
        const auto& rect = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, animation_timer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSW window_class{};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = pattern_class;
    if (!RegisterClassW(&window_class)) {
        return 1;
    }
    const auto buffered = SUCCEEDED(BufferedPaintInit());
    const auto window = CreateWindowExW(0, pattern_class, L"P2P Remote · 动态测试图", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 560, nullptr, nullptr, instance, nullptr);
    int exit_code = 1;
    if (window) {
        ShowWindow(window, show);
        MSG message{};
        BOOL result{};
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        exit_code = result == 0 ? static_cast<int>(message.wParam) : 1;
    }
    if (buffered) {
        BufferedPaintUnInit();
    }
    return exit_code;
}
