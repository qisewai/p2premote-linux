#include <p2premote/preview_session.h>

#ifdef P2PREMOTE_BUILD_STREAMING
#include "../viewer/stream_window.h"
#endif

#include <commdlg.h>
#include <winrt/base.h>

#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t host_class[] = L"P2PRemote.Host";
constexpr wchar_t preview_class[] = L"P2PRemote.PreviewSurface";
constexpr UINT_PTR stats_timer = 1;
enum ControlId { monitor_list = 100, start_capture, stop_capture, export_csv };

struct Monitor {
    HMONITOR handle;
    RECT bounds;
    std::wstring name;
};

BOOL CALLBACK AddMonitor(HMONITOR handle, HDC, LPRECT, LPARAM context) noexcept {
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(handle, &info)) {
        try {
            monitors.push_back({handle, info.rcMonitor, info.szDevice});
        } catch (...) {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
    }
    return TRUE;
}

class HostWindow {
public:
    explicit HostWindow(HINSTANCE instance) : instance_(instance) {}
    ~HostWindow() {
        if (window_) {
            DestroyWindow(window_);
        }
    }

    int Run(int show) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        window_class.lpszClassName = host_class;
        winrt::check_bool(RegisterClassW(&window_class));

        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = preview_class;
        winrt::check_bool(RegisterClassW(&window_class));

        const auto initial_dpi = GetDpiForSystem();
        winrt::check_pointer(CreateWindowExW(0, host_class, L"P2P Remote · 本机屏幕预览",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            MulDiv(1050, initial_dpi, 96), MulDiv(700, initial_dpi, 96), nullptr, nullptr, instance_, this));

