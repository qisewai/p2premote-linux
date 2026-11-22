#include "stream_window.h"
#include "input_test_pad.h"

#include <p2premote/stream_session.h>
#include <p2premote/video_viewport.h>

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using p2premote::InputButton;
using p2premote::InputKind;
using p2premote::InputPacket;
using p2premote::ConnectionMode;
using p2premote::IcePath;
using p2premote::StreamFrameRate;
using p2premote::StreamInputMode;
using p2premote::StreamQuality;
using p2premote::StreamState;
using p2premote::StreamStatistics;

enum ControlId {
    screen_select = 200, room_code, connect_button, allow_button,
    stop_button, export_button, status_text, allow_input_button, low_latency_mode, quality_select, status_panel,
    overlay_toggle
};
constexpr std::array quality_levels{StreamQuality::p720, StreamQuality::p1080, StreamQuality::p1440};
constexpr UINT_PTR statistics_timer = 1;
constexpr UINT_PTR input_timer = 2;
constexpr UINT_PTR cursor_timer = 3;
constexpr int stop_control_hotkey = 1;
constexpr wchar_t stream_window_class[] = L"P2PRemote.Stream";
constexpr wchar_t screen_class[] = L"P2PRemote.VideoSurface";
constexpr wchar_t blank_class[] = L"P2PRemote.BlankSurface";
constexpr wchar_t overlay_class[] = L"P2PRemote.Diagnostics";

bool IsActive(StreamState state) { return state != StreamState::idle && state != StreamState::stopped; }

const wchar_t* NetworkProtocol(std::string_view protocol) {
    if (protocol == "udp") return L"UDP";
    if (protocol == "tcp") return L"TCP";
    if (protocol == "tls") return L"TLS";
    return L"—";
}

const wchar_t* NetworkPath(IcePath path) {
    switch (path) {
    case IcePath::host: return L"主机候选直连";
    case IcePath::nat: return L"NAT 直连";
    case IcePath::relay: return L"TURN 中继";
    default: return L"正在获取链路";
    }
}

std::wstring Milliseconds(std::optional<double> value) {
    return value ? std::format(L"{:.1f} ms", *value) : std::wstring(L"—");
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto dpi = GetDpiForWindow(window);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, RGB(20, 30, 43));
        FillRect(dc, &bounds, brush);
        const RECT accent{0, 0, bounds.right, px(2)};
        SetDCBrushColor(dc, RGB(92, 211, 207));
        FillRect(dc, &accent, brush);
        const auto font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto original = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(242, 248, 252));
        std::array<wchar_t, 512> text{};
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        InflateRect(&bounds, -px(11), -px(9));
        DrawTextW(dc, text.data(), -1, &bounds, DT_LEFT | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, original);
        if (font) DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

