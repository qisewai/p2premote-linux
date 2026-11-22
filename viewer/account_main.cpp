#include "account_api.h"
#include "stream_window.h"

#include <winrt/base.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using p2premote::AccountDevice;
using p2premote::AccountLogin;

constexpr wchar_t account_class[] = L"P2PRemote.Account";
constexpr wchar_t default_service_address[] = L"https://remote.pipisime88.net";
constexpr UINT account_result = WM_APP + 1;
constexpr UINT stream_error = WM_APP + 2;
constexpr UINT_PTR refresh_timer = 1;

enum ControlId {
    server_field = 300, user_field, password_field, login_button, register_button,
    device_list, connect_button, receive_button, refresh_button, logout_button, account_status
};

std::wstring Text(HWND control) {
    const auto length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(control, value.data(), length + 1)));
    return value;
}

void Cancel(std::jthread& worker) {
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
    }
}

struct Apartment {
    explicit Apartment(winrt::apartment_type type = winrt::apartment_type::multi_threaded) {
        winrt::init_apartment(type);
    }
    ~Apartment() { winrt::uninit_apartment(); }
};

struct Result {
    std::optional<AccountLogin> login;
    std::optional<std::vector<AccountDevice>> devices;
    std::wstring detail;
    bool expired{};
};

struct IceOverrides {
    std::filesystem::path trace_directory;
    std::filesystem::path latency_trace_directory;
    std::string stun_url;
    bool stun_only{};
};

class AccountWindow {
public:
    AccountWindow(HINSTANCE instance, bool compatible_http, IceOverrides ice)
        : instance_(instance), compatible_http_(compatible_http), ice_(std::move(ice)) {}

    ~AccountWindow() {
        Cancel(request_);
        Cancel(viewer_);
        Cancel(host_);
        if (window_) DestroyWindow(window_);
    }