        combo_ = CreateControl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, monitor_list);
        start_ = CreateControl(L"BUTTON", L"开始预览", WS_TABSTOP | BS_PUSHBUTTON, start_capture);
        stop_ = CreateControl(L"BUTTON", L"停止", WS_TABSTOP | BS_PUSHBUTTON, stop_capture);
        export_ = CreateControl(L"BUTTON", L"导出 CSV", WS_TABSTOP | BS_PUSHBUTTON, export_csv);
        status_ = CreateControl(L"STATIC", L"", SS_LEFT, 0);
        preview_ = CreateControl(preview_class, L"", 0, 0);

        RefreshMonitors();
        UpdateControls();
        UpdateStatus();
        Layout();
        winrt::check_bool(SetTimer(window_, stats_timer, 1000, nullptr) != 0);
        ShowWindow(window_, show);

        for (;;) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                if (!IsDialogMessageW(window_, &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }

            std::array<HANDLE, 2> events{};
            DWORD count = 0;
            if (const auto capture = session_.CaptureEvent()) {
                events[count++] = capture;
            }
            if (const auto present = session_.PresentEvent()) {
                events[count++] = present;
            }
            const auto result = MsgWaitForMultipleObjectsEx(count, events.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_FAILED) {
                winrt::throw_last_error();
            }
            try {
                if (count && result == WAIT_OBJECT_0) {
                    if (!session_.ProcessCapture()) {
                        Stop(L"Windows 已关闭采集源");
                    }
                } else if (count == 2 && result == WAIT_OBJECT_0 + 1) {
                    session_.Present();
                }
            } catch (...) {
                ReportError();
            }
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* app = static_cast<HostWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app) {
            try {
                return app->OnMessage(message, wparam, lparam);
            } catch (...) {
                app->ReportError();
                return 0;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT OnMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                switch (LOWORD(wparam)) {
                case start_capture: Start(); return 0;
                case stop_capture: Stop(L"已停止"); return 0;
                case export_csv: Export(); return 0;
                }
            }
            break;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_DPICHANGED: {
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto& info = *reinterpret_cast<MINMAXINFO*>(lparam);
            const auto dpi = GetDpiForWindow(window_);
            info.ptMinTrackSize = {MulDiv(670, dpi ? dpi : 96, 96), MulDiv(365, dpi ? dpi : 96, 96)};
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (combo_ && !session_.IsRunning()) {
                RefreshMonitors();
                UpdateControls();
                UpdateStatus();
            }
            break;
        case WM_TIMER:
            if (wparam == stats_timer && session_.IsRunning()) {
                UpdateStatus();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window_, stats_timer);
            session_.Stop();
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            const auto hwnd = window_;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            window_ = nullptr;
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    HWND CreateControl(const wchar_t* type, const wchar_t* label, DWORD style, int id) const {
        const auto control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        winrt::check_pointer(control);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return control;
    }

    void Layout() {
        if (!preview_) {
            return;
        }
        if (IsIconic(window_)) {
            session_.OnPreviewResize(false);
            return;
        }
        const auto dpi = GetDpiForWindow(window_);
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        RECT client{};
        GetClientRect(window_, &client);
        MoveWindow(combo_, px(10), px(10), px(280), px(240), TRUE);
        MoveWindow(start_, px(300), px(9), px(100), px(27), TRUE);
        MoveWindow(stop_, px(410), px(9), px(68), px(27), TRUE);
        MoveWindow(export_, px(488), px(9), px(95), px(27), TRUE);
        const auto preview_width = std::max(0L, client.right - px(20));
        const auto preview_height = std::max(0L, client.bottom - px(110));
        MoveWindow(preview_, px(10), px(44), preview_width, preview_height, TRUE);
        MoveWindow(status_, px(10), px(50) + preview_height, preview_width, px(56), TRUE);
        session_.OnPreviewResize(preview_width > 0 && preview_height > 0);
    }

    void RefreshMonitors() {
        std::wstring previous;
        const auto index = SendMessageW(combo_, CB_GETCURSEL, 0, 0);
        if (index >= 0 && static_cast<std::size_t>(index) < monitors_.size()) {
            previous = monitors_[static_cast<std::size_t>(index)].name;
        }
        monitors_.clear();
        winrt::check_bool(EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&monitors_)));
        SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
        std::size_t selected = 0;
        for (std::size_t i = 0; i < monitors_.size(); ++i) {
            const auto& monitor = monitors_[i];
            const auto& rect = monitor.bounds;
            const auto label = std::format(L"显示器 {} · {} × {}  ({}, {})", i + 1,
                rect.right - rect.left, rect.bottom - rect.top, rect.left, rect.top);
            SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (monitor.name == previous) {
                selected = i;
            }
        }
        if (!monitors_.empty()) {
            SendMessageW(combo_, CB_SETCURSEL, selected, 0);
        }
    }

    void Start() {
        if (session_.IsRunning()) {
            return;
        }
        const auto index = SendMessageW(combo_, CB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<std::size_t>(index) >= monitors_.size()) {
            return;
        }
        session_.Start(monitors_[static_cast<std::size_t>(index)].handle, preview_);
        status_message_ = L"预览中";
        previous_stats_ = {};
        UpdateControls();
        UpdateStatus();
    }

    void Stop(const wchar_t* message) {
        session_.Stop();
        status_message_ = message;
        InvalidateRect(preview_, nullptr, TRUE);
        RefreshMonitors();
        UpdateControls();
        UpdateStatus();
    }

    void UpdateControls() {
        const auto active = session_.IsRunning();
        EnableWindow(combo_, !active);
        EnableWindow(start_, !active && !monitors_.empty());
        EnableWindow(stop_, active);
        EnableWindow(export_, !active && session_.Statistics().presented > 0);
    }

    void UpdateStatus() {
        const auto stats = session_.Statistics();
        if (!stats.source_width) {
            SetWindowTextW(status_, monitors_.empty() ? L"未找到显示器" : status_message_);
            return;
        }
        const auto baseline = session_.IsRunning() ? previous_stats_ : p2premote::PreviewStatistics{};
        const auto elapsed = stats.elapsed_seconds - baseline.elapsed_seconds;
        const auto captured_fps = elapsed > 0 ? static_cast<double>(stats.captured - baseline.captured) / elapsed : 0;
        const auto presented_fps = elapsed > 0 ? static_cast<double>(stats.presented - baseline.presented) / elapsed : 0;
        const auto latency = stats.capture_to_present_return;
        const auto line = std::format(L"{}  |  {} × {}  |  采集 {:.1f} fps  |  呈现 {:.1f} fps  |  丢帧 {}\r\n"
            L"队列 {} / 峰值 {}  |  取帧至 Present 返回 P50 {:.1f} ms / P95 {:.1f} ms\r\n"
            L"分段 P50/P95 (ms)  |  取纹理 {:.1f}/{:.1f}  |  等待 {:.1f}/{:.1f}  |  提交 {:.1f}/{:.1f}  |  Present {:.1f}/{:.1f}",
            status_message_, stats.source_width, stats.source_height, captured_fps, presented_fps,
            stats.dropped, stats.queue_depth, stats.max_queue_depth, latency.p50_ms, latency.p95_ms,
            stats.capture_to_ready.p50_ms, stats.capture_to_ready.p95_ms,
            stats.ready_to_draw.p50_ms, stats.ready_to_draw.p95_ms,
            stats.draw_to_submit.p50_ms, stats.draw_to_submit.p95_ms,
            stats.present_call.p50_ms, stats.present_call.p95_ms);
        SetWindowTextW(status_, line.c_str());
        previous_stats_ = stats;
    }

    void Export() {
        std::array<wchar_t, 32768> name{};
        wcscpy_s(name.data(), name.size(), L"preview-timings.csv");
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0";
        dialog.lpstrFile = name.data();
        dialog.nMaxFile = static_cast<DWORD>(name.size());
        dialog.lpstrDefExt = L"csv";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) {
            session_.ExportCsv(name.data());
        } else if (const auto error = CommDlgExtendedError()) {
            throw winrt::hresult_error(E_FAIL, std::format(L"保存对话框错误 0x{:X}", error));
        }
    }

    void ReportError() noexcept {
        session_.Stop();
        status_message_ = L"预览已停止";
        InvalidateRect(preview_, nullptr, TRUE);
        EnableWindow(combo_, TRUE);
        EnableWindow(start_, !monitors_.empty());
        EnableWindow(stop_, FALSE);
        SetWindowTextW(status_, status_message_);
        try {
            UpdateControls();
            try {
                throw;
            } catch (const winrt::hresult_error& error) {
                MessageBoxW(window_, error.message().c_str(), L"P2P Remote", MB_OK | MB_ICONERROR);
            } catch (const std::exception& error) {
                MessageBoxW(window_, winrt::to_hstring(error.what()).c_str(), L"P2P Remote", MB_OK | MB_ICONERROR);
            }
        } catch (...) {
            MessageBoxW(window_, L"运行本机预览时发生错误。", L"P2P Remote", MB_OK | MB_ICONERROR);
        }
    }

    HINSTANCE instance_{};
    HWND window_{};
    HWND preview_{};
    HWND combo_{};
    HWND start_{};
    HWND stop_{};
    HWND export_{};
    HWND status_{};
    std::vector<Monitor> monitors_;
    p2premote::PreviewSession session_;
    p2premote::PreviewStatistics previous_stats_;
    const wchar_t* status_message_{L"选择显示器后开始预览"};
};

int RunHost(HINSTANCE instance, int show) {
    HostWindow window(instance);
    return window.Run(show);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
#ifdef P2PREMOTE_BUILD_STREAMING
        auto options = ReadStreamOptions(true);
        const auto result = options.share ? RunStreamWindow(instance, show, true, std::move(options))
                                          : RunHost(instance, show);
#else
        const auto result = RunHost(instance, show);
#endif
        winrt::uninit_apartment();
        return result;
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"P2P Remote", MB_OK | MB_ICONERROR);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"P2P Remote", MB_OK | MB_ICONERROR);
    }
    return 1;
}
