#include "account_window.h"
#include "app_settings.h"
#include "frameless_chrome.h"
#include "settings_dialog.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFormLayout>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QJsonDocument>
#include <QHeaderView>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QVBoxLayout>

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr int stop_hotkey = 1;
constexpr int device_state_role = Qt::UserRole + 1;
constexpr int device_name_role = Qt::UserRole + 2;
constexpr int lan_port_role = Qt::UserRole + 3;
constexpr int lan_busy_role = Qt::UserRole + 4;

QString winText(const wchar_t* text) { return text ? QString::fromWCharArray(text) : QString{}; }

QLabel* heading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("heading"));
    label->setTextFormat(Qt::PlainText);
    return label;
}

struct Monitor {
    HMONITOR handle{};
    QSize size;
    bool primary{};
};

BOOL CALLBACK enumMonitors(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    try {
        auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(context);
        const auto& rect = info.rcMonitor;
        monitors.push_back({monitor, QSize(rect.right - rect.left, rect.bottom - rect.top),
            (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

bool interactiveDesktop() {
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    wchar_t name[32]{};
    DWORD size{};
    const bool valid = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) &&
        _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return valid;
}

QByteArray captureAgentScreen(void* monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(static_cast<HMONITOR>(monitor), &info) || !interactiveDesktop()) return {};
    DwmFlush();
    if (!interactiveDesktop()) return {};
    const auto& area = info.rcMonitor;
    const QSize source(area.right - area.left, area.bottom - area.top);
    const auto output = source.width() > 1280 || source.height() > 720
        ? source.scaled(QSize(1280, 720), Qt::KeepAspectRatio) : source;
    if (output.isEmpty()) return {};

    struct DesktopDc {
        HDC dc{GetDC(nullptr)};
        ~DesktopDc() { if (dc) ReleaseDC(nullptr, dc); }
    } desktop;
    if (!desktop.dc) return {};
    const std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)> memory(CreateCompatibleDC(desktop.dc), DeleteDC);
    if (!memory) return {};
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = output.width();
    bitmap.bmiHeader.biHeight = -output.height();
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    const std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> dib(
        CreateDIBSection(desktop.dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0), DeleteObject);
    if (!dib || !pixels) return {};
    const auto previous = SelectObject(memory.get(), dib.get());
    if (!previous || previous == HGDI_ERROR) return {};
    SetStretchBltMode(memory.get(), HALFTONE);
    SetBrushOrgEx(memory.get(), 0, 0, nullptr);
    const auto copied = StretchBlt(memory.get(), 0, 0, output.width(), output.height(), desktop.dc,
        area.left, area.top, source.width(), source.height(), SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(memory.get(), previous);
    if (!copied || !interactiveDesktop()) return {};
    auto image = QImage(static_cast<const uchar*>(pixels), output.width(), output.height(), QImage::Format_RGB32).copy();
    if (image.isNull() || !interactiveDesktop()) return {};
    for (const auto& size : {QSize(1280, 720), QSize(960, 540), QSize(640, 360), QSize(480, 270)}) {
        if (image.width() > size.width() || image.height() > size.height()) {
            image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray png;
        QBuffer buffer(&png);
        if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") && png.size() <= P2P_AGENT_IMAGE_LIMIT) {
            return png;
        }
    }
    return {};
}

} // namespace

class HostSession final {
public:
    using Callback = std::function<void(int state, const QString& status, bool desktop_control)>;
    using AgentCallback = std::function<void(std::uint32_t id, int tool)>;

    HostSession(p2p_account* account, void* monitor, LaunchOptions options, Callback on_update,
        AgentCallback on_agent, QString* error)
        : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), on_update_(std::move(on_update)), on_agent_(std::move(on_agent)) {
        init(account, std::nullopt, monitor, std::move(options), error);
    }

    HostSession(const LanConnectOptions& lan, void* monitor, LaunchOptions options, Callback on_update, QString* error)
        : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), on_update_(std::move(on_update)) {
        init(nullptr, lan, monitor, std::move(options), error);
    }

    ~HostSession() {
        stop();
        if (wake_) CloseHandle(wake_);
    }

    bool valid() const { return started_; }
    bool stopped() const { return stopped_.load(std::memory_order_acquire); }

    void displayChanged() {
        display_changed_.store(true, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

    void answerAgent(std::uint32_t id, bool approved, QByteArray png = {}) {
        {
            const std::lock_guard lock(agent_mutex_);
            agent_answer_ = AgentAnswer{id, approved, std::move(png)};
        }
        if (wake_) SetEvent(wake_);
    }

    void revokeAgent(bool temporary_block) {
        agent_revoke_.store(temporary_block ? 2 : 1, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

private:
    struct AgentAnswer {
        std::uint32_t id;
        bool approved;
        QByteArray png;
    };

    void init(p2p_account* account, std::optional<LanConnectOptions> lan, void* monitor, LaunchOptions options,
        QString* error) {
        if (!wake_) {
            if (error) *error = QStringLiteral("无法创建接收线程唤醒事件。");
            return;
        }
        std::promise<QString> started;
        auto ready = started.get_future();
        thread_ = std::jthread([this, account, lan = std::move(lan), monitor, options = std::move(options),
            started = std::move(started)](std::stop_token stop) mutable {
            run(stop, account, lan, monitor, options, std::move(started));
        });
        const auto startup_error = ready.get();
        if (!startup_error.isEmpty()) {
            if (error) *error = startup_error;
            stop();
            return;
        }
        started_ = true;
    }

    void stop() {
        if (thread_.joinable()) {
            thread_.request_stop();
            if (wake_) SetEvent(wake_);
            thread_.join();
        }
    }

    void run(std::stop_token stop, p2p_account* account, const std::optional<LanConnectOptions>& lan,
        void* monitor, const LaunchOptions& options, std::promise<QString> started) {
        std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream(nullptr, p2p_stream_destroy);
        if (lan) {
            const p2p_lan_options_v1 lan_settings{
                lan->host.empty() ? nullptr : lan->host.c_str(), lan->port, lan->access_code.c_str(), 1,
                options.ice_trace.c_str(), options.latency_trace.c_str()};
            stream.reset(p2p_stream_create_lan(1, nullptr, &lan_settings));
        } else {
            const p2p_connection_options_v1 settings{
                options.ice_trace.c_str(), options.latency_trace.c_str(), options.stun.c_str(), options.stun_only, 1, 1};
            stream.reset(p2p_stream_create(account, 1, nullptr, nullptr, &settings));
        }
        if (!stream) {
            started.set_value(winText(p2p_last_error()));
            return;
        }
        if (p2p_stream_start(stream.get(), monitor, 60) != P2P_OK) {
            started.set_value(winText(p2p_last_error()));
            return;
        }
        started.set_value(QString{});

        int last_state = -1;
        bool last_desktop = false;
        bool input_ready = false;
        std::uint32_t agent_approval{};
        std::uint32_t input_blocked_for{};
        QString last_status;
        auto publish = [&](bool force) {
            p2p_stream_snapshot_v1 snapshot{};
            if (p2p_stream_snapshot(stream.get(), &snapshot) != P2P_OK) {
                const auto error = winText(p2p_last_error());
                const auto reason = error.toStdWString();
                p2p_stream_stop(stream.get(), reason.c_str());
                stopped_.store(true, std::memory_order_release);
                if (on_update_) on_update_(P2P_STOPPED, error, false);
                return false;
            }
            input_ready = snapshot.input_ready != 0;
            const auto status = winText(snapshot.status);
            const bool desktop = snapshot.desktop_control != 0;
            if (snapshot.state == P2P_STOPPED) stopped_.store(true, std::memory_order_release);
            if (force || snapshot.state != last_state || desktop != last_desktop || status != last_status) {
                last_state = snapshot.state;
                last_desktop = desktop;
                last_status = status;
                if (on_update_) on_update_(snapshot.state, status, desktop);
            }
            return snapshot.state != P2P_STOPPED;
        };

        auto fail = [&]() {
            const auto error = winText(p2p_last_error());
            const auto reason = error.toStdWString();
            p2p_stream_stop(stream.get(), reason.c_str());
            publish(true);
        };

        if (!publish(true)) return;

        constexpr ULONGLONG tick_interval_ms = 1000;
        constexpr ULONGLONG cursor_interval_ms = 33;
        auto now = GetTickCount64();
        auto next_tick = now + tick_interval_ms;
        auto next_cursor = now + cursor_interval_ms;

        while (!stop.stop_requested()) {
            HANDLE handles[3]{wake_};
            uint32_t kinds[3]{0};
            DWORD count = 1;
            if (const auto event = p2p_stream_wait_handle(stream.get(), P2P_WAIT_SESSION)) {
                handles[count] = event;
                kinds[count++] = P2P_WAIT_SESSION;
            }
            if (const auto capture = p2p_stream_wait_handle(stream.get(), P2P_WAIT_CAPTURE)) {
                handles[count] = capture;
                kinds[count++] = P2P_WAIT_CAPTURE;
            }

            now = GetTickCount64();
            ULONGLONG wait_ms = next_tick > now ? (next_tick - now) : 0;
            if (input_ready) {
                const ULONGLONG cursor_wait = next_cursor > now ? (next_cursor - now) : 0;
                wait_ms = std::min(wait_ms, cursor_wait);
            }

            const DWORD waited = MsgWaitForMultipleObjectsEx(count, handles, static_cast<DWORD>(wait_ms),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (stop.stop_requested()) break;

            bool check_snapshot = false;
            if (display_changed_.exchange(false, std::memory_order_acq_rel)) {
                if (p2p_stream_display_changed(stream.get()) != P2P_OK) {
                    fail();
                    break;
                }
                check_snapshot = true;
            }

            if (const auto revoke = agent_revoke_.exchange(0, std::memory_order_acq_rel)) {
                p2p_stream_revoke_agent_approval(stream.get(), revoke == 2);
                check_snapshot = true;
            }
            std::optional<AgentAnswer> answer;
            {
                const std::lock_guard lock(agent_mutex_);
                answer = std::exchange(agent_answer_, {});
            }
            if (answer) {
                const auto id = answer->id;
                p2p_stream_decide_agent_approval(stream.get(), id, answer->approved, answer->png.constData(),
                    static_cast<std::size_t>(answer->png.size()));
                if (input_blocked_for == id) {
                    if (p2p_stream_block_agent_prompt_input(stream.get(), 0) != P2P_OK) {
                        fail();
                        break;
                    }
                    input_blocked_for = 0;
                }
                check_snapshot = true;
            }

            if (waited >= WAIT_OBJECT_0 + 1 && waited < WAIT_OBJECT_0 + count) {
                const auto kind = kinds[waited - WAIT_OBJECT_0];
                if (p2p_stream_dispatch(stream.get(), kind) != P2P_OK) {
                    fail();
                    break;
                }
                if (kind == P2P_WAIT_SESSION) check_snapshot = true;
            }

            // Pump STA COM / WinRT callbacks (e.g. WGC frame arrival) without blocking.
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (stop.stop_requested()) break;

            now = GetTickCount64();
            if (now >= next_tick) {
                next_tick = now + tick_interval_ms;
                if (p2p_stream_tick(stream.get()) != P2P_OK) {
                    fail();
                    break;
                }
                check_snapshot = true;
            }
            if (input_ready && now >= next_cursor) {
                next_cursor = now + cursor_interval_ms;
                if (p2p_stream_poll_cursor(stream.get()) != P2P_OK) {
                    fail();
                    break;
                }
            }

            if (check_snapshot || p2p_stream_state(stream.get()) != last_state
                || (p2p_stream_input_ready(stream.get()) != 0) != input_ready) {
                if (!publish(false)) break;
            }
            if (check_snapshot) {
                int tool{};
                const auto next = p2p_stream_pending_agent_approval(stream.get(), &tool);
                if (next != agent_approval) {
                    if (next) {
                        if (p2p_stream_block_agent_prompt_input(stream.get(), 1) != P2P_OK) {
                            fail();
                            break;
                        }
                        input_blocked_for = next;
                    }
                    agent_approval = next;
                    if (on_agent_) on_agent_(next, tool);
                }
            }
        }
    }

    HANDLE wake_{};
    Callback on_update_;
    AgentCallback on_agent_;
    std::mutex agent_mutex_;
    std::optional<AgentAnswer> agent_answer_;
    std::atomic<int> agent_revoke_{};
    std::atomic<bool> display_changed_{false};
    std::atomic<bool> stopped_{false};
    bool started_{false};
    std::jthread thread_;
};

AccountWindow::AccountWindow(LaunchOptions options)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), options_(std::move(options)) {
    account_.reset(p2p_account_create());
    setWindowTitle(QStringLiteral("P2P Remote"));
    resize(740, 560);
    setMinimumSize(640, 490);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* shell = new QFrame(this);
    shell->setObjectName(QStringLiteral("framelessShell"));
    auto* shell_layout = new QVBoxLayout(shell);
    shell_layout->setContentsMargins(1, 1, 1, 1);
    shell_layout->setSpacing(0);

    title_bar_ = new TitleBar(this, QStringLiteral("P2P Remote"), true, true, shell);
    mode_button_ = new QPushButton(QStringLiteral("局域网直连"), title_bar_);
    mode_button_->setObjectName(QStringLiteral("titleToolBtn"));
    mode_button_->setFocusPolicy(Qt::NoFocus);
    mode_button_->setToolTip(QStringLiteral("无需服务器，局域网通过 IP 和固定被控码直连"));
    title_bar_->addToolWidget(mode_button_);
    connect(mode_button_, &QPushButton::clicked, this, &AccountWindow::toggleLanMode);
    auto* settings_btn = new QPushButton(QStringLiteral("⚙ 设置"), title_bar_);
    settings_btn->setObjectName(QStringLiteral("titleToolBtn"));
    settings_btn->setFocusPolicy(Qt::NoFocus);
    settings_btn->setToolTip(QStringLiteral("打开软件与插件设置"));
    title_bar_->addToolWidget(settings_btn);
    connect(settings_btn, &QPushButton::clicked, this, &AccountWindow::openSettings);
    shell_layout->addWidget(title_bar_);

    auto* body = new QWidget(shell);
    auto* root = new QVBoxLayout(body);
    root->setContentsMargins(26, 18, 26, 16);
    root->setSpacing(12);
    pages_ = new QStackedWidget(body);
    login_page_ = makeLoginPage();
    devices_page_ = makeDevicesPage();
    lan_page_ = makeLanPage();
    pages_->addWidget(login_page_);
    pages_->addWidget(devices_page_);
    pages_->addWidget(lan_page_);
    root->addWidget(pages_, 1);
    status_ = new QLabel(body);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    root->addWidget(status_);
    shell_layout->addWidget(body, 1);
    outer->addWidget(shell);

    if (!account_) {
        setStatus(winText(p2p_last_error()));
        login_page_->setEnabled(false);
        return;
    }

    account_event_.setHandle(p2p_account_event(account_.get()));
    account_event_.setEnabled(true);
    connect(&account_event_, &QWinEventNotifier::activated, this, &AccountWindow::completeRequest);
    refresh_.setInterval(3000);
    connect(&refresh_, &QTimer::timeout, this, &AccountWindow::refresh);
    lan_scan_timer_.setInterval(1500);
    connect(&lan_scan_timer_, &QTimer::timeout, this, &AccountWindow::refreshLanPeers);
    makeTray();
    elevated_ = isProcessElevated();
    refreshServiceStatus();
    connect(&service_refresh_, &QTimer::timeout, this, &AccountWindow::refreshServiceStatus);
    service_refresh_.start(1500);
    refreshScreens();
    loadCredentials();
    updateControls();
    (user_->text().isEmpty() ? user_ : password_)->setFocus();
}

AccountWindow::~AccountWindow() {
    service_refresh_.stop();
    if (service_status_thread_.joinable()) service_status_thread_.join();
    account_event_.setEnabled(false);
    closeSessions();
}

QWidget* AccountWindow::makeLoginPage() {
    auto* page = new QWidget(pages_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 4);
    layout->setSpacing(14);
    layout->addWidget(heading(QStringLiteral("登录 P2P Remote"), page));
    auto* notice = new QLabel(QStringLiteral("登录后可连接账号下的设备。添加本机并开启接收后，其他设备可连接本机。"), page);
    notice->setObjectName(QStringLiteral("muted"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto* card = new QFrame(page);
    card->setObjectName(QStringLiteral("accountCard"));
    auto* fields = new QFormLayout(card);
    fields->setContentsMargins(18, 18, 18, 18);
    fields->setHorizontalSpacing(18);
    fields->setVerticalSpacing(12);
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QSettings settings;
    server_ = new QLineEdit(settings.value(QStringLiteral("lastServer"), winText(p2p_default_server())).toString(), card);
    server_->setObjectName(QStringLiteral("server"));
    server_->setMaxLength(256);
    server_->setPlaceholderText(QStringLiteral("https://"));
    user_ = new QLineEdit(card);
    user_->setObjectName(QStringLiteral("user"));
    user_->setMaxLength(32);
    password_ = new QLineEdit(card);
    password_->setObjectName(QStringLiteral("password"));
    password_->setMaxLength(72);
    password_->setEchoMode(QLineEdit::Password);
    remember_ = new QCheckBox(QStringLiteral("记住密码"), card);
    fields->addRow(QStringLiteral("服务地址"), server_);
    fields->addRow(QStringLiteral("账号"), user_);
    fields->addRow(QStringLiteral("密码"), password_);
    fields->addRow(QString{}, remember_);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    login_ = new QPushButton(QStringLiteral("登录"), card);
    login_->setObjectName(QStringLiteral("primary"));
    register_ = new QPushButton(QStringLiteral("注册并登录"), card);
    buttons->addWidget(login_);
    buttons->addWidget(register_);
    buttons->addStretch();
    fields->addRow(QString{}, buttons);
    layout->addWidget(card);
    layout->addStretch();

    connect(login_, &QPushButton::clicked, this, [this] { signIn(false); });
    connect(register_, &QPushButton::clicked, this, [this] { signIn(true); });
    connect(password_, &QLineEdit::returnPressed, this, [this] { signIn(false); });
    connect(user_, &QLineEdit::returnPressed, password_, qOverload<>(&QLineEdit::setFocus));
    connect(server_, &QLineEdit::editingFinished, this, &AccountWindow::loadCredentials);
    connect(server_, &QLineEdit::textEdited, this, [this] {
        if (remember_->isChecked()) password_->clear();
        remember_->setChecked(false);
        loaded_server_.clear();
    });
    connect(user_, &QLineEdit::textEdited, this, [this] {
        if (remember_->isChecked()) password_->clear();
        remember_->setChecked(false);
    });
    connect(remember_, &QCheckBox::clicked, this, [this](bool checked) {
        if (!checked && !deleteCredentials(server_->text())) setStatus(QStringLiteral("无法删除 Windows 凭据。"));
    });
    return page;
}

QWidget* AccountWindow::makeDevicesPage() {
    auto* page = new QWidget(pages_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(10);
    auto* header = new QHBoxLayout;
    account_heading_ = heading(QStringLiteral("我的设备"), page);
    header->addWidget(account_heading_, 1);
    logout_ = new QPushButton(QStringLiteral("退出账号"), page);
    header->addWidget(logout_);
    layout->addLayout(header);

    auto* receiver = new QFrame(page);
    receiver->setObjectName(QStringLiteral("accountCard"));
    auto* local = new QVBoxLayout(receiver);
    local->setContentsMargins(14, 10, 14, 10);
    auto* actions = new QHBoxLayout;
    actions->addWidget(new QLabel(QStringLiteral("本机接收"), receiver));
    actions->addStretch();
    add_device_ = new QPushButton(QStringLiteral("添加本机"), receiver);
    actions->addWidget(add_device_);
    screens_ = new QComboBox(receiver);
    screens_->setAccessibleName(QStringLiteral("共享显示器"));
    actions->addWidget(screens_);
    receive_ = new QPushButton(receiver);
    actions->addWidget(receive_);
    local->addLayout(actions);
    host_status_ = new QLabel(receiver);
    host_status_->setTextFormat(Qt::PlainText);
    host_status_->setObjectName(QStringLiteral("muted"));
    host_status_->setWordWrap(true);
    local->addWidget(host_status_);

    auto* service_row = new QHBoxLayout;
    service_row->setSpacing(8);
    privilege_status_ = new QLabel(receiver);
    privilege_status_->setTextFormat(Qt::PlainText);
    privilege_status_->setObjectName(QStringLiteral("muted"));
    privilege_status_->setWordWrap(true);
    service_row->addWidget(privilege_status_, 1);
    elevate_button_ = new QPushButton(QStringLiteral("以管理员重启"), receiver);
    service_row->addWidget(elevate_button_);
    service_button_ = new QPushButton(QStringLiteral("启用系统服务"), receiver);
    service_row->addWidget(service_button_);
    local->addLayout(service_row);
    auto* agent_row = new QHBoxLayout;
    agent_status_ = new QLabel(QStringLiteral("AI Agent 诊断"), receiver);
    agent_status_->setObjectName(QStringLiteral("muted"));
    agent_row->addWidget(agent_status_, 1);
    auto* audit = new QPushButton(QStringLiteral("诊断记录…"), receiver);
    connect(audit, &QPushButton::clicked, this, &AccountWindow::openAgentAudit);
    agent_row->addWidget(audit);
    auto* stop_agent = new QPushButton(QStringLiteral("暂停诊断"), receiver);
    connect(stop_agent, &QPushButton::clicked, this, &AccountWindow::stopAgentDiagnostics);
    agent_row->addWidget(stop_agent);
    local->addLayout(agent_row);
    layout->addWidget(receiver);

    devices_ = new QListWidget(page);
    devices_->setAccessibleName(QStringLiteral("账号设备"));
    devices_->setObjectName(QStringLiteral("deviceList"));
    devices_->setSpacing(4);
    layout->addWidget(devices_, 1);
    auto* lower = new QHBoxLayout;
    connect_ = new QPushButton(QStringLiteral("连接设备"), page);
    connect_diagnostics_ = new QPushButton(QStringLiteral("只读诊断"), page);
    connect_->setObjectName(QStringLiteral("primary"));
    refresh_button_ = new QPushButton(QStringLiteral("刷新列表"), page);
    remove_device_ = new QPushButton(QStringLiteral("移除设备"), page);
    lower->addWidget(connect_);
    lower->addWidget(connect_diagnostics_);
    lower->addWidget(refresh_button_);
    lower->addWidget(remove_device_);
    lower->addStretch();
    layout->addLayout(lower);

    connect(logout_, &QPushButton::clicked, this, &AccountWindow::signOut);
    connect(receive_, &QPushButton::clicked, this, &AccountWindow::toggleReceiving);
    connect(service_button_, &QPushButton::clicked, this, &AccountWindow::toggleHostService);
    connect(elevate_button_, &QPushButton::clicked, this, &AccountWindow::relaunchAsAdmin);
    connect(refresh_button_, &QPushButton::clicked, this, &AccountWindow::refresh);
    connect(add_device_, &QPushButton::clicked, this, &AccountWindow::addLocalDevice);
    connect(remove_device_, &QPushButton::clicked, this, &AccountWindow::removeSelectedDevice);
    connect(connect_, &QPushButton::clicked, this, [this] { connectDevice(); });
    connect(connect_diagnostics_, &QPushButton::clicked, this, [this] { connectDevice(true); });
    connect(devices_, &QListWidget::itemDoubleClicked, this, [this] { connectDevice(); });
    connect(devices_, &QListWidget::itemSelectionChanged, this, &AccountWindow::updateControls);
    return page;
}

QWidget* AccountWindow::makeLanPage() {
    auto* page = new QWidget(pages_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(heading(QStringLiteral("局域网无人值守直连"), page));
    auto* notice = new QLabel(
        QStringLiteral("无需中心服务器与账号登录，局域网内可直接通过 IP 地址与固定被控码连接。"), page);
    notice->setObjectName(QStringLiteral("muted"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    // Card 1: Host unattended receiver
    auto* host_card = new QFrame(page);
    host_card->setObjectName(QStringLiteral("accountCard"));
    auto* host_layout = new QVBoxLayout(host_card);
    host_layout->setContentsMargins(14, 10, 14, 10);
    host_layout->setSpacing(8);

    auto* ip_row = new QHBoxLayout;
    ip_row->setSpacing(8);
    ip_row->addWidget(new QLabel(QStringLiteral("本机局域网被控"), host_card));
    lan_local_ips_ = new QLabel(host_card);
    lan_local_ips_->setObjectName(QStringLiteral("muted"));
    lan_local_ips_->setTextFormat(Qt::PlainText);
    lan_local_ips_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ip_row->addWidget(lan_local_ips_, 1);
    auto* copy_ip = new QPushButton(QStringLiteral("复制地址"), host_card);
    connect(copy_ip, &QPushButton::clicked, this, &AccountWindow::copyLanAddress);
    ip_row->addWidget(copy_ip);
    host_layout->addLayout(ip_row);

    auto* host_cfg = new QHBoxLayout;
    host_cfg->setSpacing(8);
    host_cfg->addWidget(new QLabel(QStringLiteral("端口"), host_card));
    lan_host_port_ = new QSpinBox(host_card);
    lan_host_port_->setRange(1024, 65535);
    lan_host_port_->setValue(QSettings().value(QStringLiteral("lan/hostPort"), P2P_LAN_DEFAULT_PORT).toInt());
    host_cfg->addWidget(lan_host_port_);

    host_cfg->addWidget(new QLabel(QStringLiteral("固定被控码"), host_card));
    lan_host_code_ = new QLineEdit(readLanSecret(QStringLiteral("HostAccessCode")), host_card);
    lan_host_code_->setMaxLength(32);
    lan_host_code_->setPlaceholderText(QStringLiteral("6–32 位固定被控码"));
    lan_host_code_->setEchoMode(QLineEdit::Password);
    host_cfg->addWidget(lan_host_code_, 1);

    auto* show_code = new QCheckBox(QStringLiteral("显示"), host_card);
    connect(show_code, &QCheckBox::toggled, this, [this](bool checked) {
        lan_host_code_->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    });
    host_cfg->addWidget(show_code);
    host_layout->addLayout(host_cfg);

    auto* host_ctrl = new QHBoxLayout;
    host_ctrl->setSpacing(8);
    lan_screens_ = new QComboBox(host_card);
    lan_screens_->setAccessibleName(QStringLiteral("局域网共享显示器"));
    host_ctrl->addWidget(lan_screens_, 1);
    lan_receive_ = new QPushButton(QStringLiteral("开启局域网接收"), host_card);
    connect(lan_receive_, &QPushButton::clicked, this, &AccountWindow::toggleLanReceiving);
    host_ctrl->addWidget(lan_receive_);
    host_layout->addLayout(host_ctrl);

    lan_host_status_ = new QLabel(QStringLiteral("本机局域网无人值守未开启。"), host_card);
    lan_host_status_->setObjectName(QStringLiteral("muted"));
    lan_host_status_->setTextFormat(Qt::PlainText);
    lan_host_status_->setWordWrap(true);
    host_layout->addWidget(lan_host_status_);
    layout->addWidget(host_card);

    // Card 2: Discovery + Viewer connect
    auto* peer_header = new QHBoxLayout;
    peer_header->addWidget(new QLabel(QStringLiteral("已发现的局域网设备（自动发现）"), page), 1);
    lan_scan_button_ = new QPushButton(QStringLiteral("重新扫描"), page);
    connect(lan_scan_button_, &QPushButton::clicked, this, [this] {
        if (lan_scanner_) p2p_lan_scanner_probe(lan_scanner_.get());
        refreshLanPeers();
        setStatus(QStringLiteral("正在扫描局域网在线被控设备…"));
    });
    peer_header->addWidget(lan_scan_button_);
    layout->addLayout(peer_header);

    lan_peers_ = new QListWidget(page);
    lan_peers_->setObjectName(QStringLiteral("deviceList"));
    lan_peers_->setSpacing(4);
    layout->addWidget(lan_peers_, 1);

    auto* connect_row = new QHBoxLayout;
    connect_row->setSpacing(8);
    lan_target_host_ = new QLineEdit(QSettings().value(QStringLiteral("lan/lastTarget")).toString(), page);
    lan_target_host_->setPlaceholderText(QStringLiteral("目标 IP 或 IP:端口（如 192.168.1.105）"));
    lan_target_host_->setMaxLength(128);
    connect_row->addWidget(lan_target_host_, 2);

    lan_target_code_ = new QLineEdit(page);
    lan_target_code_->setPlaceholderText(QStringLiteral("输入被控码"));
    lan_target_code_->setMaxLength(32);
    lan_target_code_->setEchoMode(QLineEdit::Password);
    connect_row->addWidget(lan_target_code_, 1);

    lan_connect_ = new QPushButton(QStringLiteral("直连"), page);
    lan_connect_->setObjectName(QStringLiteral("primary"));
    connect_row->addWidget(lan_connect_);
    layout->addLayout(connect_row);

    connect(lan_connect_, &QPushButton::clicked, this, &AccountWindow::connectLanDevice);
    connect(lan_target_code_, &QLineEdit::returnPressed, this, &AccountWindow::connectLanDevice);
    connect(lan_target_host_, &QLineEdit::returnPressed, lan_target_code_, qOverload<>(&QLineEdit::setFocus));
    connect(lan_peers_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const auto addr = item->data(Qt::UserRole).toString();
        const auto port = item->data(lan_port_role).toInt();
        lan_target_host_->setText(port == P2P_LAN_DEFAULT_PORT
            ? addr : QStringLiteral("%1:%2").arg(addr).arg(port));
        lan_target_code_->setFocus();
    });
    connect(lan_peers_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const auto addr = item->data(Qt::UserRole).toString();
        const auto port = item->data(lan_port_role).toInt();
        lan_target_host_->setText(port == P2P_LAN_DEFAULT_PORT
            ? addr : QStringLiteral("%1:%2").arg(addr).arg(port));
        if (!lan_target_code_->text().isEmpty()) connectLanDevice();
        else lan_target_code_->setFocus();
    });

    return page;
}

void AccountWindow::makeTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    QApplication::setQuitOnLastWindowClosed(false);
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("打开 P2P Remote"), this, &AccountWindow::restore);
    tray_receive_ = menu->addAction(QString{}, this, &AccountWindow::toggleReceiving);
    menu->addAction(QStringLiteral("AI Agent 诊断记录…"), this, &AccountWindow::openAgentAudit);
    menu->addAction(QStringLiteral("暂停 AI Agent 诊断"), this, &AccountWindow::stopAgentDiagnostics);
    menu->addAction(QStringLiteral("停止本机 Agent 共享"), this, [this] {
        if (viewer_) viewer_->stopAgentSharing();
    });
    menu->addAction(QStringLiteral("设置…"), this, &AccountWindow::openSettings);
    menu->addSeparator();
    menu->addAction(QStringLiteral("退出程序"), this, &AccountWindow::quit);
    tray_->setContextMenu(menu);
    tray_->setToolTip(QStringLiteral("P2P Remote"));
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restore();
    });
    tray_->show();
}

void AccountWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyWin32FramelessChrome(this);
    if (title_bar_) title_bar_->syncMaximizeState();
}

void AccountWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && title_bar_) {
        title_bar_->syncMaximizeState();
    }
}

void AccountWindow::closeEvent(QCloseEvent* event) {
    if (tray_ && !quitting_ && AppSettings::instance().data().close_to_tray) {
        event->ignore();
        hide();
    } else {
        closeSessions();
        event->accept();
        if (!quitting_) QApplication::quit();
    }
}

bool AccountWindow::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
    if (handleWin32FramelessChrome(this, message, result)) return true;
    const auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == stop_hotkey && (host_ || lan_host_)) {
        if (host_) stopReceiving();
        if (lan_host_) stopLanReceiving();
        setStatus(QStringLiteral("已用本机快捷键停止接收。"));
        *result = 0;
        return true;
    }
    if (msg->message == WM_DISPLAYCHANGE) {
        QMetaObject::invokeMethod(this, [this] {
            if (host_) host_->displayChanged();
            if (lan_host_) lan_host_->displayChanged();
            refreshScreens();
        }, Qt::QueuedConnection);
    }
    return QWidget::nativeEvent(type, message, result);
}