    int Run(int show) {
        WNDCLASSW cls{};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        cls.lpszClassName = account_class;
        winrt::check_bool(RegisterClassW(&cls));
        const auto dpi = GetDpiForSystem();
        std::wstring title = compatible_http_ ? L"P2P Remote · 兼容登录" : L"P2P Remote · 我的设备";
        if (!ice_.trace_directory.empty()) title += L" · ICE 诊断";
        if (ice_.stun_only) title += L" · 仅直连";
        winrt::check_pointer(CreateWindowExW(0, account_class, title.c_str(),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            MulDiv(650, dpi, 96), MulDiv(460, dpi, 96), nullptr, nullptr, instance_, this));
        CreateControls();
        Layout();
        Update();
        SetFocus(user_);
        ShowWindow(window_, show);
        MSG message{};
        for (;;) {
            const auto available = GetMessageW(&message, nullptr, 0, 0);
            if (available < 0) winrt::throw_last_error();
            if (!available) return static_cast<int>(message.wParam);
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<AccountWindow*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        const auto self = reinterpret_cast<AccountWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        try {
            return self->Handle(hwnd, message, wparam, lparam);
        } catch (const winrt::hresult_error& error) {
            self->SetStatus(error.message().c_str());
        } catch (const std::exception& error) {
            self->SetStatus(winrt::to_hstring(error.what()).c_str());
        }
        return 0;
    }

    LRESULT Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case login_button: Login(false); return 0;
            case register_button: Login(true); return 0;
            case device_list:
                if (HIWORD(wparam) == LBN_DBLCLK) Connect();
                else if (HIWORD(wparam) == LBN_SELCHANGE) Update();
                return 0;
            case connect_button: Connect(); return 0;
            case receive_button: ToggleReceiving(); return 0;
            case refresh_button: Refresh(); return 0;
            case logout_button: Logout(); return 0;
            }
            break;
        case account_result:
            OnResult();
            return 0;
        case stream_error:
            {
                const std::lock_guard lock(mutex_);
                SetStatus(child_error_.c_str());
            }
            return 0;
        case WM_TIMER:
            if (wparam == refresh_timer) Refresh();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_DPICHANGED: {
            const auto& size = *reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, size.left, size.top, size.right - size.left, size.bottom - size.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
            const auto dpi = GetDpiForWindow(hwnd);
            limits.ptMinTrackSize = {MulDiv(520, dpi, 96), MulDiv(355, dpi, 96)};
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, refresh_timer);
            Cancel(request_);
            Cancel(viewer_);
            Cancel(host_);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            window_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style = 0, int id = 0) const {
        const auto control = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        winrt::check_pointer(control);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return control;
    }

    void CreateControls() {
        constexpr DWORD edit = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
        constexpr DWORD button = WS_TABSTOP | BS_PUSHBUTTON;
        server_label_ = Control(L"STATIC", L"服务地址");
        user_label_ = Control(L"STATIC", L"账号");
        password_label_ = Control(L"STATIC", L"密码");
        server_ = Control(L"EDIT", default_service_address, edit, server_field);
        user_ = Control(L"EDIT", L"", edit, user_field);
        password_ = Control(L"EDIT", L"", edit | ES_PASSWORD, password_field);
        SendMessageW(server_, EM_SETLIMITTEXT, 256, 0);
        SendMessageW(user_, EM_SETLIMITTEXT, 32, 0);
        SendMessageW(password_, EM_SETLIMITTEXT, 72, 0);
        login_button_ = Control(L"BUTTON", L"登录", button, login_button);
        register_button_ = Control(L"BUTTON", L"注册并登录", button, register_button);
        notice_ = Control(L"STATIC", L"登录后，本机会允许同账号设备发起远程控制。");
        account_label_ = Control(L"STATIC", L"");
        list_ = Control(L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            device_list);
        connect_ = Control(L"BUTTON", L"连接设备", button, connect_button);
        receive_ = Control(L"BUTTON", L"停止接收", button, receive_button);
        refresh_ = Control(L"BUTTON", L"刷新", button, refresh_button);
        logout_ = Control(L"BUTTON", L"退出登录", button, logout_button);
        status_ = Control(L"STATIC", L"请输入账号和密码。", WS_VISIBLE, account_status);
    }

    void Layout() {
        if (!status_) return;
        const auto dpi = GetDpiForWindow(window_);
        const auto px = [dpi](int n) { return MulDiv(n, dpi, 96); };
        RECT client{};
        GetClientRect(window_, &client);
        const auto place = [&](HWND child, int x, int y, int width, int height) {
            MoveWindow(child, px(x), px(y), px(width), px(height), TRUE);
        };
        const auto width = MulDiv(client.right, 96, dpi);
        const auto height = MulDiv(client.bottom, 96, dpi);
        const auto field_width = std::max(120, width - 117);
        place(server_label_, 20, 29, 69, 22);
        place(server_, 95, 24, field_width, 26);
        place(user_label_, 20, 71, 69, 22);
        place(user_, 95, 66, field_width, 26);
        place(password_label_, 20, 113, 69, 22);
        place(password_, 95, 108, field_width, 26);
        place(login_button_, 95, 153, 88, 28);
        place(register_button_, 193, 153, 113, 28);
        place(notice_, 20, 204, width - 40, 40);
        place(account_label_, 20, 25, width - 40, 25);
        place(list_, 20, 57, width - 40, std::max(50, height - 154));
        place(connect_, 20, height - 83, 100, 28);
        place(receive_, 130, height - 83, 100, 28);
        place(refresh_, 240, height - 83, 70, 28);
        place(logout_, width - 110, height - 83, 90, 28);
        place(status_, 20, height - 42, width - 40, 40);
    }

    void SetStatus(const wchar_t* message) const {
        if (status_) SetWindowTextW(status_, message);
    }

    const AccountDevice* Selection() const {
        const auto n = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        return n >= 0 && static_cast<std::size_t>(n) < devices_.size() ? &devices_[static_cast<std::size_t>(n)] : nullptr;
    }

    void Update() {
        const bool signed_in = login_.has_value();
        for (auto child : {server_label_, server_, user_label_, user_, password_label_, password_,
                 login_button_, register_button_, notice_}) {
            ShowWindow(child, signed_in ? SW_HIDE : SW_SHOWNA);
            EnableWindow(child, !busy_);
        }
        for (auto child : {account_label_, list_, connect_, receive_, refresh_, logout_}) {
            ShowWindow(child, signed_in ? SW_SHOWNA : SW_HIDE);
        }
        const auto selected = Selection();
        EnableWindow(connect_, signed_in && selected && selected->state == L"online" && selected->id != login_->device_id);
        EnableWindow(refresh_, !busy_);
        SetWindowTextW(receive_, receiving_ ? L"停止接收" : L"开始接收");
    }

    template <class Job>
    void Request(Job job) {
        if (busy_) return;
        if (request_.joinable()) request_.join();
        const auto target = window_;
        request_ = std::jthread([this, target, job = std::move(job)](std::stop_token stop) {
            Result result;
            try {
                const Apartment apartment;
                result = job(stop);
            } catch (const p2premote::LoginExpired& error) {
                result.expired = true;
                result.detail = winrt::to_hstring(error.what());
            } catch (const winrt::hresult_error& error) {
                result.detail = error.message();
            } catch (const std::exception& error) {
                result.detail = winrt::to_hstring(error.what());
            }
            if (stop.stop_requested()) return;
            {
                const std::lock_guard lock(mutex_);
                result_ = std::move(result);
            }
            PostMessageW(target, account_result, 0, 0);
        });
        busy_ = true;
        Update();
    }

    void Login(bool create) {
        if (login_ || busy_) return;
        const auto server = Text(server_);
        const auto user = Text(user_);
        auto password = Text(password_);
        if (server.empty() || user.empty() || password.empty()) {
            SetStatus(L"请输入服务地址、账号和密码。");
            return;
        }
        SetWindowTextW(password_, L"");
        SetStatus(create ? L"正在注册并登记本机…" : L"正在登录并登记本机…");
        Request([server, user, password = std::move(password), create, compatible_http = compatible_http_](std::stop_token stop) {
            Result r;
            r.login = p2premote::SignIn(stop, server, user, password, create, compatible_http);
            return r;
        });
    }

    void Refresh() {
        if (!login_) return;
        Request([login = *login_](std::stop_token stop) {
            Result r;
            r.devices = p2premote::AccountDevices(stop, login);
            return r;
        });
    }

    void OnResult() {
        std::optional<Result> pending;
        {
            const std::lock_guard lock(mutex_);
            pending = std::exchange(result_, std::nullopt);
        }
        if (!pending) return;
        busy_ = false;
        if (pending->expired) ClearLogin();
        if (!pending->detail.empty()) SetStatus(pending->detail.c_str());
        if (pending->login) {
            login_ = std::move(pending->login);
            SetWindowTextW(account_label_, (L"我的设备 · " + Text(user_)).c_str());
            SetStatus(L"已登录，正在上线本机。");
            ToggleReceiving();
            winrt::check_bool(SetTimer(window_, refresh_timer, 3'000, nullptr) != 0);
            Refresh();
        }
        if (pending->devices) SetDevices(std::move(*pending->devices));
        Update();
    }

    void SetDevices(std::vector<AccountDevice> devices) {
        if (devices != devices_) {
            const auto selection = Selection();
            const std::wstring current = selection ? selection->id : std::wstring{};
            devices_ = std::move(devices);
            SendMessageW(list_, LB_RESETCONTENT, 0, 0);
            for (std::size_t i = 0; i < devices_.size(); ++i) {
                const auto& d = devices_[i];
                std::wstring line = d.name + L"  ·  " + d.platform;
                if (d.id == login_->device_id) line += L"  ·  本机";
                line += d.state == L"online" ? L"  ·  可连接" : d.state == L"busy" ? L"  ·  控制中" : L"  ·  离线";
                SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
                if (d.id == current) SendMessageW(list_, LB_SETCURSEL, i, 0);
            }
        }
        SetStatus(receiving_ ? L"本机已登记。可在设备列表查看上线状态。" : L"本机已暂停接收远程控制。");
    }

    void OpenStream(bool host, p2premote::ConnectionOptions connection) {
        if (ice_.stun_only && connection.relay_only) {
            throw std::runtime_error("服务要求通过 TURN 中继，此会话无法使用 --stun-only");
        }
        auto& stream = host ? host_ : viewer_;
        Cancel(stream);
        connection.ice_trace_dir = ice_.trace_directory;
        connection.latency_trace_dir = ice_.latency_trace_directory;
        if (!ice_.stun_url.empty()) {
            bool replaced{};
            for (auto& server : connection.ice_servers) {
                for (auto& url : server.urls) {
                    if (url.starts_with("stun:")) {
                        url = ice_.stun_url;
                        replaced = true;
                    }
                }
            }
            if (!replaced) {
                p2premote::IceServerOptions stun;
                stun.urls.push_back(ice_.stun_url);
                connection.ice_servers.insert(connection.ice_servers.begin(), std::move(stun));
            }
        }
        if (ice_.stun_only) {
            for (auto& server : connection.ice_servers) {
                std::erase_if(server.urls, [](const std::string& url) {
                    return url.starts_with("turn:") || url.starts_with("turns:");
                });
            }
            std::erase_if(connection.ice_servers, [](const auto& server) { return server.urls.empty(); });
        }
        const auto parent = window_;
        stream = std::jthread([this, parent, host, connection = std::move(connection)](std::stop_token stop) mutable {
            std::wstring error_text;
            try {
                const Apartment apartment(winrt::apartment_type::single_threaded);
                connection.auto_control = host;
                RunStreamWindow(instance_, host ? SW_HIDE : SW_SHOWNORMAL, host,
                    StreamLaunchOptions{host, std::move(connection), !host, stop});
            } catch (const winrt::hresult_error& error) {
                error_text = error.message();
            } catch (const std::exception& error) {
                error_text = winrt::to_hstring(error.what());
            }
            if (!stop.stop_requested() && !error_text.empty()) {
                const std::lock_guard lock(mutex_);
                child_error_ = std::move(error_text);
                PostMessageW(parent, stream_error, 0, 0);
            }
        });
    }

    void ToggleReceiving() {
        if (!login_) return;
        if (receiving_) {
            Cancel(host_);
            SetStatus(L"本机已暂停接收远程控制。");
        } else {
            OpenStream(true, login_->connection);
            SetStatus(L"正在上线本机。");
        }
        receiving_ = !receiving_;
        Update();
    }

    void Connect() {
        if (!login_) return;
        const auto selected = Selection();
        if (!selected || selected->state != L"online" || selected->id == login_->device_id) return;
        auto connection = login_->connection;
        connection.remote_device = selected->id;
        OpenStream(false, std::move(connection));
    }

    void ClearLogin() {
        KillTimer(window_, refresh_timer);
        Cancel(viewer_);
        Cancel(host_);
        receiving_ = false;
        login_.reset();
        devices_.clear();
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    }

    void Logout() {
        if (!login_) return;
        Cancel(request_);
        {
            const std::lock_guard lock(mutex_);
            result_.reset();
        }
        busy_ = false;
        auto login = std::move(*login_);
        ClearLogin();
        SetStatus(L"正在退出登录…");
        Request([login = std::move(login)](std::stop_token stop) {
            p2premote::SignOut(stop, login);
            Result r;
            r.detail = L"已退出登录。";
            return r;
        });
    }

    HINSTANCE instance_;
    bool compatible_http_;
    IceOverrides ice_;
    HWND window_{}, server_label_{}, user_label_{}, password_label_{};
    HWND server_{}, user_{}, password_{}, login_button_{}, register_button_{}, notice_{};
    HWND account_label_{}, list_{}, connect_{}, receive_{}, refresh_{}, logout_{}, status_{};
    bool busy_{}, receiving_{};
    std::optional<AccountLogin> login_;
    std::vector<AccountDevice> devices_;
    std::mutex mutex_;
    std::optional<Result> result_;
    std::wstring child_error_;
    std::jthread request_, host_, viewer_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        const Apartment apartment(winrt::apartment_type::single_threaded);
        int count{};
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> args(CommandLineToArgvW(GetCommandLineW(), &count), LocalFree);
        winrt::check_pointer(args.get());
        bool compatible_http{};
        IceOverrides ice;
        for (int i = 1; i < count; ++i) {
            const std::wstring_view option(args.get()[i]);
            if (option == L"--compat-account") {
                compatible_http = true;
            } else if (option == L"--stun-only") {
                ice.stun_only = true;
            } else if (option == L"--ice-trace" || option == L"--latency-trace") {
                if (++i >= count || !*args.get()[i] || std::wstring_view(args.get()[i]).starts_with(L"--")) {
                    throw std::runtime_error(winrt::to_string(winrt::hstring(option)) + " 需要指定日志目录");
                }
                auto& directory = option == L"--ice-trace" ? ice.trace_directory : ice.latency_trace_directory;
                directory = std::filesystem::absolute(args.get()[i]);
                std::filesystem::create_directories(directory);
            } else if (option == L"--stun") {
                if (++i >= count || !*args.get()[i] || std::wstring_view(args.get()[i]).starts_with(L"--")) {
                    throw std::runtime_error("--stun 需要指定 stun:主机:端口");
                }
                ice.stun_url = winrt::to_string(args.get()[i]);
                if (!ice.stun_url.starts_with("stun:")) {
                    throw std::runtime_error("--stun 需要以 stun: 开头");
                }
            }
        }
        return AccountWindow(instance, compatible_http, std::move(ice)).Run(show);
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"P2P Remote", MB_ICONERROR);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"P2P Remote", MB_ICONERROR);
    }
    return 1;
}
