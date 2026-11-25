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
class QMessageBox;
class QPushButton;
class QSpinBox;
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
    void startLanHostFromCli(const QString& code, std::uint16_t port = P2P_LAN_DEFAULT_PORT);
    void connectLanFromCli(const QString& target, const QString& code);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

private:
    enum class Request { none, login, devices, add_device, remove_device, logout };

    QWidget* makeLoginPage();
    QWidget* makeDevicesPage();
    QWidget* makeLanPage();
    void toggleLanMode();
    void refreshLanPeers();
    void toggleLanReceiving();
    void startLanReceiving();
    void stopLanReceiving();
    void connectLanDevice();
    void copyLanAddress();
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
    void showAgentApproval(std::uint32_t id, int tool, std::uint64_t generation, void* monitor);
    std::uint32_t clearAgentApproval();
    void stopAgentDiagnostics();
    void openAgentAudit();
    void refreshServiceStatus();
    void toggleHostService();
    void relaunchAsAdmin();
    void connectDevice(bool diagnostics = false);
    void closeSessions();

    LaunchOptions options_;
    std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account_{nullptr, p2p_account_destroy};
    std::unique_ptr<HostSession> host_;
    uint64_t host_generation_{};
    std::unique_ptr<HostSession> lan_host_;
    uint64_t lan_host_generation_{};
    std::unique_ptr<p2p_lan_scanner, decltype(&p2p_lan_scanner_destroy)> lan_scanner_{nullptr, p2p_lan_scanner_destroy};
    QTimer lan_scan_timer_;
    bool lan_mode_{};
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
    std::uint32_t agent_prompt_id_{};
    QPointer<QMessageBox> agent_prompt_;
    QLabel* agent_status_{};
    bool service_status_pending_{};
    bool elevated_{};
    QString loaded_server_;
    bool hotkey_{};
    bool quitting_{};
    QStackedWidget* pages_{};
    QWidget *login_page_{}, *devices_page_{}, *lan_page_{};
    QPushButton* mode_button_{};
    QLabel *lan_local_ips_{}, *lan_host_status_{};
    QSpinBox* lan_host_port_{};
    QComboBox* lan_screens_{};
    QLineEdit *lan_host_code_{}, *lan_target_host_{}, *lan_target_code_{};
    QPushButton *lan_receive_{}, *lan_connect_{}, *lan_scan_button_{};
    QListWidget* lan_peers_{};
    QLineEdit *server_{}, *user_{}, *password_{};
    QCheckBox* remember_{};
    QLabel *status_{}, *account_heading_{}, *host_status_{}, *privilege_status_{};
    QListWidget* devices_{};
    QComboBox* screens_{};
    QPushButton *login_{}, *register_{}, *connect_{}, *receive_{}, *refresh_button_{}, *logout_{};
    QPushButton *add_device_{}, *remove_device_{};
    QPushButton* connect_diagnostics_{};
    QPushButton *service_button_{}, *elevate_button_{};
    QSystemTrayIcon* tray_{};
    QAction* tray_receive_{};
};