void AccountWindow::openSettings() {
    if (!settings_dialog_) {
        settings_dialog_ = new SettingsDialog(this);
        settings_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    settings_dialog_->show();
    settings_dialog_->raise();
    settings_dialog_->activateWindow();
}

void AccountWindow::restore() {
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
}

void AccountWindow::quit() {
    quitting_ = true;
    close();
    QApplication::quit();
}

void AccountWindow::setStatus(const QString& text) { status_->setText(text); }

void AccountWindow::updateControls() {
    if (!account_) return;
    const auto* local = p2p_account_device_id(account_.get());
    const bool signed_in = p2p_account_signed_in(account_.get()) != 0;
    const bool busy = request_ != Request::none;
    pages_->setCurrentWidget(lan_mode_ ? lan_page_ : (signed_in ? devices_page_ : login_page_));
    if (mode_button_) {
        mode_button_->setText(lan_mode_
            ? (signed_in ? QStringLiteral("我的设备") : QStringLiteral("账号登录"))
            : QStringLiteral("局域网直连"));
    }
    if (lan_local_ips_) {
        const auto ips = winText(p2p_lan_local_addresses());
        lan_local_ips_->setText(ips.isEmpty() ? QStringLiteral("未检测到局域网 IPv4") : ips);
    }
    if (lan_host_port_) lan_host_port_->setEnabled(!lan_host_);
    if (lan_host_code_) lan_host_code_->setEnabled(!lan_host_);
    if (lan_screens_) lan_screens_->setEnabled(!lan_host_ && lan_screens_->count() > 0);
    if (lan_receive_) {
        lan_receive_->setEnabled(lan_host_ || (lan_screens_ && lan_screens_->count() > 0));
        lan_receive_->setText(lan_host_ ? QStringLiteral("停止局域网接收") : QStringLiteral("开启局域网接收"));
    }
    if (lan_host_status_ && !lan_host_) {
        lan_host_status_->setText(QStringLiteral("本机局域网无人值守未开启。"));
    }
    login_page_->setEnabled(!busy);
    const auto* item = devices_->currentItem();
    const auto selected_id = item ? item->data(Qt::UserRole).toString() : QString{};
    const bool is_self_device = !selected_id.isEmpty() &&
        (selected_id == winText(local) || (service_info_.running && selected_id == service_info_.device_id));
    connect_->setEnabled(signed_in && item && item->data(device_state_role).toString() == QStringLiteral("online")
        && !is_self_device);
    connect_diagnostics_->setEnabled(connect_->isEnabled());
    refresh_button_->setEnabled(!busy);
    add_device_->setVisible(!local);
    add_device_->setEnabled(signed_in && !busy);
    remove_device_->setEnabled(signed_in && !busy && !selected_id.isEmpty());
    const bool service_active = service_info_.running;
    screens_->setEnabled(local && !host_ && !service_active);
    receive_->setVisible(!service_active);
    receive_->setEnabled(local && (screens_->count() || host_));
    const auto label = host_ ? QStringLiteral("停止接收") : QStringLiteral("开始接收");
    receive_->setText(label);
    if (tray_receive_) {
        tray_receive_->setText(service_active ? QStringLiteral("由系统服务托管中") : label);
        tray_receive_->setEnabled(signed_in && !service_active && receive_->isEnabled());
    }
    if (service_active) {
        host_status_->setText(service_info_.status.isEmpty()
            ? QStringLiteral("[系统服务] 后台已启动，正在等待接收状态。")
            : service_info_.status);
    } else if (!host_) {
        host_status_->setText(local ? QStringLiteral("本机已暂停接收。")
            : QStringLiteral("添加本机后，可开启接收远程连接。"));
    }

    const bool has_service_exe = serviceExecutableExists();
    service_button_->setVisible(has_service_exe || service_info_.installed);
    service_button_->setText(service_info_.installed ? QStringLiteral("卸载系统服务") : QStringLiteral("启用系统服务"));
    service_button_->setEnabled(local || service_info_.installed);
    elevate_button_->setVisible(!elevated_ && !service_active);
    if (service_active) {
        privilege_status_->setText(QStringLiteral("SYSTEM 系统服务已托管 · 支持任务管理器 / UAC / 锁屏 / 开机自启"));
    } else if (elevated_) {
        privilege_status_->setText(QStringLiteral("当前为管理员权限 · 支持任务管理器（如需支持 UAC/锁屏请启用系统服务）"));
    } else {
        privilege_status_->setText(QStringLiteral("当前为普通权限 · 任务管理器/UAC 会拦截输入，建议启用系统服务"));
    }
}

void AccountWindow::loadCredentials() {
    const auto server = server_->text().trimmed();
    if (server.isEmpty() || server == loaded_server_) return;
    loaded_server_ = server;
    if (auto saved = readCredentials(server)) {
        user_->setText(saved->user);
        password_->setText(QString::fromStdWString(saved->password));
        remember_->setChecked(true);
    }
}

void AccountWindow::signIn(bool create) {
    if (request_ != Request::none || p2p_account_signed_in(account_.get())) return;
    pending_.emplace(server_->text().trimmed(), user_->text(), password_->text().toStdWString(), remember_->isChecked());
    const auto endpoint = pending_->server.toStdWString();
    const auto user = pending_->user.toStdWString();
    if (p2p_account_login(account_.get(), endpoint.c_str(), user.c_str(), pending_->password.c_str(),
        create, options_.compatible_http, options_.relay_only) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        pending_.reset();
        return;
    }
    password_->clear();
    request_ = Request::login;
    setStatus(create ? QStringLiteral("正在注册账号…") : QStringLiteral("正在登录…"));
    updateControls();
}

void AccountWindow::refresh() {
    refreshServiceStatus();
    if (request_ != Request::none || !p2p_account_signed_in(account_.get())) return;
    if (p2p_account_refresh(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::devices;
    updateControls();
}

void AccountWindow::addLocalDevice() {
    if (request_ != Request::none || !p2p_account_signed_in(account_.get()) || p2p_account_device_id(account_.get())) return;
    if (p2p_account_add_local_device(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::add_device;
    setStatus(QStringLiteral("正在添加本机…"));
    updateControls();
}

void AccountWindow::removeSelectedDevice() {
    if (request_ != Request::none || !p2p_account_signed_in(account_.get())) return;
    const auto* item = devices_->currentItem();
    if (!item) return;
    const auto id = item->data(Qt::UserRole).toString().toStdWString();
    const auto name = item->data(device_name_role).toString();
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("移除设备"),
        QStringLiteral("确定移除「%1」？它将停止接收当前账号的远程连接，现有会话也会结束。").arg(name),
        QMessageBox::Yes | QMessageBox::No, this);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::No);
    refresh_.stop();
    const bool accepted = confirm.exec() == QMessageBox::Yes;
    refresh_.start();
    if (!accepted) return;
    if (p2p_account_remove_device(account_.get(), id.c_str()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::remove_device;
    setStatus(QStringLiteral("正在移除设备…"));
    updateControls();
}

void AccountWindow::completeRequest() {
    const bool was_registered = p2p_account_device_id(account_.get()) != nullptr;
    const auto result = p2p_account_complete(account_.get());
    if (result == P2P_PENDING) return;
    const auto request = std::exchange(request_, Request::none);
    if (result == P2P_OK) {
        if (request == Request::login) {
            QSettings().setValue(QStringLiteral("lastServer"), pending_->server);
            if (pending_->remember && !saveCredentials(*pending_)) {
                setStatus(QStringLiteral("已登录，无法保存 Windows 凭据。"));
            } else {
                setStatus(QString{});
            }
            account_heading_->setText(QStringLiteral("我的设备 · %1").arg(pending_->user));
            active_credentials_.emplace(pending_->server, pending_->user, pending_->password, pending_->remember);
            pending_.reset();
            refreshServiceStatus();
            if (!service_info_.running && AppSettings::instance().data().auto_receive_on_login) startReceiving();
            refresh_.start();
            refresh();
        } else if (request == Request::devices || request == Request::add_device || request == Request::remove_device) {
            if (was_registered && !p2p_account_device_id(account_.get())) {
                if (host_) stopReceiving();
                setStatus(QStringLiteral("本机已从账号移除，接收已停止。"));
            } else if (request == Request::add_device) {
                setStatus(QStringLiteral("已添加本机。可点击「开始接收」上线。"));
            } else if (request == Request::remove_device) {
                setStatus(QStringLiteral("已移除设备。"));
            }
            fillDevices();
            if (request != Request::devices) refresh();
        } else {
            setStatus(QStringLiteral("已退出账号。"));
        }
    } else {
        const auto error = winText(p2p_last_error());
        if (result == P2P_LOGIN_EXPIRED) {
            refresh_.stop();
            closeSessions();
            active_credentials_.reset();
            devices_->clear();
        }
        pending_.reset();
        setStatus(error);
    }
    updateControls();
}

void AccountWindow::signOut() {
    if (!p2p_account_signed_in(account_.get())) return;
    refresh_.stop();
    p2p_account_cancel(account_.get());
    request_ = Request::none;
    closeSessions();
    active_credentials_.reset();
    if (p2p_account_logout(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
    } else {
        request_ = Request::logout;
        setStatus(QStringLiteral("正在退出账号…"));
    }
    devices_->clear();
    loaded_server_.clear();
    loadCredentials();
    updateControls();
}

void AccountWindow::fillDevices() {
    const auto* current = devices_->currentItem();
    const auto selected = current ? current->data(Qt::UserRole).toString() : QString{};
    const auto local = winText(p2p_account_device_id(account_.get()));
    const auto count = p2p_account_device_count(account_.get());
    const QSignalBlocker block(devices_);
    for (size_t i = 0; i < count; ++i) {
        p2p_device device{};
        if (p2p_account_device(account_.get(), i, &device) != P2P_OK) continue;
        const auto id = winText(device.id);
        const auto state = winText(device.state);
        const auto name = winText(device.name);
        auto subtitle = winText(device.platform);
        if (id == local) subtitle += QStringLiteral("  ·  本机");
        else if (service_info_.running && id == service_info_.device_id) subtitle += QStringLiteral("  ·  本机(系统服务)");
        subtitle += state == QStringLiteral("online") ? QStringLiteral("  ·  可连接")
            : state == QStringLiteral("busy") ? QStringLiteral("  ·  控制中") : QStringLiteral("  ·  离线");
        const auto row = static_cast<int>(i);
        auto* item = row < devices_->count() ? devices_->item(row) : new QListWidgetItem(devices_);
        const auto label = name + QStringLiteral("\n") + subtitle;
        if (item->text() != label) item->setText(label);
        item->setData(Qt::UserRole, id);
        item->setData(device_state_role, state);
        item->setData(device_name_role, name);
        if (id == selected) devices_->setCurrentItem(item);
    }
    while (devices_->count() > static_cast<int>(count)) delete devices_->takeItem(devices_->count() - 1);
    if (!selected.isEmpty() && devices_->currentItem() && devices_->currentItem()->data(Qt::UserRole) != selected) {
        devices_->setCurrentItem(nullptr);
    }
}

void AccountWindow::refreshScreens() {
    if ((host_ && !host_->stopped()) || (lan_host_ && !lan_host_->stopped())) return;
    const auto previous = screens_->currentData();
    const auto lan_previous = lan_screens_ ? lan_screens_->currentData() : QVariant{};
    std::vector<Monitor> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, enumMonitors, reinterpret_cast<LPARAM>(&monitors))) return;
    screens_->clear();
    if (lan_screens_) lan_screens_->clear();
    int primary{};
    for (const auto& monitor : monitors) {
        if (monitor.primary) primary = screens_->count();
        const auto label = QStringLiteral("显示器 %1 · %2 × %3").arg(screens_->count() + 1)
            .arg(monitor.size.width()).arg(monitor.size.height());
        const auto screen_data = QVariant::fromValue(static_cast<qulonglong>(reinterpret_cast<quintptr>(monitor.handle)));
        screens_->addItem(label, screen_data);
        if (lan_screens_) lan_screens_->addItem(label, screen_data);
    }
    const auto match = screens_->findData(previous);
    screens_->setCurrentIndex(match >= 0 ? match : primary);
    if (lan_screens_) {
        const auto lan_match = lan_screens_->findData(lan_previous);
        lan_screens_->setCurrentIndex(lan_match >= 0 ? lan_match : primary);
    }
    updateControls();
}

void AccountWindow::toggleReceiving() {
    if (host_) stopReceiving();
    else startReceiving();
}

void AccountWindow::startReceiving() {
    if (!p2p_account_device_id(account_.get()) || host_ || service_info_.running || !screens_->count()) return;
    const auto screen = reinterpret_cast<void*>(static_cast<quintptr>(screens_->currentData().toULongLong()));
    const auto generation = ++host_generation_;
    QString error;
    auto session = std::make_unique<HostSession>(account_.get(), screen, options_,
        [this, generation](int state, const QString& status, bool desktop_control) {
            QMetaObject::invokeMethod(this, [this, generation, state, status, desktop_control] {
                if (generation != host_generation_ || !host_) return;
                auto text = status;
                if (desktop_control) {
                    text += hotkey_ ? QStringLiteral("\nCtrl+Alt+F11 可停止接收。")
                        : QStringLiteral("\n可在此处或在托盘中停止接收。");
                }
                if (host_status_->text() != text) host_status_->setText(text);
                if (state == P2P_STOPPED) {
                    stopReceiving();
                    setStatus(text);
                }
            }, Qt::QueuedConnection);
        }, [this, generation, screen](std::uint32_t id, int tool) {
            QMetaObject::invokeMethod(this, [this, generation, id, tool, screen] {
                showAgentApproval(id, tool, generation, screen);
            },
                Qt::QueuedConnection);
        }, &error);
    if (!session->valid()) {
        setStatus(error);
        return;
    }
    hotkey_ = RegisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey,
        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11) != FALSE;
    host_ = std::move(session);
    updateControls();
}

void AccountWindow::stopReceiving() {
    ++host_generation_;
    clearAgentApproval();
    host_.reset();
    if (hotkey_ && !lan_host_) {
        UnregisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey);
        hotkey_ = false;
    }
    updateControls();
}

void AccountWindow::toggleLanMode() {
    lan_mode_ = !lan_mode_;
    if (lan_mode_) {
        if (!lan_scanner_) lan_scanner_.reset(p2p_lan_scanner_create());
        if (lan_scanner_) p2p_lan_scanner_probe(lan_scanner_.get());
        lan_scan_timer_.start();
        refreshLanPeers();
    } else {
        lan_scan_timer_.stop();
    }
    updateControls();
}

void AccountWindow::startLanHostFromCli(const QString& code, std::uint16_t port) {
    if (!lan_mode_) toggleLanMode();
    if (port >= 1024) lan_host_port_->setValue(port);
    if (!code.isEmpty()) lan_host_code_->setText(code);
    startLanReceiving();
}

void AccountWindow::connectLanFromCli(const QString& target, const QString& code) {
    if (!lan_mode_) toggleLanMode();
    if (!target.isEmpty()) lan_target_host_->setText(target);
    if (!code.isEmpty()) lan_target_code_->setText(code);
    connectLanDevice();
}

void AccountWindow::refreshLanPeers() {
    if (!lan_scanner_ || !lan_peers_) return;
    const auto* current = lan_peers_->currentItem();
    const auto selected_addr = current ? current->data(Qt::UserRole).toString() : QString{};
    const auto selected_port = current ? current->data(lan_port_role).toInt() : 0;
    const auto count = p2p_lan_scanner_read(lan_scanner_.get());
    const QSignalBlocker block(lan_peers_);
    for (size_t i = 0; i < count; ++i) {
        p2p_lan_peer_v1 peer{};
        if (p2p_lan_scanner_peer(lan_scanner_.get(), i, &peer) != P2P_OK) continue;
        const auto name = winText(peer.name);
        const auto addr = winText(peer.address);
        const auto status = peer.busy ? QStringLiteral("控制中") : QStringLiteral("可连接");
        const auto label = QStringLiteral("%1\n%2:%3  ·  %4").arg(name, addr).arg(peer.port).arg(status);
        const auto row = static_cast<int>(i);
        auto* item = row < lan_peers_->count() ? lan_peers_->item(row) : new QListWidgetItem(lan_peers_);
        if (item->text() != label) item->setText(label);
        item->setData(Qt::UserRole, addr);
        item->setData(device_name_role, name);
        item->setData(lan_port_role, static_cast<int>(peer.port));
        item->setData(lan_busy_role, peer.busy != 0);
        if (addr == selected_addr && peer.port == selected_port) lan_peers_->setCurrentItem(item);
    }
    while (lan_peers_->count() > static_cast<int>(count)) delete lan_peers_->takeItem(lan_peers_->count() - 1);
}

void AccountWindow::toggleLanReceiving() {
    if (lan_host_) stopLanReceiving();
    else startLanReceiving();
}

void AccountWindow::startLanReceiving() {
    if (lan_host_ || !lan_screens_ || !lan_screens_->count()) return;
    const auto code = lan_host_code_->text();
    if (code.size() < 6 || code.size() > 32) {
        setStatus(QStringLiteral("请先设置 6 至 32 位固定被控码。"));
        lan_host_code_->setFocus();
        return;
    }
    const auto port = static_cast<std::uint16_t>(lan_host_port_->value());
    QSettings().setValue(QStringLiteral("lan/hostPort"), static_cast<int>(port));
    saveLanSecret(QStringLiteral("HostAccessCode"), code);

    const auto screen = reinterpret_cast<void*>(static_cast<quintptr>(lan_screens_->currentData().toULongLong()));
    const auto generation = ++lan_host_generation_;
    LanConnectOptions lan{{}, port, code.toStdWString()};
    QString error;
    auto session = std::make_unique<HostSession>(lan, screen, options_,
        [this, generation](int state, const QString& status, bool desktop_control) {
            QMetaObject::invokeMethod(this, [this, generation, state, status, desktop_control] {
                if (generation != lan_host_generation_ || !lan_host_) return;
                auto text = status;
                if (desktop_control) {
                    text += hotkey_ ? QStringLiteral("\nCtrl+Alt+F11 可停止接收。")
                        : QStringLiteral("\n可在此处停止局域网接收。");
                }
                if (lan_host_status_->text() != text) lan_host_status_->setText(text);
                if (state == P2P_STOPPED) {
                    stopLanReceiving();
                    setStatus(text);
                }
            }, Qt::QueuedConnection);
        }, &error);
    if (!session->valid()) {
        setStatus(error);
        return;
    }
    if (!hotkey_) {
        hotkey_ = RegisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey,
            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11) != FALSE;
    }
    lan_host_ = std::move(session);
    setStatus(QStringLiteral("已开启局域网无人值守接收（端口 %1）。").arg(port));
    updateControls();
}

void AccountWindow::stopLanReceiving() {
    ++lan_host_generation_;
    lan_host_.reset();
    if (hotkey_ && !host_) {
        UnregisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey);
        hotkey_ = false;
    }
    updateControls();
}

