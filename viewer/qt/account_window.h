#pragma once

#include "credentials.h"
#include "remote_window.h"

#include <QPointer>
#include <QWidget>

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

class AccountWindow final : public QWidget {
public:
    explicit AccountWindow(LaunchOptions options);
    ~AccountWindow() override;
    bool valid() const { return account_ != nullptr; }

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

private:
    enum class Request { none, login, devices, logout };

    QWidget* makeLoginPage();
    QWidget* makeDevicesPage();
    void makeTray();
    void restore();
    void quit();
    void setStatus(const QString& text);
    void updateControls();
    void loadCredentials();
    void signIn(bool create);
    void completeRequest();
    void refresh();
    void signOut();
    void fillDevices();
    void refreshScreens();
    void toggleReceiving();
    void startReceiving();
    void stopReceiving();
    void connectDevice();
    void closeSessions();

    LaunchOptions options_;
    std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account_{nullptr, p2p_account_destroy};
    std::unique_ptr<HostSession> host_;
    uint64_t host_generation_{};
    QPointer<RemoteWindow> viewer_;
    QWinEventNotifier account_event_;
    QTimer refresh_;
    Request request_{Request::none};
    std::optional<Credentials> pending_;
    QString loaded_server_;
    bool hotkey_{};
    bool quitting_{};
    QStackedWidget* pages_{};
    QWidget *login_page_{}, *devices_page_{};
    QLineEdit *server_{}, *user_{}, *password_{};
    QCheckBox* remember_{};
    QLabel *status_{}, *account_heading_{}, *host_status_{};
    QListWidget* devices_{};
    QComboBox* screens_{};
    QPushButton *login_{}, *register_{}, *connect_{}, *receive_{}, *refresh_button_{}, *logout_{};
    QSystemTrayIcon* tray_{};
    QAction* tray_receive_{};
};
