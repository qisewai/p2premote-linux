#pragma once

#include "stream_driver.h"

#include <QTimer>
#include <QWidget>

#include <vector>

class QFrame;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QVBoxLayout;
class RemoteSessionTab;

class RemoteWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteWindow(QWidget* parent = nullptr);
    ~RemoteWindow() override;

    void openSession(p2p_account* account, const QString& device, const QString& name,
        const LaunchOptions& options);

protected:
    void changeEvent(QEvent* event) override;

private:
    RemoteSessionTab* currentSession() const;
    RemoteSessionTab* sessionAt(int index) const;
    int findSession(const QString& device) const;
    void onTabChanged(int index);
    void onTabMoved(int from, int to);
    void closeTab(int index);
    void refreshChrome();
    void toggleFullscreen();
    void positionFullscreenToolbar();
    void showDisplayMenu();
    void showAudioMenu();
    void showPluginsMenu();
    void showActionsMenu();

    QVBoxLayout* layout_{};
    QFrame* topbar_{};
    QTabBar* tabs_{};
    QPushButton *add_tab_{}, *perf_pill_{};
    QPushButton *display_menu_{}, *audio_menu_{}, *plugins_menu_{}, *actions_menu_{}, *fullscreen_{};
    QStackedWidget* stack_{};
    QFrame* statusbar_{};
    QLabel* status_text_{};
    QTimer hover_;
    bool fullscreen_mode_{};
    bool was_maximized_{};
    bool show_diagnostics_{true};
    std::vector<RemoteSessionTab*> sessions_;
};