void AccountWindow::copyLanAddress() {
    const auto ips = winText(p2p_lan_local_addresses());
    const auto first = ips.section(QStringLiteral(","), 0, 0).trimmed();
    if (first.isEmpty()) {
        setStatus(QStringLiteral("未检测到可用的局域网 IPv4 地址。"));
        return;
    }
    const auto port = lan_host_port_->value();
    const auto text = port == P2P_LAN_DEFAULT_PORT
        ? first : QStringLiteral("%1:%2").arg(first).arg(port);
    if (auto* cb = QApplication::clipboard()) cb->setText(text);
    setStatus(QStringLiteral("已复制本机局域网地址：%1").arg(text));
}

void AccountWindow::connectLanDevice() {
    const auto raw = lan_target_host_->text().trimmed();
    const auto code = lan_target_code_->text();
    if (raw.isEmpty()) {
        setStatus(QStringLiteral("请输入目标设备的局域网 IP 地址，或在上方列表点选设备。"));
        lan_target_host_->setFocus();
        return;
    }
    if (code.size() < 6 || code.size() > 32) {
        setStatus(QStringLiteral("请输入 6 至 32 位被控码。"));
        lan_target_code_->setFocus();
        return;
    }
    QString host = raw;
    std::uint16_t port = P2P_LAN_DEFAULT_PORT;
    const int colon = raw.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && raw.indexOf(QLatin1Char(':')) == colon) {
        bool ok = false;
        const int parsed = raw.mid(colon + 1).toInt(&ok);
        if (!ok || parsed <= 0 || parsed > 65535) {
            setStatus(QStringLiteral("端口号无效，请填写 1–65535。"));
            lan_target_host_->setFocus();
            return;
        }
        host = raw.left(colon).trimmed();
        port = static_cast<std::uint16_t>(parsed);
    }
    if (host.isEmpty()) {
        setStatus(QStringLiteral("请输入有效的目标 IP 地址。"));
        return;
    }
    QSettings().setValue(QStringLiteral("lan/lastTarget"), raw);

    QString display_name;
    if (const auto* item = lan_peers_->currentItem();
        item && item->data(Qt::UserRole).toString() == host && item->data(lan_port_role).toInt() == port) {
        display_name = item->data(device_name_role).toString();
    }
    LanConnectOptions lan{host.toStdWString(), port, code.toStdWString()};
    if (!viewer_) viewer_ = new RemoteWindow(this);
    viewer_->openLanSession(lan, display_name, options_);
}

