#pragma once

#include "plugin_menu.h"
#include "stream_driver.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <vector>
#include <memory>

class AgentBroker;
class FramelessDialog;
class QFrame;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTabBar;
class QVBoxLayout;
class RemoteSessionTab;
class SettingsDialog;
class WindowCaptionButton;

class RemoteWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteWindow(QWidget* parent = nullptr);
    ~RemoteWindow() override;

    void openSession(p2p_account* account, const QString& device, const QString& name,
        const LaunchOptions& options, bool diagnostics = false);
    void openLanSession(const LanConnectOptions& lan, const QString& name, const LaunchOptions& options);
    void stopAgentSharing();

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

private:
    RemoteSessionTab* currentSession() const;
    RemoteSessionTab* sessionAt(int index) const;
    int findSession(const QString& device, bool diagnostics) const;
    void onTabChanged(int index);
    void onTabMoved(int from, int to);
    void closeTab(int index);
    void refreshChrome();
    PluginMenuLayout::Invoke pluginMenuInvoker(RemoteSessionTab* session);
    void popupToolbarMenu(QPushButton* button, QMenu* menu, bool align_right = false);
    void syncWindowControls();
    void openSettings();
    void toggleFullscreen();
    void positionFullscreenToolbar();
    void showControlCenterMenu();
    void populateDisplayMenu(QMenu& menu, RemoteSessionTab* session);
    void populateAudioMenu(QMenu& menu, RemoteSessionTab* session);
    void populateActionsMenu(QMenu& menu, RemoteSessionTab* session);
    void shareAgentDiagnostics();

    QFrame* shell_{};
    QVBoxLayout* layout_{};
    QFrame* topbar_{};
    QFrame* tab_strip_{};
    QTabBar* tabs_{};
    QPushButton* add_tab_{};
    QPushButton* control_center_{};
    QPushButton* exit_fullscreen_{};
    QFrame* caption_divider_{};
    PluginMenuLayout plugin_menu_;
    WindowCaptionButton *win_min_{}, *win_max_{}, *win_close_{};
    QStackedWidget* stack_{};
    QFrame* statusbar_{};
    QLabel* status_text_{};
    QPointer<SettingsDialog> settings_dialog_;
    QPointer<FramelessDialog> agent_dialog_;
    QPointer<RemoteSessionTab> agent_target_;
    std::unique_ptr<AgentBroker> agent_broker_;
    p2p_account* account_{};
    QTimer hover_;
    bool fullscreen_mode_{};
    bool was_maximized_{};
    std::vector<RemoteSessionTab*> sessions_;
};
