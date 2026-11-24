#pragma once

#include "credentials.h"
#include "remote_window.h"
#include "service_control.h"

#include <QPointer>
#include <QWidget>

#include <thread>

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class HostSession;
class SettingsDialog;
class TitleBar;

class AccountWindow final : public QWidget {
public:
    explicit AccountWindow(LaunchOptions options);
    ~AccountWindow() override;
    bool valid() const { return account_ != nullptr; }

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

private:
    enum class Request { none, login, devices, add_device, remove_device, logout };

    QWidget* makeLoginPage();
    QWidget* makeDevicesPage();
    void makeTray();
    void restore();
    void quit();
    void openSettings();
    void setStatus(const QString& text);
    void updateControls();
    void loadCredentials();
    void signIn(bool create);
    void completeRequest();
    void refresh();
    void addLocalDevice();
    void removeSelectedDevice();
    void signOut();
    void fillDevices();
    void refreshScreens();
    void toggleReceiving();
    void startReceiving();
    void stopReceiving();
    void refreshServiceStatus();
    void toggleHostService();
    void relaunchAsAdmin();
    void connectDevice();
    void closeSessions();

    LaunchOptions options_;
    std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account_{nullptr, p2p_account_destroy};
    std::unique_ptr<HostSession> host_;
    uint64_t host_generation_{};
    QPointer<RemoteWindow> viewer_;
    QPointer<SettingsDialog> settings_dialog_;
    TitleBar* title_bar_{};
    QWinEventNotifier account_event_;
    QTimer refresh_;
    Request request_{Request::none};
    std::optional<Credentials> pending_;
    std::optional<Credentials> active_credentials_;
    HostServiceInfo service_info_;
    QTimer service_refresh_;
    std::jthread service_status_thread_;
    std::uint64_t service_status_generation_{};
    bool service_status_pending_{};
    bool elevated_{};
    QString loaded_server_;
    bool hotkey_{};
    bool quitting_{};
    QStackedWidget* pages_{};
    QWidget *login_page_{}, *devices_page_{};
    QLineEdit *server_{}, *user_{}, *password_{};
    QCheckBox* remember_{};
    QLabel *status_{}, *account_heading_{}, *host_status_{}, *privilege_status_{};
    QListWidget* devices_{};
    QComboBox* screens_{};
    QPushButton *login_{}, *register_{}, *connect_{}, *receive_{}, *refresh_button_{}, *logout_{};
    QPushButton *add_device_{}, *remove_device_{};
    QPushButton *service_button_{}, *elevate_button_{};
    QSystemTrayIcon* tray_{};
    QAction* tray_receive_{};
};