std::uint32_t AccountWindow::clearAgentApproval() {
    const auto previous = std::exchange(agent_prompt_id_, 0);
    if (auto* prompt = agent_prompt_.data()) {
        prompt->hide();
        prompt->deleteLater();
        agent_prompt_.clear();
    }
    if (agent_status_) agent_status_->setText(QStringLiteral("AI Agent 诊断"));
    return previous;
}

void AccountWindow::showAgentApproval(std::uint32_t id, int tool, std::uint64_t generation, void* monitor) {
    if (generation != host_generation_ || !host_ || id == agent_prompt_id_) return;
    const auto previous = clearAgentApproval();
    if (previous) host_->answerAgent(previous, false);
    if (!id) return;
    const bool screen = tool == P2P_AGENT_SCREEN_SNAPSHOT;
    auto* prompt = new QMessageBox(QMessageBox::Question, QStringLiteral("AI Agent 诊断"), screen
        ? QStringLiteral("已连接的主控请求截取当前共享显示器的一帧。是否批准？")
        : QStringLiteral("已连接的主控请求查看本机系统摘要。是否批准本次请求？"), QMessageBox::NoButton, this);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    prompt->setWindowFlag(Qt::WindowStaysOnTopHint);
    prompt->setInformativeText(screen
        ? QStringLiteral("批准后，将把所选显示器当前可见内容发送给主控端。请确认屏幕上可见的内容可以共享。")
        : QStringLiteral("摘要包括系统版本、硬件、开机时长、系统盘容量、主机名和被控程序运行账户及权限。"
        "\n批准仅对本次生效，未确认将自动拒绝。确认期间本会话的远程键鼠暂停。"));
    auto* approve = prompt->addButton(QStringLiteral("批准本次"), QMessageBox::AcceptRole);
    auto* reject = prompt->addButton(QStringLiteral("拒绝"), QMessageBox::RejectRole);
    prompt->setDefaultButton(reject);
    prompt->setEscapeButton(reject);
    agent_prompt_ = prompt;
    agent_prompt_id_ = id;
    agent_status_->setText(QStringLiteral("AI Agent 诊断 · 1 条待审批"));
    connect(prompt, &QMessageBox::finished, this, [this, generation, id, prompt, approve, screen, monitor] {
        if (generation != host_generation_ || agent_prompt_id_ != id || !host_) return;
        const bool approved = prompt->clickedButton() == approve;
        clearAgentApproval();
        host_->answerAgent(id, approved, approved && screen ? captureAgentScreen(monitor) : QByteArray{});
    });
    prompt->show();
    prompt->raise();
}

