#pragma once

#include "credentials.h"
#include "remote_window.h"
#include "service_control.h"

#include <QElapsedTimer>
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
class QSocketNotifier;
class QSystemTrayIcon;
class DeviceHeroWidget;
class SettingsDialog;
class TitleBar;
class HostSession;
class ToggleSwitch;

class AccountWindow final : public QWidget {
public:
    explicit AccountWindow(LaunchOptions options);
    ~AccountWindow() override;
    bool valid() const { return account_ != nullptr; }
#if defined(Q_OS_WIN)
    void startLanHostFromCli(const QString& code, std::uint16_t port = P2P_LAN_DEFAULT_PORT);
#endif
    void connectLanFromCli(const QString& target, const QString& code);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

private:
    enum class Request { none, login, devices, add_device, remove_device, logout };

    struct SelectedDevice {
        QString id;
        QString name;
        QString state;    // "online" | "busy" | "offline"
        QString platform;
        bool is_self{};
        bool valid() const { return !id.isEmpty(); }
    };

    QWidget* makeSidebar(QWidget* parent);
    QWidget* makeLocalPage(QWidget* parent);
    QWidget* makeDetailPage(QWidget* parent);
    void makeLoginPopup();
    void makeTray();
    void restore();
    void quit();
    void openSettings();
    void setStatus(const QString& text, bool persistent = false);
    void updateControls();
    void updateLocalPage();
    void updateLocalAddresses();
    void updateDetailView();
    void showLoginPopup();
    void showAccountMenu();
    void loadCredentials();
    void signIn(bool create);
    void completeRequest();
    void refresh();
    void addLocalDevice();
    void removeSelectedDevice();
    void signOut();
    void finishSignOut();
    void fillDevices();
    void refreshScreens();
    void setHostServiceEnabled(bool enabled);
    void setLanAllowEnabled(bool enabled);
    void setUnattendedEnabled(bool enabled);
    void startClientAliveBeacon();
    void stopClientAliveBeacon();
    void syncCloudHostIfEnabled(bool enable_cloud, std::function<void(bool)> completed = {});
    void editLanAccessCode();
    void copyLanConnectionInfo();
    void openLanConnectDialog();
    void connectSelectedDevice(bool diagnostics = false);
    void stopAgentDiagnostics();
    void openAgentAudit();
    void refreshServiceStatus();
#if defined(Q_OS_LINUX)
    void changeLinuxHostService(bool enabled, QString code = {});
    void changeLinuxCloudReceiving(bool enabled, std::function<void(bool)> completed = {});
#else
    bool installOrUpdateHostService(HostConfigUpdate update, bool enable_lan, bool enable_cloud, QString* error);
#endif
    void closeSessions();

    LaunchOptions options_;
    std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account_{nullptr, p2p_account_destroy};
    QString lan_host_code_;
    std::uint16_t lan_host_port_{P2P_LAN_DEFAULT_PORT};
    bool lan_code_visible_{true};

    QPointer<RemoteWindow> viewer_;
    QPointer<SettingsDialog> settings_dialog_;
    TitleBar* title_bar_{};
    QPushButton* account_btn_{};
#if defined(Q_OS_WIN)
    QWinEventNotifier account_event_;
#else
    QTimer account_event_;
#endif
    QTimer refresh_;
    QTimer status_clear_timer_;
    Request request_{Request::none};
    bool add_device_after_refresh_{};
    std::optional<Credentials> pending_;
    std::optional<Credentials> active_credentials_;
    HostServiceInfo service_info_;
    QTimer service_refresh_;
    QElapsedTimer service_status_age_;
    std::jthread service_status_thread_;
    std::uint64_t service_status_generation_{};
    bool service_status_pending_{};
#if defined(Q_OS_LINUX)
    bool service_operation_pending_{};
#endif
    QString loaded_server_;
    bool quitting_{};
    bool viewing_local_{true};
    SelectedDevice selected_;

    // Left sidebar
    QPushButton* local_nav_btn_{};
    QListWidget* cloud_list_{};
    QPushButton* cloud_refresh_btn_{};

    // Right stage
    QStackedWidget* right_stack_{};
    QWidget *local_page_{}, *detail_page_{};

    // Local machine page widgets
    QLabel *local_title_{}, *local_status_pill_{};
    QFrame* local_host_card_{};
    ToggleSwitch* host_enable_switch_{};
    QLabel* local_host_desc_{};
    QWidget *host_active_area_{}, *host_off_hint_{};
    ToggleSwitch* lan_allow_switch_{};
    QLabel* local_lan_desc_{};
    QWidget *lan_info_area_{}, *lan_off_hint_{};
    QCheckBox* unattended_check_{};
    QLabel *local_ip_value_{}, *local_ip_extra_{}, *local_code_value_{};
    QPushButton *lan_toggle_code_btn_{}, *lan_copy_btn_{}, *lan_edit_btn_{};
    QComboBox* screens_{};
    QLabel *local_summary_ip_{}, *local_summary_display_{}, *privilege_status_{};
#if defined(Q_OS_LINUX)
    QLabel* local_summary_account_{};
#endif
    QWidget* cloud_host_section_{};
    QLabel *cloud_host_status_{}, *cloud_guest_hint_{};
    QPushButton* add_device_{};

    // Remote detail page widgets
    QLabel *detail_status_pill_{}, *detail_name_{};
    QPushButton* detail_more_btn_{};
    DeviceHeroWidget* hero_widget_{};
    QPushButton *strip_btn_1_{}, *strip_btn_2_{}, *strip_btn_3_{};

    // Login popup menu widgets
    QFrame* login_popup_{};
    QLabel* login_popup_status_{};
    QElapsedTimer login_popup_closed_;
    QLineEdit *server_{}, *user_{}, *password_{};
    QCheckBox* remember_{};
    QPushButton *login_{}, *register_{};

    QLabel* status_{};
    QSystemTrayIcon* tray_{};
    QAction* tray_host_enable_{};
    QAction* tray_lan_allow_{};
#if defined(Q_OS_WIN)
    QPushButton* upgrade_service_{};
    void updateForegroundHost(const HostServiceInfo& info);
    void stopForegroundHost();
    std::uint32_t clearAgentApproval();
    void showAgentApproval(std::uint32_t id, int tool, std::uint64_t generation, void* monitor);
    std::unique_ptr<HostSession> host_;
    QString host_lease_, foreground_status_;
    int foreground_state_{};
    bool foreground_control_{};
    std::uint64_t host_generation_{};
    QPointer<QMessageBox> agent_prompt_;
    std::uint32_t agent_prompt_id_{};
#else
    QSocketNotifier* client_notifier_{};
    int client_alive_fd_{-1};
#endif
};
