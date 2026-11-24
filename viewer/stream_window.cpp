#include "stream_window.h"
#include "connection_overlay.h"
#include "viewer_input.h"

#include <p2premote/stream_session.h>
#include <p2premote/video_viewport.h>

#include <commdlg.h>
#include <windowsx.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using p2premote::IcePath;
using p2premote::StreamFrameRate;
using p2premote::StreamInputMode;
using p2premote::StreamQuality;
using p2premote::StreamState;
using p2premote::StreamStatistics;
using p2premote::VideoBitratePreset;

enum ControlId {
    screen_select = 200, connect_button, allow_button,
    stop_button, export_button, status_text, allow_input_button, low_latency_mode, quality_select, status_panel,
    overlay_toggle, bitrate_select, plugin_button
};
constexpr std::array quality_levels{StreamQuality::p720, StreamQuality::p1080, StreamQuality::p1440};
constexpr std::array quality_labels{L"720p", L"1080p", L"1440p"};
constexpr std::array bitrate_presets{VideoBitratePreset::economy, VideoBitratePreset::automatic, VideoBitratePreset::detail};
constexpr std::array bitrate_labels{L"省流", L"自动", L"清晰"};
constexpr UINT_PTR statistics_timer = 1;
constexpr UINT_PTR input_timer = 2;
constexpr UINT_PTR cursor_timer = 3;
constexpr int stop_control_hotkey = 1;
constexpr wchar_t stream_window_class[] = L"P2PRemote.Stream";
constexpr wchar_t screen_class[] = L"P2PRemote.VideoSurface";
constexpr wchar_t blank_class[] = L"P2PRemote.BlankSurface";

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
        : instance_(instance), host_(host),
          poll_cursor_(host || !options.connection.video_cursor),
          connect_on_open_(options.connect_on_open),
          lifetime_(options.lifetime),
          session_(host, std::move(options.connection)), input_(session_) {}
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

        });

        const auto dpi = GetDpiForSystem();
        const auto width = MulDiv(host_ ? 680 : 960, dpi, 96);
        const auto height = MulDiv(host_ ? 230 : 660, dpi, 96);
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
            allow_input_ = Control(L"BUTTON", L"允许整桌面控制", BS_PUSHBUTTON | WS_TABSTOP, allow_input_button);
            low_latency_ = Control(L"BUTTON", L"低延迟 60 fps", BS_AUTOCHECKBOX | WS_TABSTOP, low_latency_mode);
            SendMessageW(low_latency_, BM_SETCHECK, BST_CHECKED, 0);
        }
        connect_ = Control(L"BUTTON", host_ ? L"等待连接" : L"连接设备", BS_PUSHBUTTON | WS_TABSTOP, connect_button);
        stop_ = Control(L"BUTTON", L"断开", BS_PUSHBUTTON | WS_TABSTOP, stop_button);
        export_ = Control(L"BUTTON", L"导出 CSV", BS_PUSHBUTTON | WS_TABSTOP, export_button);
        if (!host_) status_panel_ = Control(L"BUTTON", L"连接状态", BS_GROUPBOX, status_panel);
        status_ = Control(L"STATIC", L"", SS_LEFT, status_text);
        if (!host_) {
            plugins_ = Control(L"BUTTON", L"插件", BS_PUSHBUTTON | WS_TABSTOP, plugin_button);
            quality_label_ = Control(L"STATIC", L"分辨率", SS_LEFT, 0);
            quality_ = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, quality_select);
            for (const auto* label : quality_labels) {
                SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            }
            SendMessageW(quality_, CB_SETCURSEL, 1, 0);
            bitrate_label_ = Control(L"STATIC", L"画质", SS_LEFT, 0);
            bitrate_ = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, bitrate_select);
            for (std::size_t i = 0; i < bitrate_presets.size(); ++i) {
                const auto label = std::format(L"{} · {} Mbps", bitrate_labels[i],
                    p2premote::VideoBitrateLimit(bitrate_presets[i]) / 1'000'000);
                SendMessageW(bitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(bitrate_, CB_SETCURSEL, 1, 0);
            placeholder_ = Control(blank_class, L"", 0, 0);
            surface_ = Control(screen_class, L"", 0, 0);
            input_.Attach(surface_);
            SetWindowLongPtrW(surface_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(surface_, SW_HIDE);
            overlay_toggle_ = Control(L"BUTTON", L"诊断", BS_AUTOCHECKBOX | WS_TABSTOP, overlay_toggle);
            SendMessageW(overlay_toggle_, BM_SETCHECK, BST_CHECKED, 0);
            overlay_ = p2premote::viewer::CreateConnectionOverlay(surface_);
            winrt::check_pointer(overlay_);
        }

        Layout();
        Update();
        winrt::check_bool(SetTimer(window_, statistics_timer, 1000, nullptr) != 0);
        const std::stop_callback close(lifetime_, [window = window_] { PostMessageW(window, WM_CLOSE, 0, 0); });
        ShowWindow(window_, show);
        if (!lifetime_.stop_requested() && (host_ || connect_on_open_)) Start();

        for (;;) {
            MSG message{};
            for (int drained = 0; drained < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++drained) {
                if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
                if (!IsDialogMessageW(window_, &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            try {
                input_.FlushPendingMove();
                if (!host_) TryPresent();
            } catch (...) {
                HandleError();
            }
            const auto event = session_.Event();
            const auto capture = session_.CaptureEvent();
            const auto present = session_.PresentEvent();
            const auto media = capture ? capture : present;
            std::array<HANDLE, 2> handles{};
            DWORD count = 0;
            int media_index = -1;
            int event_index = -1;
            if (media) {
                media_index = static_cast<int>(count);
                handles[count++] = media;
            }
            if (event) {
                event_index = static_cast<int>(count);
                handles[count++] = event;
            }
            const auto timeout = input_.PendingMoveTimeoutMs();
            const auto result = MsgWaitForMultipleObjectsEx(count, handles.data(), timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_FAILED) winrt::throw_last_error();
            try {
                if (result == WAIT_TIMEOUT) {
                    input_.FlushPendingMove();
                } else if (media_index >= 0 && result == WAIT_OBJECT_0 + static_cast<DWORD>(media_index)) {
                    if (capture) {
                        session_.Capture();
                        if (!IsActive(session_.State())) Update();
                    } else {
                        session_.Present();
                    }
                    if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0) {
                        ProcessSessionEvents();
                    }
                } else if (event_index >= 0 && result == WAIT_OBJECT_0 + static_cast<DWORD>(event_index)) {
                    ProcessSessionEvents();
                    if (!host_) {
                        TryPresent();
                    } else if (const auto next_capture = session_.CaptureEvent();
                            next_capture && WaitForSingleObject(next_capture, 0) == WAIT_OBJECT_0) {
                        session_.Capture();
                        if (!IsActive(session_.State())) Update();
                    }
                }
                input_.FlushPendingMove();
            } catch (...) {
                HandleError();
            }
        }
    }

private:
    static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* window = reinterpret_cast<StreamWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (window) {
            try {
                if (const auto result = window->input_.OnSurfaceMessage(hwnd, message, wparam, lparam)) return *result;
            }
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
            if (LOWORD(wparam) == bitrate_select && HIWORD(wparam) == CBN_SELCHANGE) {
                const auto index = SendMessageW(bitrate_, CB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < bitrate_presets.size()) {
                    session_.SetVideoBitratePreset(bitrate_presets[static_cast<std::size_t>(index)]);
                }
                return 0;
            }
            if (HIWORD(wparam) != BN_CLICKED) break;
            switch (LOWORD(wparam)) {
            case connect_button: Start(); return 0;
            case allow_button: session_.Approve(StreamInputMode::view_only); Update(); return 0;
            case allow_input_button: session_.Approve(StreamInputMode::desktop); Update(); return 0;
            case stop_button: session_.Stop(L"已断开"); Update(); return 0;
            case export_button: Export(); return 0;
            case overlay_toggle: UpdateOverlay(session_.Statistics()); return 0;
            case plugin_button: ShowPluginMenu(); return 0;
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
                info.ptMinTrackSize = {MulDiv(770, GetDpiForWindow(hwnd), 96), MulDiv(340, GetDpiForWindow(hwnd), 96)};
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
                input_.Heartbeat();
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

    void ProcessSessionEvents() {
        const auto before = session_.State();
        const auto had_menu = !host_ && session_.HasPluginMenu();
        const std::wstring previous_notice = !host_ ? std::wstring(session_.PluginNotice()) : std::wstring{};
        session_.ProcessEvents();
        const auto input_changed = RefreshInputReady();
        if (!host_) input_.RefreshLocalCursor();
        const auto new_stats = Measure();
        const auto plugin_ui_changed = !host_ && (had_menu != session_.HasPluginMenu() || previous_notice != session_.PluginNotice());
        if (new_stats || input_changed || plugin_ui_changed || before != session_.State() || before == StreamState::waiting) {
            Update();
        }
    }

    void ShowPluginMenu() {
        const auto entries = session_.PluginMenu();
        if (entries.empty()) return;
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        for (const auto& item : entries) {
            AppendMenuW(menu, MF_STRING | (item.checked.value_or(false) ? MF_CHECKED : MF_UNCHECKED), item.id, item.label.c_str());
        }
        RECT button{};
        GetWindowRect(plugins_, &button);
        const auto command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            button.left, button.bottom, window_, nullptr);
        DestroyMenu(menu);
        if (command) {
            p2premote::ViewerInput::UpdatePluginLocks(session_);
            session_.InvokePluginMenu(command);
        }
    }

    void TryPresent() {
        if (const auto present = session_.PresentEvent();
                present && WaitForSingleObject(present, 0) == WAIT_OBJECT_0) {
            session_.Present();
        }
    }

    bool RefreshInputReady() {
        const auto ready = session_.InputReady();
        if (host_ ? ready == input_ready_ : !input_.RefreshReady()) return false;
        input_ready_ = ready;
        if (!host_) {
            if (ready) winrt::check_bool(SetTimer(window_, input_timer, p2premote::input_heartbeat_ms, nullptr) != 0);
            else KillTimer(window_, input_timer);
        }
        if (poll_cursor_) {
            if (ready) {
                session_.PollCursor();
                winrt::check_bool(SetTimer(window_, cursor_timer, 33, nullptr) != 0);
            } else {
                KillTimer(window_, cursor_timer);
            }
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
        if (host_) {
            const auto index = SendMessageW(screens_, CB_GETCURSEL, 0, 0);
            if (index < 0 || static_cast<std::size_t>(index) >= monitors_.size()) return;
            monitor = monitors_[static_cast<std::size_t>(index)];
            if (SendMessageW(low_latency_, BM_GETCHECK, 0, 0) == BST_CHECKED) frame_rate = StreamFrameRate::fps60;
        }
        previous_ = {};
        input_.Reset();
        rate_text_.clear();
        session_.Start(monitor, surface_, frame_rate);
        Update();
    }

    void Layout() {
        if (!status_) return;
        const auto dpi = GetDpiForWindow(window_);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const auto place = [&](HWND child, int x, int y, int width, int height) {
            MoveWindow(child, px(x), px(y), px(width), px(height), TRUE);
        };
        constexpr int connect_width = 96;
        place(connect_, 10, 9, connect_width, 24);
        place(stop_, 10 + connect_width + 8, 9, 65, 24);
        place(export_, 10 + connect_width + 82, 9, 91, 24);
        if (quality_) {
            const auto quality_x = 10 + connect_width + 185;
            place(quality_label_, quality_x, 13, 46, 20);
            place(quality_, quality_x + 48, 9, 80, 124);
            place(bitrate_label_, quality_x + 138, 13, 35, 20);
            place(bitrate_, quality_x + 176, 9, 116, 124);
        }
        RECT rect{};
        GetClientRect(window_, &rect);
        if (host_) {
            place(screens_, 10, 44, 222, 220);
            place(allow_, 240, 42, 88, 25);
            place(allow_input_, 336, 42, 153, 25);
            place(low_latency_, 501, 42, 147, 25);
            MoveWindow(status_, px(10), px(82), rect.right - px(20), px(105), TRUE);
        } else {
            const auto width = std::max(0L, rect.right - px(20));
            const auto height = std::max(0L, rect.bottom - px(176));
            MoveWindow(overlay_toggle_, rect.right - px(70), px(9), px(60), px(24), TRUE);
            MoveWindow(plugins_, rect.right - px(138), px(9), px(60), px(24), TRUE);
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
        if (session_.State() != StreamState::streaming || IsIconic(window_)
            || SendMessageW(overlay_toggle_, BM_GETCHECK, 0, 0) != BST_CHECKED) {
            ShowWindow(overlay_, SW_HIDE);
            return;
        }
        p2premote::viewer::PlaceConnectionOverlay(overlay_, stats.width, stats.height, 200, 174);

        const auto seconds = static_cast<std::uint64_t>(stats.seconds);
        const auto fps = presented_fps_ ? std::format(L"{:.1f}", *presented_fps_) : std::wstring(L"—");
        const auto loss = loss_rate_ ? std::format(L"{:.1f}%", *loss_rate_) : std::wstring(L"—");
        auto text = std::format(L"{:02}:{:02}:{:02}\r\n{} {}\r\n{} fps\r\n↓ {}\r\n↑ {}\r\n"
                                L"RTT {}\r\n帧等待 {}\r\n丢包 {}\r\n{} × {}",
            seconds / 3600, seconds / 60 % 60, seconds % 60, NetworkProtocol(stats.ice_protocol), NetworkPath(stats.ice_path),
            fps, down_rate_.empty() ? L"—" : down_rate_, up_rate_.empty() ? L"—" : up_rate_, Milliseconds(stats.ice_rtt_ms),
            Milliseconds(stats.decoded_frame_wait_ms), loss, stats.width, stats.height);
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
            ? std::format(L"采集 {:.1f} fps  |  提交 {:.1f} fps",
                rate(next.captured, previous_.captured), rate(next.submitted, previous_.submitted))
            : std::format(L"接收 {:.1f} fps  |  解码 {:.1f} fps  |  呈现 {:.1f} fps",
                rate(next.received, previous_.received), rate(next.decoded, previous_.decoded), *presented_fps_);
        if (previous_.seconds > 0) {
            const auto bandwidth = [](double bits_per_second) {
                const auto kbps = bits_per_second / 1000;
                return kbps >= 1000 ? std::format(L"{:.2f} Mbps", kbps / 1000) : std::format(L"{:.1f} Kbps", kbps);
            };
            up_rate_ = next.upload_bps ? bandwidth(*next.upload_bps) : L"—";
            down_rate_ = next.download_bps ? bandwidth(*next.download_bps) : L"—";
            rate_text_ += std::format(L"  |  上行 {}  下行 {}", up_rate_, down_rate_);
            loss_rate_.reset();
            if (next.video_loss_fraction) loss_rate_ = 100 * *next.video_loss_fraction;
        }
        previous_ = next;
        return true;
    }

    void Update() {
        RefreshInputReady();
        const auto state = session_.State();
        if (state == StreamState::waiting || state == StreamState::reconnecting) {
            previous_ = {};
            input_.Reset();
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
        EnableWindow(export_, !active && stats.seconds > 0);
        if (host_) {
            EnableWindow(screens_, !active);
            EnableWindow(low_latency_, !active);
            EnableWindow(allow_, state == StreamState::approval);
            EnableWindow(allow_input_, state == StreamState::approval);
        } else {
            p2premote::ViewerInput::UpdatePluginLocks(session_);
            EnableWindow(plugins_, session_.HasPluginMenu());
            const auto show_video = state == StreamState::streaming;
            if (show_video != video_visible_) {
                ShowWindow(surface_, show_video ? SW_SHOWNA : SW_HIDE);
                ShowWindow(placeholder_, show_video ? SW_HIDE : SW_SHOWNA);
                video_visible_ = show_video;
            }
            UpdateOverlay(stats);
        }
        const auto idle = host_ ? L"等待同账号设备连接" : L"点击连接设备";
        auto text = state == StreamState::idle ? idle : session_.Status();
        if (state == StreamState::streaming) {
            if (!host_) text += L"\r\n";
            if (!host_ || stats.ice_path != IcePath::unknown) {
                if (host_) text += L"  |  ";
                text += NetworkPath(stats.ice_path);
            }
            if (!stats.ice_protocol.empty()) text += std::format(L"  |  {}", NetworkProtocol(stats.ice_protocol));
            if (stats.ice_rtt_ms) text += std::format(L"  |  RTT {:.1f} ms", *stats.ice_rtt_ms);
            if (stats.srtp_active) text += L"  |  SRTP";
            if (!host_) {
                text += loss_rate_ ? std::format(L"  |  视频丢包 {:.1f}%", *loss_rate_) : L"  |  视频丢包 —";
                text += L"\r\n";
            }
        }
        if (state == StreamState::streaming && stats.width) {
            if (!stats.video_codec.empty()) {
                if (host_) text += L"  |  ";
                text += stats.video_codec;
                if (stats.gpu_native) text += L" · D3D11 纹理";
                else if (stats.gpu_conversion) text += L" · GPU 转换";
            }
            text += std::format(L"  |  {} × {}  |  {} 帧\r\n{}", stats.width, stats.height,
                host_ ? stats.submitted : stats.presented, rate_text_);
            if (stats.video_bitrate_limit_bps) {
                text += std::format(L"  |  视频上限 {} Mbps", *stats.video_bitrate_limit_bps / 1'000'000);
            }
            text += L"\r\n";
            text += host_ ? std::format(L"平均采集至提交 {:.2f} ms", stats.capture_to_submit_ms)
                          : std::format(L"绘制至提交 {:.2f} ms", stats.draw_to_present_ms);
            const auto ms = [](std::optional<double> value) {
                return value ? std::format(L"{:.2f} ms", *value) : std::wstring(L"—");
            };
            text += host_ ? std::format(L"\r\n平均发送排队 {}", ms(stats.packet_send_queue_ms))
                          : std::format(L"  |  解码后等待 {}", ms(stats.decoded_frame_wait_ms));
        }
        if (!host_ && input_ready_) {
            HCURSOR cursor{};
            if (session_.LocalCursor(cursor)) text += L"\r\n本地光标 · 点击画面接管键鼠，按 F12 释放。";
            else text += L"\r\n画面同步光标 · 点击画面接管键鼠，按 F12 释放。";
        }
        if (const auto notice = session_.PluginNotice(); !notice.empty()) {
            text += L"\r\n";
            text += notice;
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
    bool poll_cursor_;
    bool connect_on_open_;
    std::stop_token lifetime_;
    p2premote::StreamSession session_;
    p2premote::ViewerInput input_;
    HWND window_{};
    HWND screens_{};
    HWND connect_{};
    HWND allow_{};
    HWND allow_input_{};
    HWND low_latency_{};
    HWND quality_label_{};
    HWND quality_{};
    HWND bitrate_label_{};
    HWND bitrate_{};
    HWND stop_{};
    HWND export_{};
    HWND status_panel_{};
    HWND status_{};
    HWND surface_{};
    HWND placeholder_{};
    HWND overlay_toggle_{};
    HWND plugins_{};
    HWND overlay_{};
    bool video_visible_{};
    bool input_ready_{};
    bool hotkey_{};
    std::vector<HMONITOR> monitors_;
    StreamStatistics previous_;
    std::wstring visible_status_;
    std::wstring visible_overlay_;
    std::wstring rate_text_;
    std::wstring up_rate_;
    std::wstring down_rate_;
    std::optional<double> presented_fps_;
    std::optional<double> loss_rate_;
};

} // namespace

int RunStreamWindow(HINSTANCE instance, int show, bool host, StreamLaunchOptions options) {
    return StreamWindow(instance, host, std::move(options)).Run(show);
}
