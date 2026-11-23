#pragma once

#include "stream_driver.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class PluginDialogManager;

class RemoteWindow final : public QWidget {
public:
    RemoteWindow(p2p_account* account, const QString& device, const QString& name,
        const LaunchOptions& options, QWidget* parent);
    ~RemoteWindow() override;
    void start();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void showState(const p2p_stream_snapshot_v1& snapshot);
    void syncVideo();
    void toggleFullscreen();
    void positionFullscreenToolbar();
    void showPlugins();
    void exportCsv();

    std::unique_ptr<StreamDriver> driver_;
    PluginDialogManager* dialogs_{};
    QVBoxLayout* layout_{};
    QWidget* toolbar_{};
    QWidget* video_{};
    QLabel* status_{};
    QLabel* metrics_{};
    QPushButton *connect_{}, *stop_{}, *plugins_{}, *fullscreen_{}, *export_{};
    QComboBox *quality_{}, *bitrate_{};
    QCheckBox* diagnostics_{};
    QTimer hover_;
    bool fullscreen_mode_{};
    bool was_maximized_{};
    bool playing_{};
    QString metrics_text_;
    double previous_seconds_{}, fps_{};
    uint64_t previous_frames_{};
};
