#include "connection_stage_overlay.h"
#include "app_theme.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPushButton>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int kAnimationIntervalMs = 16;

ThemePalette safeThemePalette() {
    const auto& current = AppTheme::current();
    if (!current.bg.isEmpty() && !current.surface.isEmpty() && !current.accent.isEmpty()) {
        return current;
    }
    return AppTheme::paletteFor(AppSettingsData{});
}

bool containsErrorKeyword(const QString& text) {
    static const std::array<QString, 9> kErrorKeywords = {
        QStringLiteral("失败"),
        QStringLiteral("超时"),
        QStringLiteral("错误"),
        QStringLiteral("无效"),
        QStringLiteral("拒绝"),
        QStringLiteral("缺少"),
        QStringLiteral("失效"),
        QStringLiteral("不匹配"),
        QStringLiteral("无法")
    };
    return std::ranges::any_of(kErrorKeywords, [&](const QString& word) {
        return text.contains(word);
    });
}

} // namespace

ConnectionStageInfo classifyConnectionStage(int p2p_state, bool has_video, const QString& status_text) {
    switch (p2p_state) {
    case P2P_STREAMING:
        if (has_video) {
            return {
                ConnectionVisualStage::ready,
                4,
                1.0,
                false,
                false,
                false,
                QStringLiteral("画面已就绪")
            };
        }
        {
            const bool delayed = status_text.contains(QStringLiteral("暂未收到"))
                || status_text.contains(QStringLiteral("超时"));
            return {
                ConnectionVisualStage::loading_video,
                3,
                0.88,
                true,
                delayed,
                false,
                delayed ? QStringLiteral("暂未收到远程画面") : QStringLiteral("正在加载远程画面")
            };
        }

    case P2P_NEGOTIATING:
        return {
            ConnectionVisualStage::negotiating,
            2,
            0.68,
            true,
            false,
            false,
            QStringLiteral("正在建立加密通道")
        };

    case P2P_APPROVAL:
        if (status_text.contains(QStringLiteral("信令"))) {
            return {
                ConnectionVisualStage::signaling,
                0,
                0.20,
                true,
                false,
                false,
                QStringLiteral("正在连接信令服务")
            };
        }
        return {
            ConnectionVisualStage::approval,
            1,
            0.44,
            true,
            true,
            false,
            QStringLiteral("等待被控端授权")
        };

    case P2P_WAITING:
    case P2P_IDLE:
        if (status_text.contains(QStringLiteral("验证"))
            || status_text.contains(QStringLiteral("认证"))
            || status_text.contains(QStringLiteral("授权"))) {
            return {
                ConnectionVisualStage::approval,
                1,
                0.42,
                true,
                false,
                false,
                QStringLiteral("正在验证连接身份")
            };
        }
        return {
            ConnectionVisualStage::signaling,
            0,
            0.20,
            true,
            false,
            false,
            QStringLiteral("正在连接远程设备")
        };

    case P2P_RECONNECTING:
        return {
            ConnectionVisualStage::reconnecting,
            0,
            0.35,
            true,
            true,
            false,
            QStringLiteral("正在尝试重新连接")
        };

    case P2P_STOPPED:
    default: {
        const bool is_error = containsErrorKeyword(status_text);
        return {
            ConnectionVisualStage::stopped,
            -1,
            0.0,
            false,
            false,
            is_error,
            is_error ? QStringLiteral("连接未能建立") : QStringLiteral("远程连接已断开")
        };
    }
    }
}

ConnectionStageOverlay::ConnectionStageOverlay(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("connectionStageOverlay"));
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
#if defined(Q_OS_WIN)
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif

    reconnect_btn_ = new QPushButton(QStringLiteral("重新连接"), this);
    reconnect_btn_->setObjectName(QStringLiteral("primary"));
    reconnect_btn_->setCursor(Qt::PointingHandCursor);
    reconnect_btn_->setFocusPolicy(Qt::NoFocus);
    reconnect_btn_->setFixedSize(128, 32);
    reconnect_btn_->hide();
    connect(reconnect_btn_, &QPushButton::clicked, this, &ConnectionStageOverlay::reconnectRequested);

    anim_timer_.setInterval(kAnimationIntervalMs);
    anim_timer_.setTimerType(Qt::PreciseTimer);
    connect(&anim_timer_, &QTimer::timeout, this, &ConnectionStageOverlay::onAnimationTick);
}

