#pragma once

#include "credentials.h"
#include "remote_window.h"
#include "service_control.h"

#include <QPointer>
#include <QWidget>

#include <thread>

class QAbstractButton;
class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMessageBox;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class DeviceHeroWidget;
class HostSession;
class SettingsDialog;
class TitleBar;
class ToggleSwitch;

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
    enum class SelectionKind { none, lan, cloud };

    struct SelectedTarget {
        SelectionKind kind{SelectionKind::none};
        QString id;       // LAN: ip address; Cloud: device id
        QString name;
        QString state;    // "online" | "busy" | "offline"
        QString subtitle; // LAN: IP; Cloud: platform
        std::uint16_t port{P2P_LAN_DEFAULT_PORT};
        bool is_self{};
    };

    QWidget* makeSidebar(QWidget* parent);
    QWidget* makeUnattendedBanner(QWidget* parent);
    QWidget* makeDetailPage(QWidget* parent);
    QWidget* makeWelcomePage(QWidget* parent);
    QWidget* makeLoginPage(QWidget* parent);
    void makeTray();
    void restore();
    void quit();
    void openSettings();
    void setStatus(const QString& text);
    void updateControls();
    void updateUnattendedBanner();
    void updateDetailView();
    void showLoginPage();
    void showAccountMenu();
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
    void setUnattendedEnabled(bool enabled);
    void startLanReceiving(const QString& override_code = {}, std::uint16_t port = P2P_LAN_DEFAULT_PORT);
    void stopLanReceiving();
    void editUnattendedCode();
    void copyUnattendedInfo();
    void refreshLanPeers();
    void openLanConnectDialog(const QString& prefill_host = {}, const QString& prefill_name = {});
    void connectSelectedDevice(bool diagnostics = false);
    void showAgentApproval(std::uint32_t id, int tool, std::uint64_t generation, void* monitor);
    std::uint32_t clearAgentApproval();
    void stopAgentDiagnostics();
    void openAgentAudit();
    void refreshServiceStatus();
    void toggleHostService();
    void relaunchAsAdmin();
    void closeSessions();

    LaunchOptions options_;
    std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account_{nullptr, p2p_account_destroy};
    std::unique_ptr<HostSession> host_;
    uint64_t host_generation_{};
    std::unique_ptr<HostSession> lan_host_;
    uint64_t lan_host_generation_{};
    std::unique_ptr<p2p_lan_scanner, decltype(&p2p_lan_scanner_destroy)> lan_scanner_{nullptr, p2p_lan_scanner_destroy};
    QTimer lan_scan_timer_;
    QString lan_host_code_;
    std::uint16_t lan_host_port_{P2P_LAN_DEFAULT_PORT};
    bool lan_code_visible_{};
    QString lan_host_status_text_;
    bool lan_host_controlled_{};

    QPointer<RemoteWindow> viewer_;
    QPointer<SettingsDialog> settings_dialog_;
    TitleBar* title_bar_{};
    ToggleSwitch* unattended_switch_{};
    QPushButton* account_btn_{};
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
    QString host_status_text_;
    bool service_status_pending_{};
    bool elevated_{};
    QString loaded_server_;
    bool hotkey_{};
    bool quitting_{};
    SelectedTarget selected_;

    // Left sidebar
    QListWidget* lan_list_{};
    QListWidget* cloud_list_{};
    QPushButton* cloud_refresh_btn_{};
    QPushButton* cloud_login_sidebar_btn_{};

    // Right stage
    QFrame* unattended_banner_{};
    QLabel *unattended_pill_{}, *unattended_ip_label_{}, *unattended_code_label_{};
    QPushButton *unattended_toggle_code_btn_{}, *unattended_copy_btn_{}, *unattended_edit_btn_{};
    QComboBox* screens_{};
    QStackedWidget* right_stack_{};
    QWidget *detail_page_{}, *welcome_page_{}, *login_page_{};

    // Detail page widgets
    QLabel *detail_status_pill_{}, *detail_name_{};
    QPushButton* detail_more_btn_{};
    DeviceHeroWidget* hero_widget_{};
    QPushButton *strip_btn_1_{}, *strip_btn_2_{}, *strip_btn_3_{};
    QFrame* cloud_host_card_{};
    QLabel *cloud_host_status_{}, *privilege_status_{};
    QPushButton *add_device_{}, *receive_{}, *service_button_{}, *elevate_button_{};

    // Welcome page widgets
    QPushButton* welcome_login_btn_{};

    // Login page widgets
    QLineEdit *server_{}, *user_{}, *password_{};
    QCheckBox* remember_{};
    QPushButton *login_{}, *register_{};

    QLabel* status_{};
    QSystemTrayIcon* tray_{};
    QAction* tray_unattended_{};
    QAction* tray_receive_{};
};
