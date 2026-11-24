#include "account_window.h"
#include "app_settings.h"
#include "frameless_chrome.h"
#include "settings_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int stop_hotkey = 1;
constexpr int device_state_role = Qt::UserRole + 1;
constexpr int device_name_role = Qt::UserRole + 2;

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

} // namespace

class HostSession final {
public:
    using Callback = std::function<void(int state, const QString& status, bool desktop_control)>;

    HostSession(p2p_account* account, void* monitor, LaunchOptions options, Callback on_update, QString* error)
        : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), on_update_(std::move(on_update)) {
        if (!wake_) {
            if (error) *error = QStringLiteral("无法创建接收线程唤醒事件。");
            return;
        }
        std::promise<QString> started;
        auto ready = started.get_future();
        thread_ = std::jthread([this, account, monitor, options = std::move(options),
            started = std::move(started)](std::stop_token stop) mutable {
            run(stop, account, monitor, options, std::move(started));
        });
        const auto startup_error = ready.get();
        if (!startup_error.isEmpty()) {
            if (error) *error = startup_error;
            stop();
            return;
        }
        started_ = true;
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

private:
    void stop() {
        if (thread_.joinable()) {
            thread_.request_stop();
            if (wake_) SetEvent(wake_);
            thread_.join();
        }
    }

    void run(std::stop_token stop, p2p_account* account, void* monitor, const LaunchOptions& options,
        std::promise<QString> started) {
        const p2p_connection_options_v1 settings{
            options.ice_trace.c_str(), options.latency_trace.c_str(), options.stun.c_str(), options.stun_only, 1};
        std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream(
            p2p_stream_create(account, 1, nullptr, nullptr, &settings), p2p_stream_destroy);
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
        }
    }

    HANDLE wake_{};
    Callback on_update_;
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
    pages_->addWidget(login_page_);
    pages_->addWidget(devices_page_);
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
    layout->addWidget(receiver);

    devices_ = new QListWidget(page);
    devices_->setAccessibleName(QStringLiteral("账号设备"));
    devices_->setObjectName(QStringLiteral("deviceList"));
    devices_->setSpacing(4);
    layout->addWidget(devices_, 1);
    auto* lower = new QHBoxLayout;
    connect_ = new QPushButton(QStringLiteral("连接设备"), page);
    connect_->setObjectName(QStringLiteral("primary"));
    refresh_button_ = new QPushButton(QStringLiteral("刷新列表"), page);
    remove_device_ = new QPushButton(QStringLiteral("移除设备"), page);
    lower->addWidget(connect_);
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
    connect(connect_, &QPushButton::clicked, this, &AccountWindow::connectDevice);
    connect(devices_, &QListWidget::itemDoubleClicked, this, &AccountWindow::connectDevice);
    connect(devices_, &QListWidget::itemSelectionChanged, this, &AccountWindow::updateControls);
    return page;
}

void AccountWindow::makeTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    QApplication::setQuitOnLastWindowClosed(false);
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("打开 P2P Remote"), this, &AccountWindow::restore);
    tray_receive_ = menu->addAction(QString{}, this, &AccountWindow::toggleReceiving);
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
    if (!dwm_applied_) {
        dwm_applied_ = true;
        applyWin32FramelessChrome(this);
    }
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
    if (handleFramelessResizeHitTest(this, message, result)) return true;
    const auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == stop_hotkey && host_) {
        stopReceiving();
        setStatus(QStringLiteral("已用本机快捷键停止接收。"));
        *result = 0;
        return true;
    }
    if (msg->message == WM_DISPLAYCHANGE) {
        QMetaObject::invokeMethod(this, [this] {
            if (host_) host_->displayChanged();
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
    pages_->setCurrentWidget(signed_in ? devices_page_ : login_page_);
    login_page_->setEnabled(!busy);
    const auto* item = devices_->currentItem();
    const auto selected_id = item ? item->data(Qt::UserRole).toString() : QString{};
    const bool is_self_device = !selected_id.isEmpty() &&
        (selected_id == winText(local) || (service_info_.running && selected_id == service_info_.device_id));
    connect_->setEnabled(signed_in && item && item->data(device_state_role).toString() == QStringLiteral("online")
        && !is_self_device);
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
    if (host_ && !host_->stopped()) return;
    const auto previous = screens_->currentData();
    std::vector<Monitor> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, enumMonitors, reinterpret_cast<LPARAM>(&monitors))) return;
    screens_->clear();
    int primary{};
    for (const auto& monitor : monitors) {
        if (monitor.primary) primary = screens_->count();
        screens_->addItem(QStringLiteral("显示器 %1 · %2 × %3").arg(screens_->count() + 1)
            .arg(monitor.size.width()).arg(monitor.size.height()),
            QVariant::fromValue(static_cast<qulonglong>(reinterpret_cast<quintptr>(monitor.handle))));
    }
    const auto match = screens_->findData(previous);
    screens_->setCurrentIndex(match >= 0 ? match : primary);
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
    host_.reset();
    if (hotkey_) {
        UnregisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey);
        hotkey_ = false;
    }
    updateControls();
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

void AccountWindow::connectDevice() {
    if (!connect_->isEnabled()) return;
    const auto* item = devices_->currentItem();
    if (!viewer_) viewer_ = new RemoteWindow(this);
    viewer_->openSession(account_.get(), item->data(Qt::UserRole).toString(),
        item->data(device_name_role).toString(), options_);
}

void AccountWindow::closeSessions() {
    refresh_.stop();
    delete viewer_.data();
    stopReceiving();
}
