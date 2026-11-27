#pragma once

#include <p2premote/core_api.h>

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPushButton;

enum class ConnectionVisualStage {
    signaling = 0,
    approval = 1,
    negotiating = 2,
    loading_video = 3,
    ready = 4,
    reconnecting = 5,
    stopped = 6
};

struct ConnectionStageInfo {
    ConnectionVisualStage stage{ConnectionVisualStage::signaling};
    int active_step{0};
    qreal target_progress{0.20};
    bool animating{true};
    bool warning_tone{false};
    bool error_tone{false};
    QString headline;
};

ConnectionStageInfo classifyConnectionStage(int p2p_state, bool has_video, const QString& status_text);

class ConnectionStageOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionStageOverlay(QWidget* parent = nullptr);

    void setSessionState(int p2p_state, bool has_video, const QString& status_text, bool can_reconnect);
    void refreshTheme();
    void advanceAnimation(qreal dt_sec);

    ConnectionVisualStage stage() const noexcept { return stage_info_.stage; }
    bool isAnimationRunning() const noexcept { return anim_timer_.isActive(); }
    qreal displayProgress() const noexcept { return display_progress_; }
    QPushButton* reconnectButton() const noexcept { return reconnect_btn_; }

signals:
    void reconnectRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum class OverlayPhase {
        hidden,
        connecting,
        stopped
    };

    void onAnimationTick();
    void syncAnimationTimer();
    int stoppedTextHeight(int max_width) const;
    void positionReconnectButton();

    QString status_text_;
    ConnectionStageInfo stage_info_;
    OverlayPhase phase_{OverlayPhase::hidden};

    bool initialized_{false};
    int last_state_{P2P_IDLE};
    bool last_has_video_{false};
    QString last_status_;
    bool last_can_reconnect_{false};

    QTimer anim_timer_;
    QElapsedTimer frame_clock_;
    qreal display_progress_{0.08};
    qreal spinner_angle_{0.0};
    qreal indeterminate_phase_{0.0};

    QPushButton* reconnect_btn_{};
};