BOOL CALLBACK AddScreen(HMONITOR monitor, HDC, LPRECT, LPARAM context) noexcept {
    try {
        reinterpret_cast<std::vector<HMONITOR>*>(context)->push_back(monitor);
        return TRUE;
    } catch (...) {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
}

class StreamWindow {
public:
    StreamWindow(HINSTANCE instance, bool host, StreamLaunchOptions options)
        : instance_(instance), host_(host), internet_(options.connection.mode == ConnectionMode::internet),
          quick_connect_(options.connection.mode != ConnectionMode::session_code),
          quality_selection_(options.connection.SupportsQualitySelection()),
          poll_cursor_(options.connection.SupportsLocalCursor() && (host || !options.connection.video_cursor)),
          connect_on_open_(options.connect_on_open),
          lifetime_(options.lifetime),
          session_(host, std::move(options.connection)) {}
    ~StreamWindow() { if (window_) DestroyWindow(window_); }

    int Run(int show) {
        static std::once_flag window_classes;
        std::call_once(window_classes, [this] {
            WNDCLASSW cls{};
            cls.lpfnWndProc = WindowProc;
            cls.hInstance = instance_;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
            cls.lpszClassName = stream_window_class;
            winrt::check_bool(RegisterClassW(&cls));
            cls.lpfnWndProc = SurfaceProc;
            cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            cls.lpszClassName = screen_class;
            winrt::check_bool(RegisterClassW(&cls));
            cls.lpfnWndProc = DefWindowProcW;
            cls.lpszClassName = blank_class;
            winrt::check_bool(RegisterClassW(&cls));
            cls.lpfnWndProc = OverlayProc;
            cls.hbrBackground = nullptr;
            cls.lpszClassName = overlay_class;
            winrt::check_bool(RegisterClassW(&cls));
        });

        const auto dpi = GetDpiForSystem();
        const auto width = MulDiv(host_ ? 680 : 960, dpi, 96);
        const auto height = MulDiv(host_ ? (quick_connect_ ? 230 : 465) : 660, dpi, 96);
        RECT work_area{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
        const auto margin = MulDiv(12, dpi, 96);
        const bool side_by_side = work_area.right - work_area.left >= MulDiv(1640, dpi, 96) + 3 * margin;
        const auto x = host_ ? work_area.left + margin : work_area.right - width - margin;
        winrt::check_pointer(CreateWindowExW(0, stream_window_class,
            host_ ? L"P2P Remote · Host 屏幕共享" : L"P2P Remote · Viewer",
            (host_ ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN,
            side_by_side ? x : CW_USEDEFAULT, side_by_side ? work_area.top + margin : CW_USEDEFAULT, width, height,
            nullptr, nullptr, instance_, this));

        if (host_) {
            screens_ = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, screen_select);
            RefreshScreens();
            allow_ = Control(L"BUTTON", L"允许共享", BS_PUSHBUTTON | WS_TABSTOP, allow_button);
            allow_input_ = Control(L"BUTTON", quick_connect_ ? L"允许整桌面控制" : L"共享并允许键鼠测试",
                BS_PUSHBUTTON | WS_TABSTOP, allow_input_button);
            low_latency_ = Control(L"BUTTON", L"低延迟 60 fps", BS_AUTOCHECKBOX | WS_TABSTOP, low_latency_mode);
            if (quick_connect_) SendMessageW(low_latency_, BM_SETCHECK, BST_CHECKED, 0);
            if (!quick_connect_) input_pad_window_ = input_pad_.Create(window_, instance_);
        }
        code_label_ = Control(L"STATIC", L"会话代码", SS_LEFT, 0);
        code_ = Control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP | (host_ ? ES_READONLY : ES_UPPERCASE), room_code);
        SendMessageW(code_, EM_SETLIMITTEXT, 12, 0);
        const auto connect_label = internet_ ? (host_ ? L"等待公网连接" : L"连接公网 Host")
            : quick_connect_ ? (host_ ? L"等待直连" : L"一键直连") : (host_ ? L"创建会话" : L"连接");
        connect_ = Control(L"BUTTON", connect_label, BS_PUSHBUTTON | WS_TABSTOP, connect_button);
        stop_ = Control(L"BUTTON", L"断开", BS_PUSHBUTTON | WS_TABSTOP, stop_button);
        export_ = Control(L"BUTTON", L"导出 CSV", BS_PUSHBUTTON | WS_TABSTOP, export_button);
        if (!host_) status_panel_ = Control(L"BUTTON", L"连接状态", BS_GROUPBOX, status_panel);
        status_ = Control(L"STATIC", L"", SS_LEFT, status_text);
        if (quick_connect_) {
            ShowWindow(code_label_, SW_HIDE);
            ShowWindow(code_, SW_HIDE);
        }
        if (!host_) {
            if (quality_selection_) {
                quality_label_ = Control(L"STATIC", L"画质", SS_LEFT, 0);
                quality_ = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, quality_select);
                for (const auto* label : {L"720p · 4 Mbps", L"1080p · 8 Mbps", L"1440p · 16 Mbps"}) {
                    SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                }
                SendMessageW(quality_, CB_SETCURSEL, internet_ ? 0 : 1, 0);
            }
            placeholder_ = Control(blank_class, L"", 0, 0);
            surface_ = Control(screen_class, L"", 0, 0);
            SetWindowLongPtrW(surface_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(surface_, SW_HIDE);
            overlay_toggle_ = Control(L"BUTTON", L"诊断", BS_AUTOCHECKBOX | WS_TABSTOP, overlay_toggle);
            SendMessageW(overlay_toggle_, BM_SETCHECK, BST_CHECKED, 0);
            overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT, overlay_class, L"",
                WS_CHILD | WS_DISABLED, 0, 0, 0, 0, surface_, nullptr, instance_, nullptr);
            winrt::check_pointer(overlay_);
            winrt::check_bool(SetLayeredWindowAttributes(overlay_, 0, 226, LWA_ALPHA));
        }

        Layout();
        Update();
        winrt::check_bool(SetTimer(window_, statistics_timer, 1000, nullptr) != 0);
        const std::stop_callback close(lifetime_, [window = window_] { PostMessageW(window, WM_CLOSE, 0, 0); });
        ShowWindow(window_, show);
        if (!lifetime_.stop_requested() && ((host_ && quick_connect_) || connect_on_open_)) Start();

        for (;;) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
                if (!IsDialogMessageW(window_, &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            const auto event = session_.Event();
            const auto capture = session_.CaptureEvent();
            const auto present = session_.PresentEvent();
            const std::array<HANDLE, 2> handles{event, capture ? capture : present};
            const DWORD count = !event ? 0 : handles[1] ? 2 : 1;
            const auto result = MsgWaitForMultipleObjectsEx(count, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_FAILED) winrt::throw_last_error();
            try {
                if (count && result == WAIT_OBJECT_0) {
                    const auto before = session_.State();
                    const auto previous_code = session_.Code();
                    session_.ProcessEvents();
                    const auto input_changed = RefreshInputReady();
                    if (!host_) RefreshLocalCursor();
                    if (input_pad_window_) {
                        for (const auto& input : session_.TakeInput()) input_pad_.Apply(input);
                    }
                    const auto new_stats = Measure();
                    if (new_stats || input_changed || before != session_.State() || previous_code != session_.Code()
                            || before == StreamState::waiting) {
                        Update();
                    }
                } else if (count == 2 && result == WAIT_OBJECT_0 + 1) {
                    if (capture) {
                        session_.Capture();
                        if (!IsActive(session_.State())) Update();
                    } else {
                        session_.Present();
                    }
                }
            } catch (...) {
                HandleError();
            }
        }
    }

private:
    static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* window = reinterpret_cast<StreamWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (window) {
            try { return window->OnSurfaceMessage(hwnd, message, wparam, lparam); }
            catch (...) { window->HandleError(); return 0; }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* window = static_cast<StreamWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            window->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        }
        auto* window = reinterpret_cast<StreamWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (window) {
            try { return window->OnMessage(hwnd, message, wparam, lparam); }
            catch (...) { window->HandleError(); return 0; }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == quality_select && HIWORD(wparam) == CBN_SELCHANGE) {
                const auto index = SendMessageW(quality_, CB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < quality_levels.size()) {
                    session_.SetQuality(quality_levels[static_cast<std::size_t>(index)]);
                }
                return 0;
            }
            if (HIWORD(wparam) != BN_CLICKED) break;
            switch (LOWORD(wparam)) {
            case connect_button: Start(); return 0;
            case allow_button: session_.Approve(); Update(); return 0;
            case allow_input_button:
                session_.Approve(quick_connect_ ? StreamInputMode::desktop : StreamInputMode::test_pad);
                Update();
                return 0;
            case stop_button: session_.Stop(L"已断开"); Update(); return 0;
            case export_button: Export(); return 0;
            case overlay_toggle: UpdateOverlay(session_.Statistics()); return 0;
            }
            break;
        case WM_SIZE: Layout(); return 0;
        case WM_DPICHANGED: {
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_GETMINMAXINFO:
            if (!host_) {
                auto& info = *reinterpret_cast<MINMAXINFO*>(lparam);
                info.ptMinTrackSize = {MulDiv(590, GetDpiForWindow(hwnd), 96), MulDiv(340, GetDpiForWindow(hwnd), 96)};
            }
            return 0;
        case WM_DISPLAYCHANGE:
            session_.DisplayChanged();
            if (session_.DesktopControl()) session_.Stop(L"显示配置已改变，已停止整桌面控制");
            if (host_ && screens_ && !IsActive(session_.State())) { RefreshScreens(); Update(); }
            break;
        case WM_HOTKEY:
            if (wparam == stop_control_hotkey && session_.DesktopControl()) {
                session_.Stop(L"已用本机快捷键停止整桌面控制");
                Update();
            }
            return 0;
        case WM_TIMER:
            if (wparam == statistics_timer) {
                session_.Tick();
                Update();
            } else if (wparam == cursor_timer) {
                session_.PollCursor();
            } else if (wparam == input_timer) {
                if (input_pad_window_) input_pad_.Expire();
                if (!host_ && (buttons_.any() || keys_.any()) && !session_.SubmitInput({InputKind::heartbeat})) {
                    ReleaseViewerInput();
                }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, statistics_timer);
            KillTimer(hwnd, input_timer);
            KillTimer(hwnd, cursor_timer);
            if (hotkey_) UnregisterHotKey(hwnd, stop_control_hotkey);
            session_.Stop(L"已关闭");
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            window_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void RefreshLocalCursor() {
        HCURSOR cursor{};
        POINT point{};
        if (GetFocus() == surface_ && session_.LocalCursor(cursor) && GetCursorPos(&point)
                && (WindowFromPoint(point) == surface_ || GetCapture() == surface_) && ScreenToClient(surface_, &point)
                && PointInput(InputKind::move, 0, point, buttons_.any())) {
            SetCursor(cursor);
        }
    }

    LRESULT OnSurfaceMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            ReleaseViewerInput();
            break;
        case WM_CAPTURECHANGED:
            if (buttons_.any()) ReleaseViewerInput();
            break;
        }
        if (!input_ready_) return DefWindowProcW(hwnd, message, wparam, lparam);

        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        switch (message) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        case WM_SETCURSOR:
            if (quick_connect_ && GetFocus() == hwnd && LOWORD(lparam) == HTCLIENT) {
                POINT cursor{};
                if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)
                    && PointInput(InputKind::move, 0, cursor, buttons_.any())) {
                    HCURSOR shape{};
                    session_.LocalCursor(shape);
                    SetCursor(shape);
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN: SetButton(InputButton::left, true, point); return 0;
        case WM_LBUTTONUP: SetButton(InputButton::left, false, point); return 0;
        case WM_RBUTTONDOWN: SetButton(InputButton::right, true, point); return 0;
        case WM_RBUTTONUP: SetButton(InputButton::right, false, point); return 0;
        case WM_MBUTTONDOWN: SetButton(InputButton::middle, true, point); return 0;
        case WM_MBUTTONUP: SetButton(InputButton::middle, false, point); return 0;
        case WM_MOUSEMOVE:
            if (GetFocus() == hwnd) {
                if (const auto input = PointInput(InputKind::move, 0, point, buttons_.any())) {
                    session_.SubmitInput(*input);
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (GetFocus() == hwnd && ScreenToClient(hwnd, &point)) {
                const auto code = static_cast<std::uint16_t>(GET_WHEEL_DELTA_WPARAM(wparam));
                if (const auto input = PointInput(InputKind::wheel, code, point, buttons_.any())) SendControl(*input);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (GetFocus() == hwnd) SetKey(wparam, lparam, message == WM_KEYDOWN);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            if (quick_connect_ && GetFocus() == hwnd) {
                SetKey(wparam, lparam, message == WM_SYSKEYDOWN);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (quick_connect_ && GetFocus() == hwnd) return 0;
            break;
        case WM_CHAR:
            if (!quick_connect_ && GetFocus() == hwnd
                && (wparam == L'\b' || wparam == L'\r' || (wparam >= 0x20 && wparam != 0x7f))) {
                SendControl({InputKind::text, static_cast<std::uint16_t>(wparam)});
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::optional<InputPacket> PointInput(InputKind kind, std::uint16_t code, POINT point, bool clamp) const {
        RECT client{};
        GetClientRect(surface_, &client);
        const auto stats = session_.Statistics();
        const auto viewport = p2premote::FitVideo(client.right, client.bottom, stats.width, stats.height);
        if (!viewport.width || !viewport.height) return std::nullopt;
        const auto x = static_cast<int>(point.x) - static_cast<int>(viewport.left);
        const auto y = static_cast<int>(point.y) - static_cast<int>(viewport.top);
        const auto right = static_cast<int>(viewport.width) - 1;
        const auto bottom = static_cast<int>(viewport.height) - 1;
        if (!clamp && (x < 0 || y < 0 || x > right || y > bottom)) return std::nullopt;
        const auto normalized = [](int coordinate, int edge) {
            return static_cast<std::uint16_t>(MulDiv(std::clamp(coordinate, 0, edge), 65535, std::max(1, edge)));
        };
        return InputPacket{kind, code, normalized(x, right), normalized(y, bottom)};
    }

    void SetButton(InputButton button, bool down, POINT point) {
        const auto code = static_cast<std::uint16_t>(button);
        if (buttons_.test(code) == down) return;
        const auto input = PointInput(down ? InputKind::button_down : InputKind::button_up, code, point, buttons_.any());
        if (!input) {
            if (!down) ReleaseViewerInput();
            return;
        }
        if (down) {
            SetFocus(surface_);
            if (GetFocus() != surface_) return;
            if (quick_connect_) {
                HCURSOR cursor{};
                session_.LocalCursor(cursor);
                SetCursor(cursor);
            }
        }
        if (!SendControl(*input)) return;
        buttons_.set(code, down);
        if (down) SetCapture(surface_);
        else if (buttons_.none() && GetCapture() == surface_) ReleaseCapture();
    }

    void SetKey(WPARAM key, LPARAM flags, bool down) {
        if (key == (quick_connect_ ? VK_F12 : VK_ESCAPE)) {
            if (down) SetFocus(window_);
            return;
        }
        const auto code = static_cast<std::uint16_t>((flags >> 16) & 0x1ff);
        if (!code || (keys_.test(code) == down && !(quick_connect_ && down))) return;
        if (!quick_connect_ && down && (key == VK_LWIN || key == VK_RWIN || key == VK_CONTROL || key == VK_MENU
            || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)) return;
        if (SendControl({down ? InputKind::key_down : InputKind::key_up, code})) keys_.set(code, down);
    }

    bool SendControl(const InputPacket& input) {
        if (session_.SubmitInput(input)) return true;
        ReleaseViewerInput();
        return false;
    }

    void ReleaseViewerInput() {
        if (!surface_) return;
        buttons_.reset();
        keys_.reset();
        if (GetCapture() == surface_) ReleaseCapture();
        if (quick_connect_) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (input_ready_) session_.SubmitInput({InputKind::reset});
    }

    bool RefreshInputReady() {
        const auto ready = session_.InputReady();
        if (ready == input_ready_) return false;
        input_ready_ = ready;
        if (input_pad_window_) input_pad_.SetReady(ready);
        if (!host_) {
            if (!ready) ReleaseViewerInput();
            else session_.SubmitInput({InputKind::heartbeat});
        }
        if (!host_ || input_pad_window_) {
            if (ready) winrt::check_bool(SetTimer(window_, input_timer, p2premote::input_heartbeat_ms, nullptr) != 0);
            else KillTimer(window_, input_timer);
        }
        if (poll_cursor_) {
            if (ready) winrt::check_bool(SetTimer(window_, cursor_timer, 33, nullptr) != 0);
            else KillTimer(window_, cursor_timer);
        }
        return true;
    }

    HWND Control(const wchar_t* type, const wchar_t* label, DWORD style, int id) const {
        const auto handle = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        winrt::check_pointer(handle);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return handle;
    }

    void RefreshScreens() {
        monitors_.clear();
        winrt::check_bool(EnumDisplayMonitors(nullptr, nullptr, AddScreen, reinterpret_cast<LPARAM>(&monitors_)));
        SendMessageW(screens_, CB_RESETCONTENT, 0, 0);
        for (std::size_t index = 0; index < monitors_.size(); ++index) {
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(monitors_[index], &info);
            const auto& rect = info.rcMonitor;
            const auto text = std::format(L"显示器 {} · {} × {}", index + 1, rect.right - rect.left, rect.bottom - rect.top);
            SendMessageW(screens_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        if (!monitors_.empty()) SendMessageW(screens_, CB_SETCURSEL, 0, 0);
    }

    void Start() {
        if (IsActive(session_.State())) return;
        HMONITOR monitor{};
        auto frame_rate = StreamFrameRate::fps30;
        std::array<wchar_t, 13> code{};
        if (host_) {
            const auto index = SendMessageW(screens_, CB_GETCURSEL, 0, 0);
            if (index < 0 || static_cast<std::size_t>(index) >= monitors_.size()) return;
            monitor = monitors_[static_cast<std::size_t>(index)];
            if (SendMessageW(low_latency_, BM_GETCHECK, 0, 0) == BST_CHECKED) frame_rate = StreamFrameRate::fps60;
            if (input_pad_window_) input_pad_.BeginSession(monitor);
        } else if (!quick_connect_ && GetWindowTextW(code_, code.data(), static_cast<int>(code.size())) != 12) {
            session_.Stop(L"请输入 Host 显示的 12 位会话代码");
            Update();
            return;
        }
        previous_ = {};
        rate_text_.clear();
        session_.Start(monitor, surface_, code.data(), frame_rate);
        Update();
    }

    void Layout() {
        if (!status_) return;
        const auto dpi = GetDpiForWindow(window_);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const auto place = [&](HWND child, int x, int y, int width, int height) {
            MoveWindow(child, px(x), px(y), px(width), px(height), TRUE);
        };
        place(code_label_, 10, 13, 70, 20);
        place(code_, 80, 9, 152, 24);
        const auto actions = quick_connect_ ? 10 : 240;
        const auto connect_width = internet_ ? 116 : 88;
        place(connect_, actions, 9, connect_width, 24);
        place(stop_, actions + connect_width + 8, 9, 65, 24);
        place(export_, actions + connect_width + 82, 9, 91, 24);
        if (quality_) {
            const auto quality_x = actions + connect_width + 185;
            place(quality_label_, quality_x, 13, 34, 20);
            place(quality_, quality_x + 38, 9, 140, 124);
        }
        RECT rect{};
        GetClientRect(window_, &rect);
        if (host_) {
            place(screens_, 10, 44, 222, 220);
            place(allow_, 240, 42, 88, 25);
            place(allow_input_, 336, 42, 153, 25);
            place(low_latency_, 501, 42, 147, 25);
            MoveWindow(status_, px(10), px(82), rect.right - px(20), px(quick_connect_ ? 105 : 75), TRUE);
            if (input_pad_window_) {
                MoveWindow(input_pad_window_, px(10), px(164), std::max(1L, rect.right - px(20)),
                    std::max(1L, rect.bottom - px(174)), TRUE);
            }
        } else {
            const auto width = std::max(0L, rect.right - px(20));
            const auto height = std::max(0L, rect.bottom - px(176));
            MoveWindow(overlay_toggle_, rect.right - px(70), px(9), px(60), px(24), TRUE);
            MoveWindow(surface_, px(10), px(42), width, height, TRUE);
            MoveWindow(placeholder_, px(10), px(42), width, height, TRUE);
            MoveWindow(status_panel_, px(10), px(46) + height, width, px(122), TRUE);
            MoveWindow(status_, px(20), px(66) + height, std::max(0L, width - px(20)), px(96), TRUE);
            session_.Resize(width > 0 && height > 0 && !IsIconic(window_));
            UpdateOverlay(session_.Statistics());
        }
    }

    void UpdateOverlay(const StreamStatistics& stats) {
        if (!overlay_) return;
        RECT area{};
        GetClientRect(surface_, &area);
        const auto dpi = GetDpiForWindow(window_);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const auto margin = px(8);
        const auto width = px(224);
        const auto height = px(116);
        if (session_.State() != StreamState::streaming || !stats.width || !stats.height || IsIconic(window_)
            || SendMessageW(overlay_toggle_, BM_GETCHECK, 0, 0) != BST_CHECKED
            || area.right < width + 2 * margin || area.bottom < height + 2 * margin) {
            ShowWindow(overlay_, SW_HIDE);
            return;
        }
        const auto viewport = p2premote::FitVideo(area.right, area.bottom, stats.width, stats.height);
        const bool fits_video = viewport.width >= static_cast<unsigned>(width + 2 * margin)
            && viewport.height >= static_cast<unsigned>(height + 2 * margin);
        const auto right = fits_video ? static_cast<int>(viewport.left + viewport.width) : area.right;
        const auto top = fits_video ? static_cast<int>(viewport.top) : 0;
        SetWindowPos(overlay_, HWND_TOP, right - margin - width, top + margin, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);

        const auto seconds = static_cast<std::uint64_t>(stats.seconds);
        const auto fps = presented_fps_ ? std::format(L"{:.1f}", *presented_fps_) : std::wstring(L"—");
        const auto loss = loss_rate_ ? std::format(L"{:.1f}%", *loss_rate_) : std::wstring(L"—");
        const auto& protocol = stats.relay_protocol.empty() ? stats.ice_protocol : stats.relay_protocol;
        auto text = std::format(L"{:02}:{:02}:{:02}\r\n{} · {}\r\n{} fps · {} × {}\r\n"
                                L"↓ {}   ↑ {}\r\nRTT {} · 丢包 {}\r\n视频缓冲 {}",
            seconds / 3600, seconds / 60 % 60, seconds % 60, NetworkPath(stats.ice_path), NetworkProtocol(protocol),
            fps, stats.width, stats.height, down_rate_.empty() ? L"—" : down_rate_,
            up_rate_.empty() ? L"—" : up_rate_, Milliseconds(stats.ice_rtt_ms), loss, Milliseconds(stats.jitter_buffer_ms));
        if (text != visible_overlay_) {
            SetWindowTextW(overlay_, text.c_str());
            visible_overlay_ = std::move(text);
            InvalidateRect(overlay_, nullptr, FALSE);
        }
    }

    bool Measure() {
        if (session_.State() != StreamState::streaming) return false;
        const auto next = session_.Statistics();
        const auto interval = next.seconds - previous_.seconds;
        if (interval <= 0) return false;
        const auto rate = [interval](std::uint64_t now, std::uint64_t before) {
            return now >= before ? static_cast<double>(now - before) / interval : 0.0;
        };
        if (!host_) presented_fps_ = rate(next.presented, previous_.presented);
        rate_text_ = host_
            ? std::format(L"采集 {:.1f} fps  |  提交 {:.1f} fps  |  编码 {:.1f} fps",
                rate(next.captured, previous_.captured), rate(next.submitted, previous_.submitted), rate(next.encoded, previous_.encoded))
            : std::format(L"接收 {:.1f} fps  |  解码 {:.1f} fps  |  呈现 {:.1f} fps",
                rate(next.received, previous_.received), rate(next.decoded, previous_.decoded), *presented_fps_);
        if (previous_.seconds > 0) {
            const auto bandwidth = [](double bytes_per_second) {
                const auto kbps = bytes_per_second * 8 / 1000;
                return kbps >= 1000 ? std::format(L"{:.2f} Mbps", kbps / 1000) : std::format(L"{:.1f} Kbps", kbps);
            };
            up_rate_ = bandwidth(rate(next.bytes_sent, previous_.bytes_sent));
            down_rate_ = bandwidth(rate(next.bytes_received, previous_.bytes_received));
            rate_text_ += std::format(L"  |  上行 {}  下行 {}", up_rate_, down_rate_);
            loss_rate_.reset();
            if (next.video_packets_received >= previous_.video_packets_received) {
                const auto received = next.video_packets_received - previous_.video_packets_received;
                const auto lost = std::max<std::int64_t>(0, next.video_packets_lost - previous_.video_packets_lost);
                const auto total = static_cast<double>(received) + static_cast<double>(lost);
                if (total > 0) loss_rate_ = 100.0 * static_cast<double>(lost) / total;
            }
        }
        previous_ = next;
        return true;
    }

    void Update() {
        RefreshInputReady();
        const auto state = session_.State();
        if (state == StreamState::waiting || state == StreamState::reconnecting) {
            previous_ = {};
            rate_text_.clear();
            up_rate_.clear();
            down_rate_.clear();
            presented_fps_.reset();
            loss_rate_.reset();
        }
        const auto active = IsActive(state);
        const auto stats = session_.Statistics();
        if (session_.DesktopControl() && !hotkey_) {
            hotkey_ = RegisterHotKey(window_, stop_control_hotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11);
        } else if (!session_.DesktopControl() && hotkey_) {
            UnregisterHotKey(window_, stop_control_hotkey);
            hotkey_ = false;
        }
        EnableWindow(connect_, !active && (!host_ || !monitors_.empty()));
        EnableWindow(stop_, active);
        EnableWindow(export_, !active && (stats.seconds > 0 || stats.ice_diagnostics.has_value()));
        if (host_) {
            auto code = session_.Code();
            if (code != visible_code_) {
                SetWindowTextW(code_, code.c_str());
                visible_code_ = std::move(code);
            }
            EnableWindow(screens_, !active);
            EnableWindow(low_latency_, !active);
            EnableWindow(allow_, state == StreamState::approval);
            EnableWindow(allow_input_, state == StreamState::approval);
        } else {
            EnableWindow(code_, !active);
            const auto show_video = state == StreamState::streaming;
            if (show_video != video_visible_) {
                // Hiding the HWND also hides the last frame retained by the compositor.
                ShowWindow(surface_, show_video ? SW_SHOWNA : SW_HIDE);
                ShowWindow(placeholder_, show_video ? SW_HIDE : SW_SHOWNA);
                video_visible_ = show_video;
            }
            UpdateOverlay(stats);
        }
        const auto idle = internet_ ? (host_ ? L"等待公网请求" : L"点击连接公网 Host")
            : quick_connect_ ? (host_ ? L"点击等待直连" : L"点击一键直连")
                             : (host_ ? L"选择要共享的显示器并创建会话" : L"输入 Host 的会话代码");
        auto text = state == StreamState::idle ? idle : session_.Status();
        if (state == StreamState::streaming) {
            if (!host_) text += L"\r\n";
            if (!host_ || stats.ice_path != IcePath::unknown) {
                if (host_) text += L"  |  ";
                text += NetworkPath(stats.ice_path);
            }
            if (!stats.ice_protocol.empty()) text += std::format(L"  |  {}", NetworkProtocol(stats.ice_protocol));
            if (!stats.relay_protocol.empty() && stats.relay_protocol != stats.ice_protocol) {
                text += std::format(L"  |  本端到 TURN {}", NetworkProtocol(stats.relay_protocol));
            }
            if (stats.ice_rtt_ms) text += std::format(L"  |  RTT {:.1f} ms", *stats.ice_rtt_ms);
            if (!host_) {
                text += loss_rate_ ? std::format(L"  |  视频丢包 {:.1f}%", *loss_rate_) : L"  |  视频丢包 —";
                text += L"\r\n";
            }
        }
        if (state == StreamState::streaming && stats.width) {
            if (!stats.video_codec.empty()) {
                if (host_) text += L"  |  ";
                text += stats.video_codec + (stats.hardware_video ? L" 硬件" : L"");
                if (stats.gpu_conversion) text += L" · GPU 转换";
                if (stats.gpu_backbuffer) text += L" · 直写上屏";
            }
            text += std::format(L"  |  {} × {}  |  {} 帧\r\n{}\r\n", stats.width, stats.height,
                host_ ? stats.submitted : stats.presented, rate_text_);
            text += host_ ? std::format(L"平均采集至提交 {:.2f} ms  |  编码 {:.2f} ms", stats.capture_to_submit_ms, stats.encode_ms)
                          : std::format(L"平均解码 {:.2f} ms  |  绘制至提交 {:.2f} ms", stats.decode_ms, stats.draw_to_present_ms);
            const auto ms = [](std::optional<double> value) {
                return value ? std::format(L"{:.2f} ms", *value) : std::wstring(L"—");
            };
            text += host_ ? std::format(L"\r\n平均发送排队 {}", ms(stats.packet_send_queue_ms))
                          : std::format(L"  |  缓冲 {}  |  解码后等待 {}",
                                ms(stats.jitter_buffer_ms), ms(stats.decoded_frame_wait_ms));
        }
        if (!host_ && input_ready_) {
            HCURSOR cursor{};
            if (session_.LocalCursor(cursor)) text += L"\r\n本地光标 · 点击画面接管键鼠，按 F12 释放。";
            else text += quick_connect_ ? L"\r\n画面同步光标 · 点击画面接管键鼠，按 F12 释放。"
                                        : L"\r\n点击串流画面进入键鼠测试，按 Esc 退出。";
        }
        if (session_.DesktopControl()) text += hotkey_ ? L"\r\n本机 Ctrl+Alt+F11 停止整桌面控制。"
                                                      : L"\r\n点击断开以停止整桌面控制。";
        if (text != visible_status_) {
            SetWindowTextW(status_, text.c_str());
            visible_status_ = std::move(text);
        }
    }

    void Export() {
        std::array<wchar_t, MAX_PATH> file{};
        wcscpy_s(file.data(), file.size(), host_ ? L"host-stream.csv" : L"viewer-stream.csv");
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0";
        dialog.lpstrFile = file.data();
        dialog.nMaxFile = static_cast<DWORD>(file.size());
        dialog.lpstrDefExt = L"csv";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) session_.ExportCsv(file.data());
    }

    void HandleError() noexcept {
        try {
            try { throw; }
            catch (const winrt::hresult_error& error) { session_.Stop(error.message().c_str()); }
            catch (const std::exception& error) { session_.Stop(winrt::to_hstring(error.what()).c_str()); }
            Update();
        } catch (...) {
            SetWindowTextW(status_, L"串流发生错误");
        }
    }

    HINSTANCE instance_;
    bool host_;
    bool internet_;
    bool quick_connect_;
    bool quality_selection_;
    bool poll_cursor_;
    bool connect_on_open_;
    std::stop_token lifetime_;
    p2premote::StreamSession session_;
    HWND window_{};
    HWND screens_{};
    HWND code_label_{};
    HWND code_{};
    HWND connect_{};
    HWND allow_{};
    HWND allow_input_{};
    HWND low_latency_{};
    HWND quality_label_{};
    HWND quality_{};
    HWND stop_{};
    HWND export_{};
    HWND status_panel_{};
    HWND status_{};
    HWND surface_{};
    HWND placeholder_{};
    HWND overlay_toggle_{};
    HWND overlay_{};
    HWND input_pad_window_{};
    InputTestPad input_pad_;
    bool video_visible_{};
    bool input_ready_{};
    bool hotkey_{};
    std::bitset<4> buttons_;
    std::bitset<512> keys_;
    std::vector<HMONITOR> monitors_;
    StreamStatistics previous_;
    std::wstring visible_code_;
    std::wstring visible_status_;
    std::wstring visible_overlay_;
    std::wstring rate_text_;
    std::wstring up_rate_;
    std::wstring down_rate_;
    std::optional<double> presented_fps_;
    std::optional<double> loss_rate_;
};

} // namespace

StreamLaunchOptions ReadStreamOptions(bool host) {
    int count{};
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(CommandLineToArgvW(GetCommandLineW(), &count), LocalFree);
    winrt::check_pointer(arguments.get());
    StreamLaunchOptions options;
    bool test_playout_delay{};
    bool test_present_immediate{};
    bool hardware_video{};
    std::filesystem::path internet_file;
    std::filesystem::path latency_trace_dir;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view arg(arguments.get()[index]);
        if (arg == L"--share" && host && !options.share) {
            options.share = true;
        } else if (arg == L"--direct" && options.connection.mode == ConnectionMode::session_code) {
            options.connection.mode = ConnectionMode::lan_direct;
        } else if (arg == L"--auto-control" && host && !options.connection.auto_control) {
            options.connection.auto_control = true;
        } else if (arg == L"--hardware-video" && !hardware_video) {
            hardware_video = true;
        } else if (arg == L"--fixed-cadence" && host && !options.connection.fixed_cadence) {
            options.connection.fixed_cadence = true;
        } else if (arg == L"--latency-trace" && latency_trace_dir.empty() && index + 1 < count) {
            latency_trace_dir = arguments.get()[++index];
            if (latency_trace_dir.empty()) throw std::invalid_argument("--latency-trace 需要输出目录");
        } else if (arg == L"--capture-dxgi" && host && !options.connection.desktop_duplication) {
            options.connection.desktop_duplication = true;
        } else if (arg == L"--video-cursor" && !host && !options.connection.video_cursor) {
            options.connection.video_cursor = true;
        } else if (arg == L"--test-playout-delay" && !host && !test_playout_delay) {
            test_playout_delay = true;
        } else if (arg == L"--test-present-immediate" && !host && !test_present_immediate) {
            test_present_immediate = true;
        } else if (arg == L"--internet" && internet_file.empty() && index + 1 < count) {
            internet_file = arguments.get()[++index];
            if (internet_file.empty()) throw std::invalid_argument("公网连接需要配置文件");
        } else if (arg == L"--signal" && options.connection.signal_url.empty() && index + 1 < count) {
            const winrt::Windows::Foundation::Uri uri(arguments.get()[++index]);
            if ((uri.SchemeName() != L"ws" && uri.SchemeName() != L"wss") || uri.Host().empty()
                || uri.Path() != L"/signal" || !uri.UserName().empty() || !uri.Password().empty()
                || !uri.Query().empty() || !uri.Fragment().empty()) {
                throw std::invalid_argument("信令地址应为 ws://主机:端口/signal 或 wss://主机:端口/signal");
            }
            options.connection.signal_url = uri.AbsoluteUri().c_str();
        } else {
            throw std::invalid_argument("请检查启动参数，公网模式用 --internet 配置文件");
        }
    }
    if (options.connection.desktop_duplication && (options.connection.mode != ConnectionMode::lan_direct || !options.share)) {
        throw std::invalid_argument("--capture-dxgi 需要与 --share 和 --direct 同时使用");
    }
    if (options.connection.fixed_cadence && (options.connection.mode != ConnectionMode::lan_direct || !options.share)) {
        throw std::invalid_argument("--fixed-cadence 需要与 --share 和 --direct 同时使用");
    }
    if (options.connection.video_cursor && (options.connection.mode != ConnectionMode::lan_direct || !internet_file.empty())) {
        throw std::invalid_argument("--video-cursor 需要局域网直连模式");
    }
    if (!internet_file.empty()) {
        if (!options.connection.signal_url.empty() || options.connection.mode != ConnectionMode::session_code
                || options.connection.auto_control) {
            throw std::invalid_argument("--internet 配置文件已指定公网连接方式");
        }
        options.connection = p2premote::ReadInternetConfig(internet_file);
    }
    if (host && !options.connection.signal_url.empty() && !options.share) {
        throw std::invalid_argument("Host 使用网络信令时请同时传入 --share");
    }
    if (options.connection.mode == ConnectionMode::lan_direct && options.connection.signal_url.empty()) {
        throw std::invalid_argument("直连需要通过 --signal 指定局域网信令地址");
    }
    if (options.connection.auto_control && (!options.share || options.connection.mode != ConnectionMode::lan_direct)) {
        throw std::invalid_argument("--auto-control 需要与 --share 和 --direct 同时使用");
    }
    options.connection.test_playout_delay = test_playout_delay;
    options.connection.test_present_immediate = test_present_immediate;
    options.connection.hardware_video = hardware_video;
    options.connection.latency_trace_dir = std::move(latency_trace_dir);
    return options;
}

int RunStreamWindow(HINSTANCE instance, int show, bool host, StreamLaunchOptions options) {
    return StreamWindow(instance, host, std::move(options)).Run(show);
}
