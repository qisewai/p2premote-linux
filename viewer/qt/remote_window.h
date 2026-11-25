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
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTabBar;
class QVBoxLayout;
class RemoteSessionTab;
class SettingsDialog;

class RemoteWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteWindow(QWidget* parent = nullptr);
    ~RemoteWindow() override;

    void openSession(p2p_account* account, const QString& device, const QString& name,
        const LaunchOptions& options, bool diagnostics = false);
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
    void refreshPluginMenus(RemoteSessionTab* session);
    PluginMenuLayout::Invoke pluginMenuInvoker(RemoteSessionTab* session);
    void showPluginMenu(QPushButton* button, const QString& slot);
    void syncWindowControls();
    void openSettings();
    void toggleFullscreen();
    void positionFullscreenToolbar();
    void showDisplayMenu();
    void showAudioMenu();
    void showActionsMenu();
    void shareAgentDiagnostics();

    QFrame* shell_{};
    QVBoxLayout* layout_{};
    QFrame* topbar_{};
    QTabBar* tabs_{};
    QPushButton* add_tab_{};
    QPushButton *display_menu_{}, *audio_menu_{}, *input_menu_{}, *tools_menu_{}, *actions_menu_{}, *fullscreen_{};
    QHBoxLayout* plugin_toolbar_{};
    PluginMenuLayout plugin_menu_;
    std::vector<PluginMenuLayout::Button> plugin_buttons_;
    QPushButton *win_min_{}, *win_max_{}, *win_close_{};
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
