#include "input_test_pad.h"

#include <uxtheme.h>
#include <winrt/base.h>

#include <algorithm>
#include <bit>
#include <format>

namespace {

using p2premote::InputButton;
using p2premote::InputKind;

constexpr wchar_t pad_class[] = L"P2PRemote.InputTestPad";
constexpr auto primary = static_cast<std::size_t>(InputButton::left);

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

} // namespace

InputTestPad::~InputTestPad() {
    if (window_) {
        DestroyWindow(window_);
    }
    if (buffered_) {
        BufferedPaintUnInit();
    }
}

HWND InputTestPad::Create(HWND parent, HINSTANCE instance) {
    buffered_ = SUCCEEDED(BufferedPaintInit());
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpszClassName = pad_class;
    cls.lpfnWndProc = WindowProc;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    winrt::check_bool(RegisterClassW(&cls));
    const auto handle = CreateWindowExW(0, pad_class, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, parent, nullptr, instance, this);
    winrt::check_pointer(handle);
    Redraw();
    return handle;
}

void InputTestPad::BeginSession(HMONITOR monitor) {
    monitor_ = monitor;
    clicks_ = 0;
    wheel_ = 0;
    last_key_ = 0;
    marker_.reset();
    text_.clear();
    Release();
}

void InputTestPad::SetReady(bool ready) {
    if (ready_ != ready) {
        ready_ = ready;
        Release();
    }
}

void InputTestPad::Release() {
    keys_.reset();
    buttons_.reset();
    pointer_.reset();
    last_reliable_ = 0;
    Redraw();
}

void InputTestPad::Expire() {
    if ((keys_.any() || buttons_.any()) && GetTickCount64() - last_reliable_ >= p2premote::input_lease_ms) {
        Release();
    }
}

RECT InputTestPad::Board() const {
    RECT bounds{};
    GetClientRect(window_, &bounds);
    const auto dpi = GetDpiForWindow(window_);
    const auto inset = MulDiv(10, dpi, 96);
    return {inset, MulDiv(33, dpi, 96), bounds.right - inset, bounds.bottom - MulDiv(56, dpi, 96)};
}

std::optional<POINT> InputTestPad::Pointer(const p2premote::InputPacket& input) const {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor_, &info)) {
        return std::nullopt;
    }
    const auto& monitor = info.rcMonitor;
    POINT point{monitor.left + MulDiv(input.x, monitor.right - monitor.left - 1, 65535),
        monitor.top + MulDiv(input.y, monitor.bottom - monitor.top - 1, 65535)};
    ScreenToClient(window_, &point);
    const auto board = Board();
    if (PtInRect(&board, point)) {
        return point;
    }
    if (buttons_.test(primary) && board.left < board.right && board.top < board.bottom) {
        point.x = std::clamp(point.x, board.left, board.right - 1);
        point.y = std::clamp(point.y, board.top, board.bottom - 1);
        return point;
    }
    return std::nullopt;
}

void InputTestPad::Apply(const p2premote::InputPacket& input) {
    if (!ready_) return;
    if (input.kind != InputKind::move) last_reliable_ = GetTickCount64();

    switch (input.kind) {
    case InputKind::move:
        pointer_ = Pointer(input);
        if (buttons_.test(primary) && pointer_) marker_ = pointer_;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    case InputKind::button_down:
        pointer_ = Pointer(input);
        if (pointer_ && !buttons_.test(input.code)) {
            buttons_.set(input.code);
            ++clicks_;
            if (input.code == primary) marker_ = pointer_;
        }
        break;
    case InputKind::button_up:
        pointer_ = Pointer(input);
        if (input.code == primary && buttons_.test(primary) && pointer_) marker_ = pointer_;
        buttons_.reset(input.code);
        break;
    case InputKind::wheel:
        pointer_ = Pointer(input);
        if (pointer_) wheel_ += std::bit_cast<std::int16_t>(input.code);
        break;
    case InputKind::key_down:
        last_key_ = input.code;
        keys_.set(input.code);
        break;
    case InputKind::key_up:
        keys_.reset(input.code);
        break;
    case InputKind::text:
        if (input.code == L'\b') {
            if (!text_.empty()) {
                const auto last = text_.back();
                text_.pop_back();
                if (last >= 0xdc00 && last <= 0xdfff && !text_.empty()
                        && text_.back() >= 0xd800 && text_.back() <= 0xdbff) text_.pop_back();
            }
        } else if (text_.size() < 120) {
            text_.push_back(input.code == L'\r' ? L'↵' : static_cast<wchar_t>(input.code));
        }
        break;
    case InputKind::reset:
        Release();
        return;
    case InputKind::set_quality:
    case InputKind::cursor_ready:
    case InputKind::heartbeat:
        return;
    }
    Redraw();
}