void ConnectionStageOverlay::setSessionState(int p2p_state, bool has_video, const QString& status_text, bool can_reconnect) {
    if (initialized_ && p2p_state == last_state_ && has_video == last_has_video_
        && status_text == last_status_ && can_reconnect == last_can_reconnect_) {
        return;
    }
    initialized_ = true;
    last_state_ = p2p_state;
    last_has_video_ = has_video;
    last_status_ = status_text;
    last_can_reconnect_ = can_reconnect;

    const auto next = has_video
        ? ConnectionStageInfo{ConnectionVisualStage::ready, 4, 1.0, false, false, false, QStringLiteral("画面已就绪")}
        : classifyConnectionStage(p2p_state, false, status_text);
    status_text_ = status_text.isEmpty() ? next.headline : status_text;

    if (next.stage == ConnectionVisualStage::ready) {
        anim_timer_.stop();
        phase_ = OverlayPhase::hidden;
        stage_info_ = next;
        reconnect_btn_->hide();
        hide();
        return;
    }

    if (next.stage == ConnectionVisualStage::stopped) {
        anim_timer_.stop();
        phase_ = OverlayPhase::stopped;
        stage_info_ = next;
        reconnect_btn_->setVisible(can_reconnect);
        positionReconnectButton();
        show();
        raise();
        update();
        return;
    }

    if (phase_ != OverlayPhase::connecting) {
        display_progress_ = 0.08;
        spinner_angle_ = 0.0;
        indeterminate_phase_ = 0.0;
        reconnect_btn_->hide();
    }

    phase_ = OverlayPhase::connecting;
    stage_info_ = next;
    show();
    raise();
    syncAnimationTimer();
    update();
}

void ConnectionStageOverlay::refreshTheme() {
    if (isVisible()) update();
}

void ConnectionStageOverlay::onAnimationTick() {
    const qint64 elapsed_ns = frame_clock_.isValid() ? frame_clock_.restart() : (kAnimationIntervalMs * 1'000'000LL);
    if (!frame_clock_.isValid()) frame_clock_.start();
    const qreal dt = static_cast<qreal>(elapsed_ns) / 1'000'000'000.0;
    advanceAnimation(dt);
}

void ConnectionStageOverlay::advanceAnimation(qreal dt_sec) {
    if (phase_ != OverlayPhase::connecting) {
        anim_timer_.stop();
        return;
    }

    const qreal dt = std::clamp(dt_sec, qreal{0.001}, qreal{0.05});
    spinner_angle_ = std::fmod(spinner_angle_ + dt * 300.0, 360.0);
    indeterminate_phase_ = std::fmod(indeterminate_phase_ + dt * 0.65, 1.0);
    display_progress_ += (stage_info_.target_progress - display_progress_) * (1.0 - std::exp(-dt * 8.0));
    update();
}

void ConnectionStageOverlay::syncAnimationTimer() {
    if (isVisible() && phase_ == OverlayPhase::connecting) {
        frame_clock_.restart();
        if (!anim_timer_.isActive()) anim_timer_.start();
    } else {
        anim_timer_.stop();
    }
}

int ConnectionStageOverlay::stoppedTextHeight(int max_width) const {
    const auto theme = safeThemePalette();
    QFont text_font = font();
    text_font.setPixelSize(theme.font_size);
    const QRect bounds = QFontMetrics(text_font).boundingRect(
        QRect(0, 0, std::max(120, max_width), 96), Qt::AlignCenter | Qt::TextWordWrap, status_text_);
    return std::clamp(bounds.height(), 22, 72);
}

void ConnectionStageOverlay::positionReconnectButton() {
    if (!reconnect_btn_) return;
    const int w = std::max(1, width());
    const int h = std::max(1, height());
    const int text_w = std::min(360, std::max(160, w - 48));
    const int text_h = stoppedTextHeight(text_w);
    const int total_h = text_h + 18 + reconnect_btn_->height();
    const int top_y = (h - total_h) / 2;
    const int bx = (w - reconnect_btn_->width()) / 2;
    const int by = top_y + text_h + 18;
    reconnect_btn_->move(bx, by);
}

void ConnectionStageOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    positionReconnectButton();
}

void ConnectionStageOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    positionReconnectButton();
    syncAnimationTimer();
}

void ConnectionStageOverlay::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    anim_timer_.stop();
}