void AccountWindow::stopAgentDiagnostics() {
    const bool saved = AppSettings::savePluginConfig(QStringLiteral("ai-agent"),
        {{QStringLiteral("allow_host_access"), false}, {QStringLiteral("allow_screen_snapshot"), false}}, true);
    if (host_) {
        host_->revokeAgent(!saved);
        if (const auto id = clearAgentApproval()) host_->answerAgent(id, false);
    }
    const auto message = saved ? QStringLiteral("已暂停 AI Agent 诊断。可在设置中重新开启。")
        : QStringLiteral("无法保存 AI Agent 授权设置。");
    if (saved) setStatus(message);
    else QMessageBox::warning(this, QStringLiteral("AI Agent 诊断"), message);
}

void AccountWindow::openAgentAudit() {
    const auto directory = AppSettings::pluginConfigDirectory();
    QVector<QJsonObject> records;
    if (!directory.isEmpty()) {
        const QDir logs(directory);
        for (const auto& name : {QStringLiteral("ai-agent.audit.previous.jsonl"), QStringLiteral("ai-agent.audit.jsonl")}) {
            QFile file(logs.filePath(name));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) continue;
            for (const auto& line : file.read(256 * 1024).split('\n')) {
                const auto event = QJsonDocument::fromJson(line).object();
                if (event.value(QStringLiteral("tool")).toString() == QStringLiteral("system_info")) records.append(event);
            }
        }
    }
    auto* dialog = new FramelessDialog(QStringLiteral("AI Agent 诊断记录"), true, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(650, 370);
    const auto count = std::min<qsizetype>(records.size(), 200);
    if (!count) {
        dialog->contentLayout()->addWidget(new QLabel(QStringLiteral("暂无本机诊断记录。"), dialog));
    } else {
        auto* table = new QTableWidget(static_cast<int>(count), 5, dialog);
        table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("请求"), QStringLiteral("操作"), QStringLiteral("结果"),
            QStringLiteral("耗时 (ms)")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->hide();
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        const QHash<QString, QString> outcomes{
            {QStringLiteral("approved"), QStringLiteral("已批准")},
            {QStringLiteral("denied"), QStringLiteral("已拒绝")},
            {QStringLiteral("expired"), QStringLiteral("超时")},
            {QStringLiteral("revoked"), QStringLiteral("已撤销")},
            {QStringLiteral("cancelled"), QStringLiteral("主控已取消")},
            {QStringLiteral("disconnected"), QStringLiteral("会话已断开")},
            {QStringLiteral("unavailable"), QStringLiteral("不可用")}
        };
        for (int row = 0; row < count; ++row) {
            const auto& record = records[records.size() - 1 - row];
            const auto date = QDateTime::fromString(record.value(QStringLiteral("utc")).toString(), Qt::ISODate).toLocalTime();
            table->setItem(row, 0, new QTableWidgetItem(date.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            table->setItem(row, 1, new QTableWidgetItem(QString::number(record.value(QStringLiteral("id")).toInteger())));
            table->setItem(row, 2, new QTableWidgetItem(record.value(QStringLiteral("tool")).toString() ==
                QStringLiteral("screen_snapshot") ? QStringLiteral("屏幕截图") : QStringLiteral("系统摘要")));
            table->setItem(row, 3, new QTableWidgetItem(outcomes.value(record.value(QStringLiteral("result")).toString(),
                QStringLiteral("未知"))));
            table->setItem(row, 4, new QTableWidgetItem(QString::number(record.value(QStringLiteral("elapsed_ms")).toInteger())));
        }
        dialog->contentLayout()->addWidget(table);
    }
    dialog->show();
}

void AccountWindow::refreshServiceStatus() {
    auto status = queryHostServiceInfo();
    if (status.running != service_info_.running || status.installed != service_info_.installed) {
        ++service_status_generation_;
        service_info_ = status;
        updateControls();
    }
    if (service_info_.running && host_) {
        stopReceiving();
    }
    if (!status.running || service_status_pending_) return;
    service_status_pending_ = true;
    const auto generation = service_status_generation_;
    service_status_thread_ = std::jthread([this, generation, status = std::move(status)]() mutable {
        auto info = queryHostWorkerStatus(std::move(status));
        QMetaObject::invokeMethod(this, [this, generation, info = std::move(info)] {
            service_status_pending_ = false;
            if (generation != service_status_generation_) return;
            service_info_ = info;
            updateControls();
        }, Qt::QueuedConnection);
    });
}

void AccountWindow::toggleHostService() {
    QString error;
    if (service_info_.installed) {
        setStatus(QStringLiteral("正在停止并卸载系统服务…"));
        if (!uninstallHostService(&error)) {
            setStatus(error);
            return;
        }
        refreshServiceStatus();
        if (p2p_account_device_id(account_.get()) && !service_info_.running) {
            startReceiving();
        }
        setStatus(QStringLiteral("已卸载系统服务，已恢复绿色便携接收模式。"));
        updateControls();
        return;
    }

    const auto* device_id = p2p_account_device_id(account_.get());
    if (!active_credentials_ || !device_id) {
        setStatus(QStringLiteral("请先添加本机，再启用系统服务。"));
        return;
    }
    stopReceiving();
    setStatus(QStringLiteral("正在安装并启动系统服务（请在弹出的 UAC 窗口确认）…"));
    const int monitor_index = std::max(0, screens_->currentIndex());
    if (!installHostService(active_credentials_->server, active_credentials_->user, active_credentials_->password,
            winText(p2p_account_installation_id(account_.get())), winText(device_id), monitor_index,
            options_.compatible_http, options_.relay_only, &error)) {
        setStatus(error);
        refreshServiceStatus();
        if (!service_info_.running && p2p_account_device_id(account_.get())) startReceiving();
        updateControls();
        return;
    }
    refreshServiceStatus();
    setStatus(QStringLiteral("系统服务已启动并启用开机自启，正在等待后台接收状态。"));
    updateControls();
    refresh();
}

void AccountWindow::relaunchAsAdmin() {
    QString error;
    if (!relaunchClientElevated(&error)) {
        setStatus(error);
        return;
    }
    quit();
}

void AccountWindow::connectDevice(bool diagnostics) {
    if (!connect_->isEnabled()) return;
    const auto* item = devices_->currentItem();
    if (!viewer_) viewer_ = new RemoteWindow(this);
    viewer_->openSession(account_.get(), item->data(Qt::UserRole).toString(),
        item->data(device_name_role).toString(), options_, diagnostics);
}

void AccountWindow::closeSessions() {
    refresh_.stop();
    lan_scan_timer_.stop();
    lan_scanner_.reset();
    delete viewer_.data();
    stopReceiving();
    stopLanReceiving();
}