void InputTestPad::Redraw() {
    if (!window_) return;
    const auto state = std::format(L"点击 {}  |  滚轮 {:+.1f}  |  按住键 {}  |  按住鼠标 {}  |  扫描码 {:03X}\n{}",
        clicks_, static_cast<double>(wheel_) / WHEEL_DELTA, keys_.count(), buttons_.count(), last_key_, text_);
    SetWindowTextW(window_, state.c_str());
    InvalidateRect(window_, nullptr, FALSE);
}

void InputTestPad::Paint() const {
    PAINTSTRUCT paint{};
    const auto target = BeginPaint(window_, &paint);
    RECT bounds{};
    GetClientRect(window_, &bounds);
    HDC back{};
    const auto buffer = buffered_ ? BeginBufferedPaint(target, &bounds, BPBF_COMPATIBLEBITMAP, nullptr, &back) : nullptr;
    const auto dc = buffer ? back : target;
    Fill(dc, bounds, RGB(245, 247, 249));
    const auto board = Board();
    const auto font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(21, 38, 52));

    RECT heading{board.left, 0, board.right, board.top};
    DrawTextW(dc, ready_ ? L"在 Viewer 中点击变色、拖动标记、滚动或打字" : L"同机键鼠测试区 · 由 Host 授权后启用",
        -1, &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    Fill(dc, board, ready_ ? (clicks_ % 2 ? RGB(255, 215, 112) : RGB(203, 231, 255)) : RGB(221, 228, 233));
    if (ready_) {
        const auto marker = marker_.value_or(POINT{(board.left + board.right) / 2, (board.top + board.bottom) / 2});
        const auto saved = SaveDC(dc);
        IntersectClipRect(dc, board.left, board.top, board.right, board.bottom);
        const auto radius = MulDiv(11, GetDpiForWindow(window_), 96);
        Fill(dc, {marker.x - radius, marker.y - radius, marker.x + radius, marker.y + radius}, RGB(20, 73, 114));
        if (pointer_) {
            const auto pos = *pointer_;
            Fill(dc, {pos.x - radius, pos.y - 1, pos.x + radius + 1, pos.y + 2}, RGB(192, 26, 41));
            Fill(dc, {pos.x - 1, pos.y - radius, pos.x + 2, pos.y + radius + 1}, RGB(192, 26, 41));
        }
        RestoreDC(dc, saved);
    }
    const auto dpi = GetDpiForWindow(window_);
    auto row = RECT{board.left, board.bottom + MulDiv(4, dpi, 96), board.right, board.bottom + MulDiv(25, dpi, 96)};
    const auto counts = std::format(L"点击 {}   滚轮 {:+.1f}   按住键 {}   按住鼠标 {}   扫描码 {:03X}",
        clicks_, static_cast<double>(wheel_) / WHEEL_DELTA, keys_.count(), buttons_.count(), last_key_);
    DrawTextW(dc, counts.c_str(), -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    row.top = row.bottom;
    row.bottom = bounds.bottom;
    const auto line = L"键盘回显　" + text_;
    DrawTextW(dc, line.c_str(), -1, &row, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, font);
    if (buffer) EndBufferedPaint(buffer, TRUE);
    EndPaint(window_, &paint);
}

LRESULT CALLBACK InputTestPad::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* pad = static_cast<InputTestPad*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        pad->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pad));
    }
    auto* pad = reinterpret_cast<InputTestPad*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (pad) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: pad->Paint(); return 0;
        case WM_NCDESTROY:
            pad->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