void ConnectionStageOverlay::paintEvent(QPaintEvent*) {
    if (phase_ == OverlayPhase::hidden) return;

    const auto theme = safeThemePalette();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.fillRect(rect(), QColor(theme.bg));

    const QColor accent_color(theme.accent);
    const QColor warning_color(0xf5, 0x9e, 0x0b);
    const QColor danger_color(theme.danger);
    const QColor primary_tone = stage_info_.error_tone ? danger_color
        : (stage_info_.warning_tone ? warning_color : accent_color);

    const qreal w = static_cast<qreal>(std::max(1, width()));
    const qreal h = static_cast<qreal>(std::max(1, height()));
    const qreal cx = w * 0.5;
    const qreal text_w = std::min<qreal>(360.0, std::max<qreal>(160.0, w - 48.0));

    QFont text_font = font();
    text_font.setPixelSize(theme.font_size);
    p.setFont(text_font);

    if (phase_ == OverlayPhase::stopped) {
        const int text_h = stoppedTextHeight(qRound(text_w));
        const int btn_total = (reconnect_btn_ && reconnect_btn_->isVisible())
            ? (18 + reconnect_btn_->height()) : 0;
        const qreal top_y = (h - static_cast<qreal>(text_h + btn_total)) * 0.5;

        p.setPen(stage_info_.error_tone ? danger_color : QColor(theme.text_secondary));
        p.drawText(QRectF(cx - text_w * 0.5, top_y, text_w, static_cast<qreal>(text_h)),
            Qt::AlignCenter | Qt::TextWordWrap, status_text_);
        return;
    }

    const int text_h = std::clamp(
        QFontMetrics(text_font).boundingRect(QRect(0, 0, qRound(text_w), 72),
            Qt::AlignCenter | Qt::TextWordWrap, status_text_).height(),
        22, 48);

    constexpr qreal spinner_d = 36.0;
    constexpr qreal spinner_gap = 16.0;
    constexpr qreal bar_gap = 14.0;
    constexpr qreal bar_h = 3.0;
    const qreal total_h = spinner_d + spinner_gap + static_cast<qreal>(text_h) + bar_gap + bar_h;
    const qreal top_y = (h - total_h) * 0.5;

    // 1. Minimal circular rotating spinner.
    const QPointF spinner_center(cx, top_y + spinner_d * 0.5);
    const qreal r = spinner_d * 0.5;
    const QColor track_col = theme.is_dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 22);
    p.setPen(QPen(track_col, 2.6));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(spinner_center, r, r);

    const QRectF arc_rect(spinner_center.x() - r, spinner_center.y() - r, spinner_d, spinner_d);
    p.setPen(QPen(primary_tone, 2.6, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(arc_rect, qRound((90.0 - spinner_angle_) * 16.0), -105 * 16);

    // 2. Concise status label.
    const qreal text_y = top_y + spinner_d + spinner_gap;
    p.setPen(QColor(theme.text));
    p.drawText(QRectF(cx - text_w * 0.5, text_y, text_w, static_cast<qreal>(text_h)),
        Qt::AlignCenter | Qt::TextWordWrap, status_text_);

    // 3. Slim progress indicator bar.
    const qreal bar_w = std::min<qreal>(180.0, std::max<qreal>(120.0, w - 64.0));
    const qreal bar_y = text_y + static_cast<qreal>(text_h) + bar_gap;
    const QRectF bar_rect(cx - bar_w * 0.5, bar_y, bar_w, bar_h);
    p.setPen(Qt::NoPen);
    p.setBrush(track_col);
    p.drawRoundedRect(bar_rect, 1.5, 1.5);

    if (stage_info_.stage == ConnectionVisualStage::reconnecting) {
        const qreal seg_w = bar_w * 0.32;
        const qreal seg_x = -seg_w + indeterminate_phase_ * (bar_w + seg_w);
        const QRectF fill = bar_rect.intersected(QRectF(bar_rect.left() + seg_x, bar_y, seg_w, bar_h));
        if (fill.width() > 0.5) {
            p.setBrush(primary_tone);
            p.drawRoundedRect(fill, 1.5, 1.5);
        }
    } else {
        const qreal fill_w = std::clamp(bar_w * std::clamp(display_progress_, qreal{0.0}, qreal{1.0}), qreal{6.0}, bar_w);
        p.setBrush(primary_tone);
        p.drawRoundedRect(QRectF(bar_rect.left(), bar_y, fill_w, bar_h), 1.5, 1.5);
    }
}
