#include "remote_window.h"
#include "agent_broker.h"
#include "app_settings.h"
#include "app_theme.h"
#include "connection_stage_overlay.h"
#include "frameless_chrome.h"
#include "plugin_dialog.h"
#include "settings_dialog.h"
#include "video_surface_adapter.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWindow>

#include <map>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace {

constexpr int kTabStripHeight = 40;
constexpr int kBridgeHeight = 4;
constexpr int kTabTop = 6;
constexpr qreal kTopRadius = 9.0;
constexpr qreal kFootRadius = 9.0;

QColor tabActiveColor(const ThemePalette& theme) {
    return theme.is_dark ? QColor(theme.surface_alt) : QColor(theme.surface);
}

QIcon makeControlCenterIcon(const QColor& color) {
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.35, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(color);
    // Three sleek horizontal slider bars (control center icon)
    p.drawLine(QPointF(2.5, 4.0), QPointF(13.5, 4.0));
    p.drawEllipse(QPointF(6.0, 4.0), 1.8, 1.8);
    p.drawLine(QPointF(2.5, 8.0), QPointF(13.5, 8.0));
    p.drawEllipse(QPointF(10.5, 8.0), 1.8, 1.8);
    p.drawLine(QPointF(2.5, 12.0), QPointF(13.5, 12.0));
    p.drawEllipse(QPointF(5.5, 12.0), 1.8, 1.8);
    return QIcon(pix);
}

class RemoteTabStrip final : public QFrame {
public:
    using QFrame::QFrame;

protected:
    void paintEvent(QPaintEvent*) override {
        const auto& theme = AppTheme::current();
        QPainter painter(this);
        painter.fillRect(rect(), QColor(theme.titlebar_bg));
        const int bridge_top = height() - kBridgeHeight;
        if (bridge_top > 0) {
            painter.fillRect(QRect(0, bridge_top, width(), kBridgeHeight), tabActiveColor(theme));
            painter.setPen(QPen(QColor(theme.border), 1.0));
            painter.drawLine(QPointF(0.0, bridge_top + 0.5), QPointF(static_cast<qreal>(width()), bridge_top + 0.5));
        }
    }
};

class TabCloseButton final : public QAbstractButton {
public:
    explicit TabCloseButton(QWidget* parent) : QAbstractButton(parent) {
        setAttribute(Qt::WA_Hover, true);
        setFixedSize(20, 20);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(QStringLiteral("关闭标签页"));
        setAccessibleName(toolTip());
    }

    void setTabActive(bool active) {
        if (tab_active_ == active) return;
        tab_active_ = active;
        update();
    }

protected:
    bool event(QEvent* event) override {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
            hovered_ = true;
            update();
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            hovered_ = false;
            update();
            break;
        default:
            break;
        }
        return QAbstractButton::event(event);
    }

    void paintEvent(QPaintEvent*) override {
        const auto& theme = AppTheme::current();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool hot = hovered_ || underMouse();
        if (hot || isDown()) {
            painter.setPen(Qt::NoPen);
            if (isDown()) {
                painter.setBrush(theme.is_dark ? QColor(255, 255, 255, 38) : QColor(0, 0, 0, 28));
            } else {
                painter.setBrush(theme.is_dark ? QColor(255, 255, 255, 24) : QColor(0, 0, 0, 18));
            }
            painter.drawEllipse(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5));
        }
        QColor stroke = hot ? QColor(theme.text)
            : (tab_active_ ? QColor(theme.text_secondary) : QColor(theme.text_muted));
        painter.setPen(QPen(stroke, 1.3, Qt::SolidLine, Qt::RoundCap));
        const auto center = QRectF(rect()).center();
        painter.drawLine(center + QPointF(-3.2, -3.2), center + QPointF(3.2, 3.2));
        painter.drawLine(center + QPointF(-3.2, 3.2), center + QPointF(3.2, -3.2));
    }

private:
    bool tab_active_{};
    bool hovered_{};
};

class ViewerTabBar final : public QTabBar {
public:
    explicit ViewerTabBar(QWidget* parent = nullptr) : QTabBar(parent) {
        setAttribute(Qt::WA_Hover, true);
        setMouseTracking(true);
        setFixedHeight(kTabStripHeight);
        connect(this, &QTabBar::currentChanged, this, [this](int) {
            layoutCloseButtons();
            update();
        });
        connect(this, &QTabBar::tabMoved, this, [this](int from, int to) {
            if (from >= 0 && to >= 0 && from < static_cast<int>(close_buttons_.size())
                && to < static_cast<int>(close_buttons_.size()) && from != to) {
                auto* btn = close_buttons_[static_cast<size_t>(from)];
                close_buttons_.erase(close_buttons_.begin() + from);
                close_buttons_.insert(close_buttons_.begin() + to, btn);
            }
            layoutCloseButtons();
            update();
        });
    }

protected:
    void tabInserted(int index) override {
        QTabBar::tabInserted(index);
        auto* close = new TabCloseButton(this);
        close->show();
        connect(close, &QAbstractButton::clicked, this, [this, close] {
            for (int i = 0; i < static_cast<int>(close_buttons_.size()); ++i) {
                if (close_buttons_[static_cast<size_t>(i)] == close) {
                    emit tabCloseRequested(i);
                    return;
                }
            }
        });
        const auto pos = std::clamp(index, 0, static_cast<int>(close_buttons_.size()));
        close_buttons_.insert(close_buttons_.begin() + pos, close);
        layoutCloseButtons();
    }

    void tabRemoved(int index) override {
        QTabBar::tabRemoved(index);
        if (index >= 0 && index < static_cast<int>(close_buttons_.size())) {
            auto* btn = close_buttons_[static_cast<size_t>(index)];
            close_buttons_.erase(close_buttons_.begin() + index);
            btn->hide();
            btn->deleteLater();
        }
        hovered_tab_ = -1;
        layoutCloseButtons();
    }

    void tabLayoutChange() override {
        QTabBar::tabLayoutChange();
        layoutCloseButtons();
    }

    void resizeEvent(QResizeEvent* event) override {
        QTabBar::resizeEvent(event);
        layoutCloseButtons();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        QTabBar::mouseMoveEvent(event);
        const int next = tabAt(event->pos());
        if (next != hovered_tab_) {
            hovered_tab_ = next;
            update();
        }
        layoutCloseButtons();
    }

    void leaveEvent(QEvent* event) override {
        QTabBar::leaveEvent(event);
        if (hovered_tab_ != -1) {
            hovered_tab_ = -1;
            update();
        }
    }

    QSize tabSizeHint(int index) const override {
        constexpr int chrome_padding = static_cast<int>(kFootRadius * 2) + 24 + 34;
        const int width = fontMetrics().horizontalAdvance(tabText(index)) + chrome_padding;
        return QSize(std::clamp(width, 146, 240), kTabStripHeight);
    }

    void paintEvent(QPaintEvent*) override {
        const auto& theme = AppTheme::current();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const qreal base_y = static_cast<qreal>(height() - kBridgeHeight) + 0.5;
        const int current = currentIndex();
        const QColor active_bg = tabActiveColor(theme);
        const QColor border_color(theme.border);

        auto paintTabContent = [&](int i, const QRectF& r, bool selected) {
            const qreal body_left = r.left() + kFootRadius;
            const qreal body_right = r.right() - kFootRadius;
            const qreal body_top = static_cast<qreal>(kTabTop);
            const qreal body_h = base_y - body_top;
            const qreal cy = body_top + body_h / 2.0;

            const QIcon icon = tabIcon(i);
            if (!icon.isNull()) {
                const QPixmap pm = icon.pixmap(10, 10);
                painter.drawPixmap(QPointF(body_left + 10.0, cy - 5.0), pm);
            }

            const qreal text_left = body_left + 26.0;
            const qreal text_right = body_right - 30.0;
            if (text_right > text_left) {
                QFont font = this->font();
                font.setPixelSize(std::max(12, theme.font_size - 1));
                font.setWeight(selected ? QFont::DemiBold : QFont::Normal);
                painter.setFont(font);
                painter.setPen(QColor(selected || i == hovered_tab_ ? theme.text : theme.text_secondary));
                const QString elided = QFontMetrics(font).elidedText(
                    tabText(i), Qt::ElideRight, static_cast<int>(text_right - text_left));
                painter.drawText(QRectF(text_left, body_top, text_right - text_left, body_h),
                    Qt::AlignVCenter | Qt::AlignLeft, elided);
            }
        };

        // 1. Paint inactive tabs first (with hover pill and Chrome-style dividers)
        for (int i = 0; i < count(); ++i) {
            if (i == current) continue;
            const QRectF r = tabRect(i);
            if (!r.isValid()) continue;

            if (i == hovered_tab_) {
                const QRectF pill(r.left() + kFootRadius - 2.0, kTabTop + 1.0,
                    r.width() - (kFootRadius - 2.0) * 2.0, base_y - kTabTop - 4.0);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(theme.surface_hover));
                painter.drawRoundedRect(pill, 7.5, 7.5);
            }

            // Vertical divider on right edge when neither this nor next tab is selected or hovered
            const bool next_active = (i + 1 == current) || (i + 1 == hovered_tab_);
            if (i != hovered_tab_ && !next_active && i + 1 < count()) {
                painter.setPen(QPen(border_color, 1.0));
                const qreal dx = std::floor(r.right()) - 0.5;
                painter.drawLine(QPointF(dx, kTabTop + 7.0), QPointF(dx, base_y - 7.0));
            }

            paintTabContent(i, r, false);
        }

        // 2. Paint active tab last so its concave bottom curves sit cleanly above neighbors & bridge
        if (current >= 0 && current < count()) {
            const QRectF r = tabRect(current);
            if (r.isValid()) {
                const qreal left = r.left() + 0.5;
                const qreal right = r.right() - 0.5;
                const qreal top = static_cast<qreal>(kTabTop) + 0.5;
                const qreal tr = kTopRadius;
                const qreal fr = kFootRadius;

                // Chrome-style tab silhouette with outward concave quarter-circle bottom flares
                QPainterPath outline;
                outline.moveTo(left, base_y);
                // Left concave flare: tangent horizontally at (left, base_y), vertically at (left + fr, base_y - fr)
                outline.arcTo(QRectF(left - fr, base_y - 2.0 * fr, 2.0 * fr, 2.0 * fr), 270.0, 90.0);
                outline.lineTo(left + fr, top + tr);
                // Top-left convex corner
                outline.arcTo(QRectF(left + fr, top, 2.0 * tr, 2.0 * tr), 180.0, -90.0);
                outline.lineTo(right - fr - tr, top);
                // Top-right convex corner
                outline.arcTo(QRectF(right - fr - 2.0 * tr, top, 2.0 * tr, 2.0 * tr), 90.0, -90.0);
                outline.lineTo(right - fr, base_y - fr);
                // Right concave flare: tangent vertically at (right - fr, base_y - fr), horizontally at (right, base_y)
                outline.arcTo(QRectF(right - fr, base_y - 2.0 * fr, 2.0 * fr, 2.0 * fr), 180.0, 90.0);

                QPainterPath fill = outline;
                fill.lineTo(right, static_cast<qreal>(height()));
                fill.lineTo(left, static_cast<qreal>(height()));
                fill.closeSubpath();

                painter.setPen(Qt::NoPen);
                painter.setBrush(active_bg);
                painter.drawPath(fill);

                // Crisp rim along the curve that merges seamlessly into the bridge's top hairline
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(border_color, 1.0));
                painter.drawPath(outline);

                paintTabContent(current, r, true);
            }
        }
    }

private:
    void layoutCloseButtons() {
        const int current = currentIndex();
        const int body_h = height() - kBridgeHeight - kTabTop;
        for (int i = 0; i < static_cast<int>(close_buttons_.size()); ++i) {
            auto* btn = close_buttons_[static_cast<size_t>(i)];
            const QRect r = tabRect(i);
            if (!r.isValid() || r.width() < 60) {
                btn->hide();
                continue;
            }
            const int x = r.right() - static_cast<int>(kFootRadius) - 6 - btn->width();
            const int y = kTabTop + (body_h - btn->height()) / 2;
            btn->move(x, y);
            btn->setTabActive(i == current);
            btn->show();
            btn->raise();
        }
    }

    int hovered_tab_{-1};
    std::vector<TabCloseButton*> close_buttons_;
};

QIcon statusDotIcon(const QColor& color) {
    QPixmap pix(10, 10);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(1, 1, 8, 8);
    return QIcon(pix);
}

void addMenuSection(QMenu& menu, const QString& name) {
    if (!menu.isEmpty()) menu.addSeparator();
    menu.addAction(name)->setEnabled(false);
}

void setMenuOpen(QPushButton& button, bool open) {
    button.setProperty("menuOpen", open);
    button.style()->unpolish(&button);
    button.style()->polish(&button);
    button.update();
}

} // namespace

class RemoteSessionTab final : public QWidget {
public:
    RemoteSessionTab(p2p_account* account, const QString& device, const QString& name,
        const LaunchOptions& options, bool diagnostics, std::function<void()> on_fullscreen,
        std::function<void()> on_share,
        std::function<void()> on_changed, QWidget* dialog_parent, QWidget* parent,
        const std::optional<LanConnectOptions>& lan = std::nullopt)
        : QWidget(parent), device_(device), name_(name), diagnostics_(diagnostics), on_changed_(std::move(on_changed)) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        if (diagnostics_) {
            layout->addStretch();
            share_ = new QPushButton(QStringLiteral("共享诊断给本机 Agent…"), this);
            share_->setEnabled(false);
            connect(share_, &QPushButton::clicked, this, std::move(on_share));
            layout->addWidget(share_, 0, Qt::AlignHCenter);
            layout->addStretch();
        } else {
            surface_ = createVideoSurfaceAdapter(std::move(on_fullscreen), this);
            video_ = surface_->widget();
            video_->installEventFilter(this);
            layout->addWidget(video_, 1);
        }

        const auto render_target = surface_ ? surface_->target() : p2p_render_target_v2{};
        if (lan) {
            driver_ = std::make_unique<StreamDriver>(*lan, render_target, options, this);
        } else {
            driver_ = std::make_unique<StreamDriver>(account, false, device, render_target, options, this, diagnostics_);
        }
        if (video_) {
            stage_overlay_ = new ConnectionStageOverlay(this);
            stage_overlay_->setGeometry(video_->geometry());
            stage_overlay_->raise();
            connect(stage_overlay_, &ConnectionStageOverlay::reconnectRequested, this, [this] {
                if (!active() && handle()) start();
            });
            surface_->setFirstFrameCallback([this] {
                first_frame_received_ = true;
                updateStageOverlay();
            });
        }
        if (!driver_->handle()) {
            status_text_ = QString::fromWCharArray(p2p_last_error());
            raw_status_text_ = status_text_;
            state_ = P2P_STOPPED;
            updateStageOverlay();
            return;
        }
        const auto& defaults = AppSettings::instance().data();
        quality_index_ = defaults.default_quality;
        bitrate_index_ = defaults.default_bitrate;
        speaker_enabled_ = defaults.speaker_enabled;
        speaker_volume_ = static_cast<float>(defaults.speaker_volume) / 100.0f;
        mic_fallback_ = defaults.mic_fallback;
        connect(&overlay_timer_, &QTimer::timeout, this, [this] {
            updateConnectionOverlays(false);
        });
        connect(&AppSettings::instance(), &AppSettings::changed, this, [this](const AppSettingsData&) {
            updateConnectionOverlays();
            if (stage_overlay_) stage_overlay_->refreshTheme();
        });
        connect(&AppSettings::instance(), &AppSettings::pluginConfigChanged, this, [this](const QString& id) {
            if (overlay_configs_.erase(id.toStdWString())) updateConnectionOverlays();
        });

        dialogs_ = new PluginDialogManager(dialog_parent, this);
        connect(driver_.get(), &StreamDriver::changed, this, [this](const p2p_stream_snapshot_v2& snap) {
            onSnapshot(snap);
        });
        connect(driver_.get(), &StreamDriver::uiCommandsReady, this, [this] {
            if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
        });
        updateStageOverlay();
    }

    ~RemoteSessionTab() override {
        if (surface_) surface_->setFirstFrameCallback({});
        if (dialogs_) dialogs_->closeAll();
    }

    const QString& device() const { return device_; }
    const QString& name() const { return name_; }
    bool diagnostics() const { return diagnostics_; }
    p2p_stream* handle() const { return driver_ ? driver_->handle() : nullptr; }
    StreamDriver* driver() const { return driver_.get(); }
    int state() const { return state_; }
    bool active() const { return state_ != P2P_IDLE && state_ != P2P_STOPPED; }
    bool playing() const { return state_ == P2P_STREAMING; }
    bool waitingForVideo() const { return playing() && !diagnostics_ && !width_; }
    bool hasPluginMenu() const { return has_plugin_menu_; }
    bool canExport() const { return !diagnostics_ && !active() && seconds_ > 0; }
    int qualityIndex() const { return quality_index_; }
    int bitrateIndex() const { return bitrate_index_; }
    std::uint32_t confirmedBitrate() const { return confirmed_bitrate_; }
    bool speakerEnabled() const { return speaker_enabled_; }
    bool micEnabled() const { return mic_enabled_; }
    float speakerVolume() const { return speaker_volume_; }
    bool micFallback() const { return mic_fallback_; }
    const QString& statusText() const { return status_text_; }
    const QString& noticeText() const { return notice_text_; }

    bool refreshOverlayItems() {
        using namespace p2premote;
        if (!handle()) return false;
        const auto count = p2p_stream_read_overlays(handle());
        const auto revision = p2p_stream_overlay_revision(handle());
        if (revision == overlay_revision_) return false;
        overlay_revision_ = revision;
        overlay_dirty_ = true;
        overlay_items_.clear();
        for (std::size_t i = 0; i < count; ++i) {
            p2p_overlay_item_v1 item{};
            if (p2p_stream_overlay_item(handle(), i, &item) == P2P_OK) {
                overlay_items_.push_back({item.id, item.plugin_id, item.title, item.text,
                    static_cast<OverlayAnchor>(item.anchor), item.priority, item.font_size, item.bg_rgb, item.text_rgb,
                    item.opacity, item.monospace != 0, item.visible != 0, item.default_visible != 0});
            }
        }
        return true;
    }

    const auto& overlayItems() const { return overlay_items_; }

    QString tabTitle() const {
        if (diagnostics_) return QStringLiteral("%1 · 诊断").arg(name_);
        if (playing() && width_ && height_) {
            return QStringLiteral("%1 · 屏幕1").arg(name_);
        }
        return name_;
    }

    QIcon tabIcon() const {
        static const auto connected = statusDotIcon(QColor(0x22, 0xc5, 0x5e));
        static const auto connecting = statusDotIcon(QColor(0xf5, 0x9e, 0x0b));
        static const auto stopped = statusDotIcon(QColor(0x64, 0x74, 0x8b));
        if (playing()) return connected;
        return active() ? connecting : stopped;
    }

    void start() {
        if (driver_ && driver_->handle()) {
            if (!playing()) {
                width_ = 0;
                height_ = 0;
                first_frame_received_ = false;
            }
            syncVideo();
            driver_->update(p2p_stream_start(driver_->handle(), nullptr, 60));
            if (diagnostics_) return;
            setQuality(quality_index_);
            setBitrate(bitrate_index_);
            pushAudioOptions();
        }
    }

    void stop() {
        if (driver_ && driver_->handle()) {
            driver_->update(p2p_stream_stop(driver_->handle(), nullptr));
        }
    }

    void setQuality(int index) {
        if (!driver_ || !driver_->handle()) return;
        quality_index_ = std::clamp(index, 0, 2);
        driver_->update(p2p_stream_quality(driver_->handle(), P2P_720P + quality_index_));
    }

    void setBitrate(int index) {
        if (!driver_ || !driver_->handle()) return;
        bitrate_index_ = std::clamp(index, 0, 2);
        driver_->update(p2p_stream_bitrate(driver_->handle(), P2P_ECONOMY + bitrate_index_));
    }

    void setSpeakerEnabled(bool enabled) {
        speaker_enabled_ = enabled;
        pushAudioOptions();
    }

    void setMicEnabled(bool enabled) {
        mic_enabled_ = enabled;
        pushAudioOptions();
    }

    void setSpeakerVolume(float volume) {
        speaker_volume_ = std::clamp(volume, 0.0f, 1.0f);
        pushAudioOptions();
    }

    void setMicFallback(bool enabled) {
        mic_fallback_ = enabled;
        pushAudioOptions();
    }

    void setTabFocused(bool focused) {
        if (tab_focused_ == focused) return;
        tab_focused_ = focused;
        pushAudioOptions();
    }

    void pushAudioOptions() {
        if (diagnostics_ || !driver_ || !driver_->handle()) return;
        const p2p_audio_options_v1 opt{
            (speaker_enabled_ && tab_focused_) ? 1 : 0,
            (mic_enabled_ && tab_focused_) ? 1 : 0,
            speaker_volume_,
            mic_fallback_ ? 1 : 0
        };
        p2p_stream_set_audio(driver_->handle(), &opt);
    }

    void syncVideo() {
        if (!video_) return;
        if (driver_) driver_->resize(video_->isVisible() && !window()->isMinimized());
        updateConnectionOverlays();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        syncStageOverlayGeometry();
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == video_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::Hide || event->type() == QEvent::DevicePixelRatioChange)) {
            syncStageOverlayGeometry();
            syncVideo();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void syncStageOverlayGeometry() {
        if (!stage_overlay_ || !video_) return;
        const QRect bounds = video_->geometry();
        if (stage_overlay_->geometry() != bounds) {
            stage_overlay_->setGeometry(bounds);
        }
    }

    void updateStageOverlay() {
        if (!stage_overlay_) return;
        syncStageOverlayGeometry();
        const bool has_video = (playing() && width_ > 0 && height_ > 0) || (active() && first_frame_received_);
        stage_overlay_->setSessionState(state_, has_video, raw_status_text_, handle() != nullptr);
    }

    void updateConnectionOverlays(bool force = true) {
        if (!video_ || !AppSettings::instance().data().show_diagnostics || !playing() || !video_->isVisible() ||
            window()->isMinimized()) {
            overlay_timer_.stop();
            if (surface_) surface_->hideOverlays();
            return;
        }
        if (!overlay_timer_.isActive()) overlay_timer_.start(100);
        refreshOverlayItems();
        if (!overlay_dirty_ && !force) return;
        auto items = overlay_items_;
        for (auto& item : items) {
            const auto [it, added] = overlay_configs_.try_emplace(item.plugin_id);
            if (added) {
                const auto id = QString::fromStdWString(item.plugin_id);
                it->second = {AppSettings::loadPluginConfig(id), AppSettings::overlayVisibility(id)};
            }
            const auto& [settings, enabled] = it->second;
            item.visible = item.visible && enabled.value_or(item.default_visible);
            if (const auto anchor = p2premote::ParseOverlayAnchor(settings.value(QStringLiteral("anchor")).toString().toStdString())) {
                item.anchor = *anchor;
            }
            item.font_size = std::clamp(settings.value(QStringLiteral("font_size")).toInt(item.font_size), 9, 20);
            item.opacity = static_cast<std::uint8_t>(std::clamp(settings.value(QStringLiteral("opacity")).toInt(item.opacity), 0, 255));
        }
        surface_->renderOverlays(width_, height_, std::move(items));
        overlay_dirty_ = false;
    }

    void onSnapshot(const p2p_stream_snapshot_v2& snapshot) {
        state_ = snapshot.state;
        if (!active() || state_ == P2P_RECONNECTING) first_frame_received_ = false;
        if (share_) share_->setEnabled(playing());
        seconds_ = snapshot.seconds;
        width_ = snapshot.width;
        height_ = snapshot.height;
        confirmed_bitrate_ = snapshot.bitrate_limit_bps;
        has_plugin_menu_ = snapshot.has_plugin_menu != 0;

        if (dialogs_) {
            if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
            if (!active()) dialogs_->closeAll();
        }

        raw_status_text_ = snapshot.status ? QString::fromUtf8(snapshot.status) : QString{};
        status_text_ = raw_status_text_;
        if (snapshot.input_ready && !waitingForVideo()) status_text_ += QStringLiteral("  ·  点击画面接管键鼠，F12 释放");
        notice_text_ = *snapshot.plugin_notice ? QString::fromUtf8(snapshot.plugin_notice) : QString{};

        updateStageOverlay();
        updateConnectionOverlays();
        if (on_changed_) on_changed_();
    }

    QString device_;
    QString name_;
    bool diagnostics_{};
    std::function<void()> on_changed_;
    QPushButton* share_{};
    std::unique_ptr<VideoSurfaceAdapter> surface_;
    QWidget* video_{};
    ConnectionStageOverlay* stage_overlay_{};
    bool first_frame_received_{};
    std::unique_ptr<StreamDriver> driver_;
    PluginDialogManager* dialogs_{};
    int state_{P2P_IDLE};
    double seconds_{};
    uint32_t width_{}, height_{};
    bool has_plugin_menu_{};
    int quality_index_{1};
    int bitrate_index_{1};
    std::uint32_t confirmed_bitrate_{};
    bool speaker_enabled_{true};
    bool mic_enabled_{false};
    float speaker_volume_{1.0f};
    bool mic_fallback_{true};
    bool tab_focused_{true};
    QString raw_status_text_{QStringLiteral("正在连接设备…")};
    QString status_text_{QStringLiteral("正在连接设备…")};
    QString notice_text_;
    QTimer overlay_timer_;
    std::uint64_t overlay_revision_{};
    bool overlay_dirty_{};
    std::vector<p2premote::PluginOverlayItem> overlay_items_;
    std::map<std::wstring, std::pair<QJsonObject, std::optional<bool>>> overlay_configs_;
};

RemoteWindow::RemoteWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("remoteWindow"));
    setWindowTitle(QStringLiteral("P2P Remote"));
    setMinimumSize(820, 480);
    resize(1160, 720);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    shell_ = new QFrame(this);
    shell_->setObjectName(QStringLiteral("framelessShell"));
    layout_ = new QVBoxLayout(shell_);
    layout_->setContentsMargins(1, 1, 1, 1);
    layout_->setSpacing(0);

    topbar_ = new QFrame(shell_);
    topbar_->setObjectName(QStringLiteral("remoteTopbar"));
    auto* chrome = new QVBoxLayout(topbar_);
    chrome->setContentsMargins(0, 0, 0, 0);
    chrome->setSpacing(0);

    tab_strip_ = new RemoteTabStrip(topbar_);
    tab_strip_->setObjectName(QStringLiteral("remoteTabStrip"));
    tab_strip_->setFixedHeight(kTabStripHeight);
    tab_strip_->installEventFilter(this);
    auto* tab_row = new QHBoxLayout(tab_strip_);
    tab_row->setContentsMargins(6, 0, 0, 0);
    tab_row->setSpacing(0);

    tabs_ = new ViewerTabBar(tab_strip_);
    tabs_->setObjectName(QStringLiteral("remoteTabs"));
    tabs_->setAccessibleName(QStringLiteral("远程会话"));
    tabs_->setMovable(true);
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    tabs_->setUsesScrollButtons(true);
    tabs_->setIconSize(QSize(10, 10));
    tabs_->setElideMode(Qt::ElideRight);
    tab_row->addWidget(tabs_, 0, Qt::AlignBottom);

    auto* right_controls = new QHBoxLayout;
    right_controls->setContentsMargins(4, 0, 0, kBridgeHeight);
    right_controls->setSpacing(0);

    add_tab_ = new QPushButton(QStringLiteral("+"), tab_strip_);
    add_tab_->setObjectName(QStringLiteral("barAddBtn"));
    add_tab_->setFocusPolicy(Qt::NoFocus);
    add_tab_->setToolTip(QStringLiteral("连接其他设备"));
    add_tab_->setAccessibleName(QStringLiteral("连接其他设备"));
    right_controls->addWidget(add_tab_, 0, Qt::AlignVCenter);
    right_controls->addStretch(1);

    control_center_ = new QPushButton(QStringLiteral("控制中心 ▾"), tab_strip_);
    control_center_->setObjectName(QStringLiteral("controlCenterBtn"));
    control_center_->setFocusPolicy(Qt::NoFocus);
    control_center_->setIcon(makeControlCenterIcon(QColor(AppTheme::current().text)));
    control_center_->setIconSize(QSize(14, 14));
    control_center_->setToolTip(QStringLiteral("画面、声音、输入、工具与会话控制"));
    right_controls->addWidget(control_center_, 0, Qt::AlignVCenter);
    right_controls->addSpacing(8);

    exit_fullscreen_ = new QPushButton(QStringLiteral("退出全屏"), tab_strip_);
    exit_fullscreen_->setObjectName(QStringLiteral("controlCenterBtn"));
    exit_fullscreen_->setFocusPolicy(Qt::NoFocus);
    exit_fullscreen_->setToolTip(QStringLiteral("F11"));
    exit_fullscreen_->hide();
    right_controls->addWidget(exit_fullscreen_, 0, Qt::AlignVCenter);
    right_controls->addSpacing(6);

    caption_divider_ = new QFrame(tab_strip_);
    caption_divider_->setObjectName(QStringLiteral("titleSeparator"));
    right_controls->addWidget(caption_divider_, 0, Qt::AlignVCenter);
    right_controls->addSpacing(6);

    win_min_ = new WindowCaptionButton(CaptionButtonKind::Minimize, tab_strip_);
    right_controls->addWidget(win_min_);

    win_max_ = new WindowCaptionButton(CaptionButtonKind::Maximize, tab_strip_);
    right_controls->addWidget(win_max_);

    win_close_ = new WindowCaptionButton(CaptionButtonKind::Close, tab_strip_);
    right_controls->addWidget(win_close_);

    tab_row->addLayout(right_controls, 1);
    chrome->addWidget(tab_strip_);

    layout_->addWidget(topbar_);

    stack_ = new QStackedWidget(shell_);
    layout_->addWidget(stack_, 1);

    statusbar_ = new QFrame(shell_);
    statusbar_->setObjectName(QStringLiteral("remoteStatusbar"));
    auto* bottom = new QHBoxLayout(statusbar_);
    bottom->setContentsMargins(12, 4, 12, 4);
    bottom->setSpacing(12);
    status_text_ = new QLabel(statusbar_);
    status_text_->setObjectName(QStringLiteral("remoteStatusText"));
    status_text_->setTextFormat(Qt::PlainText);
    status_text_->setWordWrap(true);
    bottom->addWidget(status_text_, 1);
    layout_->addWidget(statusbar_);

    outer->addWidget(shell_);

    connect(&AppSettings::instance(), &AppSettings::changed, this, [this](const AppSettingsData&) {
        if (control_center_) control_center_->setIcon(makeControlCenterIcon(QColor(AppTheme::current().text)));
        if (tab_strip_) tab_strip_->update();
        if (tabs_) tabs_->update();
    });
    connect(tabs_, &QTabBar::currentChanged, this, &RemoteWindow::onTabChanged);
    connect(tabs_, &QTabBar::tabCloseRequested, this, &RemoteWindow::closeTab);
    connect(tabs_, &QTabBar::tabMoved, this, &RemoteWindow::onTabMoved);
    connect(add_tab_, &QPushButton::clicked, this, [this] {
        if (auto* owner = parentWidget()) {
            if (owner->isMinimized()) owner->showNormal();
            else owner->show();
            owner->raise();
            owner->activateWindow();
        }
    });
    connect(control_center_, &QPushButton::clicked, this, &RemoteWindow::showControlCenterMenu);
    connect(exit_fullscreen_, &QPushButton::clicked, this, &RemoteWindow::toggleFullscreen);
    connect(win_min_, &QAbstractButton::clicked, this, [this] { minimizeFramelessWindow(this); });
    connect(win_max_, &QAbstractButton::clicked, this, [this] {
        toggleFramelessMaximize(this);
    });
    connect(win_close_, &QAbstractButton::clicked, this, &QWidget::close);

    auto* shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, &RemoteWindow::toggleFullscreen);
    hover_.setInterval(60);
    connect(&hover_, &QTimer::timeout, this, &RemoteWindow::positionFullscreenToolbar);
}

RemoteWindow::~RemoteWindow() = default;

void RemoteWindow::openSession(p2p_account* account, const QString& device, const QString& name,
    const LaunchOptions& options, bool diagnostics) {
    account_ = account;
    if (const int existing = findSession(device, diagnostics); existing >= 0) {
        tabs_->setCurrentIndex(existing);
        if (auto* session = sessionAt(existing); session && !session->active()) {
            session->start();
        }
        show();
        raise();
        activateWindow();
        return;
    }

    auto* session = new RemoteSessionTab(
        account, device, name, options, diagnostics,
        [this] { toggleFullscreen(); },
        [this] { shareAgentDiagnostics(); },
        [this] { refreshChrome(); },
        this, stack_);
    sessions_.push_back(session);
    stack_->addWidget(session);
    const int idx = tabs_->addTab(session->tabIcon(), session->tabTitle());
    tabs_->setTabToolTip(idx, name);
    tabs_->setCurrentIndex(idx);
    refreshChrome();
    show();
    raise();
    activateWindow();
    session->start();
}

void RemoteWindow::openLanSession(const LanConnectOptions& lan, const QString& name, const LaunchOptions& options) {
    const auto key = QStringLiteral("lan:%1:%2").arg(QString::fromStdWString(lan.host)).arg(lan.port);
    if (const int existing = findSession(key, false); existing >= 0) {
        tabs_->setCurrentIndex(existing);
        if (auto* session = sessionAt(existing); session && !session->active()) {
            session->start();
        }
        show();
        raise();
        activateWindow();
        return;
    }

    const auto title = name.isEmpty()
        ? QStringLiteral("%1:%2").arg(QString::fromStdWString(lan.host)).arg(lan.port)
        : name;
    auto* session = new RemoteSessionTab(
        nullptr, key, title, options, false,
        [this] { toggleFullscreen(); },
        [this] { shareAgentDiagnostics(); },
        [this] { refreshChrome(); },
        this, stack_, lan);
    sessions_.push_back(session);
    stack_->addWidget(session);
    const int idx = tabs_->addTab(session->tabIcon(), session->tabTitle());
    tabs_->setTabToolTip(idx, QStringLiteral("%1 (%2:%3)").arg(title, QString::fromStdWString(lan.host)).arg(lan.port));
    tabs_->setCurrentIndex(idx);
    refreshChrome();
    show();
    raise();
    activateWindow();
    session->start();
}

void RemoteWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyWin32FramelessChrome(this);
    syncWindowControls();
}

void RemoteWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        syncWindowControls();
        if (auto* session = currentSession()) session->syncVideo();
    }
}

bool RemoteWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tab_strip_ && !fullscreen_mode_) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && !tab_strip_->childAt(me->position().toPoint()) && windowHandle()) {
                if (windowHandle()->startSystemMove()) return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && !tab_strip_->childAt(me->position().toPoint())) {
                toggleFramelessMaximize(this);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool RemoteWindow::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
    if (handleWin32FramelessChrome(this, message, result)) return true;
    return QWidget::nativeEvent(type, message, result);
}

void RemoteWindow::syncWindowControls() {
    const bool show_controls = !fullscreen_mode_;
    if (caption_divider_) caption_divider_->setVisible(show_controls);
    if (exit_fullscreen_) exit_fullscreen_->setVisible(fullscreen_mode_);
    if (win_min_) win_min_->setVisible(show_controls);
    if (win_max_) {
        win_max_->setVisible(show_controls);
        win_max_->setMaximized(isMaximized());
    }
    if (win_close_) win_close_->setVisible(show_controls);
}

void RemoteWindow::openSettings() {
    if (!settings_dialog_) {
        settings_dialog_ = new SettingsDialog(this);
        settings_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    settings_dialog_->show();
    settings_dialog_->raise();
    settings_dialog_->activateWindow();
}

RemoteSessionTab* RemoteWindow::currentSession() const {
    return sessionAt(tabs_->currentIndex());
}

RemoteSessionTab* RemoteWindow::sessionAt(int index) const {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return nullptr;
    return sessions_[static_cast<size_t>(index)];
}

int RemoteWindow::findSession(const QString& device, bool diagnostics) const {
    for (int i = 0; i < static_cast<int>(sessions_.size()); ++i) {
        const auto* session = sessions_[static_cast<size_t>(i)];
        if (session->device() == device && session->diagnostics() == diagnostics) return i;
    }
    return -1;
}

void RemoteWindow::onTabChanged(int index) {
    for (int i = 0; i < static_cast<int>(sessions_.size()); ++i) {
        sessions_[static_cast<size_t>(i)]->setTabFocused(i == index);
    }
    if (auto* session = sessionAt(index)) {
        stack_->setCurrentWidget(session);
        session->syncVideo();
    }
    refreshChrome();
}

void RemoteWindow::onTabMoved(int from, int to) {
    if (from < 0 || to < 0 || from >= static_cast<int>(sessions_.size())
        || to >= static_cast<int>(sessions_.size()) || from == to) {
        return;
    }
    auto* moved = sessions_[static_cast<size_t>(from)];
    sessions_.erase(sessions_.begin() + from);
    sessions_.insert(sessions_.begin() + to, moved);
    onTabChanged(tabs_->currentIndex());
}

void RemoteWindow::closeTab(int index) {
    auto* session = sessionAt(index);
    if (!session) return;
    if (agent_target_ == session) stopAgentSharing();
    sessions_.erase(sessions_.begin() + index);
    tabs_->removeTab(index);
    stack_->removeWidget(session);
    delete session;
    if (sessions_.empty()) {
        close();
        return;
    }
    if (auto* next = currentSession()) {
        stack_->setCurrentWidget(next);
        next->syncVideo();
    }
    refreshChrome();
}

void RemoteWindow::refreshChrome() {
    for (int i = 0; i < static_cast<int>(sessions_.size()); ++i) {
        const auto* session = sessions_[static_cast<size_t>(i)];
        const auto title = session->tabTitle();
        if (tabs_->tabText(i) != title) tabs_->setTabText(i, title);
        const auto icon = session->tabIcon();
        if (tabs_->tabIcon(i).cacheKey() != icon.cacheKey()) tabs_->setTabIcon(i, icon);
    }

    auto* current = currentSession();
    plugin_menu_.read(current && current->hasPluginMenu() ? current->handle() : nullptr);
    if (!current) {
        setWindowTitle(QStringLiteral("P2P Remote"));
        control_center_->setEnabled(false);
        statusbar_->hide();
        return;
    }

    setWindowTitle(QStringLiteral("%1 · P2P Remote").arg(current->name()));
    const bool has_handle = current->handle() != nullptr;
    control_center_->setEnabled(true);

    const bool mic_live = has_handle && current->playing() && current->micEnabled();
    const auto center_label = mic_live ? QStringLiteral("控制中心 · 麦开 ▾") : QStringLiteral("控制中心 ▾");
    if (control_center_->text() != center_label) control_center_->setText(center_label);
    if (control_center_->property("micLive").toBool() != mic_live) {
        control_center_->setProperty("micLive", mic_live);
        control_center_->style()->unpolish(control_center_);
        control_center_->style()->polish(control_center_);
    }

    QString left = current->statusText();
    if (!current->noticeText().isEmpty()) {
        left += QStringLiteral("  ·  ") + current->noticeText();
    }
    const bool agent_shared = agent_broker_ && agent_broker_->active();
    if (!agent_shared && agent_dialog_) agent_dialog_->close();
    if (agent_shared) left += QStringLiteral("  ·  正在共享 Agent 只读诊断");
    if (status_text_->text() != left) status_text_->setText(left);

    const bool need_statusbar = !current->noticeText().isEmpty() || current->waitingForVideo()
        || (!fullscreen_mode_ && (!current->playing() || current->diagnostics() || agent_shared));
    statusbar_->setVisible(need_statusbar);
}

PluginMenuLayout::Invoke RemoteWindow::pluginMenuInvoker(RemoteSessionTab* session) {
    return [this, target = QPointer<RemoteSessionTab>(session)](uint32_t id) {
        if (target && currentSession() == target && target->handle()) {
            target->driver()->update(p2p_stream_invoke_menu(target->handle(), id));
        }
    };
}

void RemoteWindow::toggleFullscreen() {
    fullscreen_mode_ = !fullscreen_mode_;
    if (fullscreen_mode_) {
        was_maximized_ = isMaximized();
        layout_->removeWidget(topbar_);
        topbar_->setParent(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        topbar_->setAttribute(Qt::WA_ShowWithoutActivating);
        syncWindowControls();
        statusbar_->hide();
        showFullScreen();
        hover_.start();
        positionFullscreenToolbar();
    } else {
        hover_.stop();
        topbar_->hide();
        topbar_->setParent(shell_, Qt::Widget);
        layout_->insertWidget(0, topbar_);
        syncWindowControls();
        topbar_->show();
        if (was_maximized_) showMaximized();
        else showNormal();
        refreshChrome();
    }
    if (auto* session = currentSession()) session->syncVideo();
}

void RemoteWindow::positionFullscreenToolbar() {
    bool associated_popup{};
    for (auto* widget = QApplication::activePopupWidget(); widget; widget = widget->parentWidget()) {
        if (widget == topbar_) {
            associated_popup = true;
            break;
        }
    }
    if (!isActiveWindow() && !topbar_->isActiveWindow() && !associated_popup) {
        topbar_->hide();
        return;
    }
    const auto top = mapToGlobal(QPoint(0, 0));
    const auto point = QCursor::pos();
    const auto inside_top = QRect(top, QSize(width(), 8)).contains(point);
    const auto inside_bar = topbar_->isVisible() && topbar_->frameGeometry().contains(point);
    const auto* session = currentSession();
    const bool playing = session && session->playing();
    if (!playing || inside_top || inside_bar || associated_popup) {
        const auto bar_width = std::min(width(), 1080);
        topbar_->setGeometry(top.x() + (width() - bar_width) / 2, top.y(),
            bar_width, topbar_->sizeHint().height());
        topbar_->show();
    } else {
        topbar_->hide();
    }
}

void RemoteWindow::popupToolbarMenu(QPushButton* button, QMenu* menu, bool align_right) {
    const auto name = QStringLiteral("remoteMenu");
    menu->setObjectName(name);
    for (auto* submenu : menu->findChildren<QMenu*>()) submenu->setObjectName(name);
    setMenuOpen(*button, true);
    connect(menu, &QMenu::aboutToHide, menu, [menu, anchor = QPointer<QPushButton>(button)] {
        if (anchor) setMenuOpen(*anchor, false);
        menu->deleteLater();
    });
    const int x = align_right ? (button->width() - menu->sizeHint().width()) : 0;
    menu->popup(button->mapToGlobal(QPoint(x, button->height() + 4)));
}

void RemoteWindow::showControlCenterMenu() {
    auto* session = currentSession();
    if (!session) return;
    if (session->handle()) plugin_menu_.read(session->handle());
    else plugin_menu_.read(nullptr);

    auto* menu = new QMenu(control_center_);
    const auto invoke = pluginMenuInvoker(session);
    const bool has_handle = session->handle() != nullptr;
    const bool can_stream_ui = has_handle && !session->diagnostics();

    // Quick fullscreen toggle at top
    auto* fs = menu->addAction(fullscreen_mode_ ? QStringLiteral("退出全屏 (F11)") : QStringLiteral("全屏显示 (F11)"));
    fs->setEnabled(fullscreen_mode_ || !session->diagnostics());
    connect(fs, &QAction::triggered, this, &RemoteWindow::toggleFullscreen);

    menu->addSeparator();

    // 1. 画面设置
    auto* display_sub = menu->addMenu(QStringLiteral("画面设置"));
    display_sub->setEnabled(can_stream_ui);
    if (can_stream_ui) populateDisplayMenu(*display_sub, session);

    // 2. 声音设置
    const bool mic_live = has_handle && session->playing() && session->micEnabled();
    const auto audio_title = mic_live ? QStringLiteral("声音设置（麦开）")
        : (!session->speakerEnabled() ? QStringLiteral("声音设置（静音）") : QStringLiteral("声音设置"));
    auto* audio_sub = menu->addMenu(audio_title);
    audio_sub->setEnabled(can_stream_ui);
    if (can_stream_ui) populateAudioMenu(*audio_sub, session);

    // 3. 输入与快捷键 (plugins in "input" slot)
    if (plugin_menu_.hasItems(QStringLiteral("input"))) {
        auto* input_sub = menu->addMenu(QStringLiteral("输入与快捷键"));
        plugin_menu_.append(*input_sub, QStringLiteral("input"), invoke);
    }

    // 4. 远程工具 (plugins in "tools" slot)
    if (plugin_menu_.hasItems(QStringLiteral("tools"))) {
        auto* tools_sub = menu->addMenu(QStringLiteral("远程工具"));
        plugin_menu_.append(*tools_sub, QStringLiteral("tools"), invoke);
    }

    // 5. Custom plugin top-level menus or direct plugin commands
    const auto extra_buttons = plugin_menu_.toolbar();
    if (!extra_buttons.empty()) {
        menu->addSeparator();
        for (const auto& spec : extra_buttons) {
            if (spec.command) {
                auto* act = menu->addAction(spec.label);
                if (spec.checked >= 0) {
                    act->setCheckable(true);
                    act->setChecked(spec.checked > 0);
                }
                connect(act, &QAction::triggered, this, [invoke, id = spec.command] { invoke(id); });
            } else {
                auto* sub = menu->addMenu(spec.label);
                plugin_menu_.append(*sub, spec.menu, invoke);
            }
        }
    }

    // 6. 会话管理 & 本机设置
    menu->addSeparator();
    auto* session_sub = menu->addMenu(QStringLiteral("会话管理"));
    populateActionsMenu(*session_sub, session);

    auto* open_settings = menu->addAction(QStringLiteral("软件设置…"));
    connect(open_settings, &QAction::triggered, this, &RemoteWindow::openSettings);

    auto* close_tab = menu->addAction(QStringLiteral("关闭当前标签页"));
    connect(close_tab, &QAction::triggered, this, [this] {
        closeTab(tabs_->currentIndex());
    });

    popupToolbarMenu(control_center_, menu, true);
}

void RemoteWindow::populateDisplayMenu(QMenu& menu, RemoteSessionTab* session) {
    const auto invoke = pluginMenuInvoker(session);

    addMenuSection(menu, QStringLiteral("分辨率"));
    auto* res_group = new QActionGroup(&menu);
    res_group->setExclusive(true);
    const QStringList resolutions = {QStringLiteral("720p"), QStringLiteral("1080p"), QStringLiteral("1440p")};
    for (int i = 0; i < resolutions.size(); ++i) {
        auto* action = menu.addAction(resolutions[i]);
        action->setCheckable(true);
        action->setChecked(session->qualityIndex() == i);
        res_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, session, i] {
            if (currentSession() == session) session->setQuality(i);
        });
    }

    plugin_menu_.append(menu, QStringLiteral("display"), invoke, QStringLiteral("resolution"));
    addMenuSection(menu, session->playing() && session->confirmedBitrate()
        ? QStringLiteral("画质码率 · 被控端上限 %1 Mbps").arg(session->confirmedBitrate() / 1'000'000.0, 0, 'g', 4)
        : QStringLiteral("画质码率"));
    auto* br_group = new QActionGroup(&menu);
    br_group->setExclusive(true);
    const QStringList bitrates = {
        QStringLiteral("省流 · 2 Mbps"),
        QStringLiteral("自动 · 8 Mbps"),
        QStringLiteral("清晰 · 16 Mbps")
    };
    for (int i = 0; i < bitrates.size(); ++i) {
        auto* action = menu.addAction(bitrates[i]);
        action->setCheckable(true);
        action->setChecked(session->bitrateIndex() == i);
        br_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, session, i] {
            if (currentSession() == session) session->setBitrate(i);
        });
    }

    plugin_menu_.append(menu, QStringLiteral("display"), invoke, QStringLiteral("bitrate"));
    menu.addSeparator();
    auto* overlays = menu.addMenu(QStringLiteral("画面浮层"));
    auto* hide = overlays->addAction(QStringLiteral("隐藏全部浮层"));
    hide->setCheckable(true);
    hide->setChecked(!AppSettings::instance().data().show_diagnostics);
    connect(hide, &QAction::toggled, this, [](bool checked) {
        auto& settings = AppSettings::instance();
        auto next = settings.data();
        next.show_diagnostics = !checked;
        settings.save(next);
    });
    session->refreshOverlayItems();
    std::map<std::wstring, const p2premote::PluginOverlayItem*> plugins;
    for (const auto& item : session->overlayItems()) plugins.try_emplace(item.plugin_id, &item);
    if (!plugins.empty()) overlays->addSeparator();
    for (const auto& [plugin, item] : plugins) {
        const auto id = QString::fromStdWString(plugin);
        auto* toggle = overlays->addAction(QString::fromStdWString(item->title));
        toggle->setCheckable(true);
        toggle->setChecked(AppSettings::overlayVisibility(id).value_or(item->default_visible));
        connect(toggle, &QAction::toggled, this, [id](bool checked) {
            AppSettings::setOverlayVisibility(id, checked);
        });
    }
    plugin_menu_.append(*overlays, QStringLiteral("display"), invoke, QStringLiteral("overlays"));
    plugin_menu_.append(menu, QStringLiteral("display"), invoke);
}

void RemoteWindow::populateAudioMenu(QMenu& menu, RemoteSessionTab* session) {
    addMenuSection(menu, QStringLiteral("音频设备"));

    auto* speaker = menu.addAction(QStringLiteral("远端扬声器声音"));
    speaker->setCheckable(true);
    speaker->setChecked(session->speakerEnabled());
    connect(speaker, &QAction::toggled, this, [this, session](bool checked) {
        if (currentSession() == session) {
            session->setSpeakerEnabled(checked);
            refreshChrome();
        }
    });

    auto* mic = menu.addAction(QStringLiteral("本机麦克风（对讲 / 虚拟麦）"));
    mic->setCheckable(true);
    mic->setChecked(session->micEnabled());
    connect(mic, &QAction::toggled, this, [this, session](bool checked) {
        if (currentSession() == session) {
            session->setMicEnabled(checked);
            refreshChrome();
        }
    });

    addMenuSection(menu, QStringLiteral("扬声器音量"));
    auto* vol_group = new QActionGroup(&menu);
    vol_group->setExclusive(true);
    struct VolumePreset { const char* label; float value; };
    const VolumePreset presets[] = {{"100%", 1.0f}, {"75%", 0.75f}, {"50%", 0.5f}, {"25%", 0.25f}};
    for (const auto& preset : presets) {
        auto* action = menu.addAction(QString::fromUtf8(preset.label));
        action->setCheckable(true);
        action->setChecked(std::abs(session->speakerVolume() - preset.value) < 0.05f);
        vol_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, session, val = preset.value] {
            if (currentSession() == session) {
                session->setSpeakerVolume(val);
                if (!session->speakerEnabled()) session->setSpeakerEnabled(true);
                refreshChrome();
            }
        });
    }

    addMenuSection(menu, QStringLiteral("对讲输出"));
    auto* fallback = menu.addAction(QStringLiteral("远端无虚拟声卡时由扬声器播放"));
    fallback->setCheckable(true);
    fallback->setChecked(session->micFallback());
    connect(fallback, &QAction::toggled, this, [this, session](bool checked) {
        if (currentSession() == session) session->setMicFallback(checked);
    });
    plugin_menu_.append(menu, QStringLiteral("audio"), pluginMenuInvoker(session));
}

void RemoteWindow::populateActionsMenu(QMenu& menu, RemoteSessionTab* session) {
    addMenuSection(menu, QStringLiteral("连接"));

    auto* reconnect = menu.addAction(QStringLiteral("重新连接"));
    reconnect->setEnabled(session->handle() && !session->active());
    connect(reconnect, &QAction::triggered, this, [this, session] {
        if (currentSession() == session) session->start();
    });

    auto* disconnect = menu.addAction(QStringLiteral("断开连接"));
    disconnect->setEnabled(session->handle() && session->active());
    connect(disconnect, &QAction::triggered, this, [this, session] {
        if (currentSession() == session) session->stop();
    });

    addMenuSection(menu, QStringLiteral("诊断"));
    auto* csv = menu.addAction(QStringLiteral("导出连接诊断 CSV…"));
    csv->setEnabled(session->canExport());
    connect(csv, &QAction::triggered, this, [this, session] {
        if (currentSession() != session || !session->handle()) return;
        const auto file = QFileDialog::getSaveFileName(this, QStringLiteral("导出连接诊断"),
            QStringLiteral("viewer-stream.csv"), QStringLiteral("CSV 文件 (*.csv)"));
        if (!file.isEmpty() && p2p_stream_export_csv(session->handle(), file.toStdWString().c_str()) != P2P_OK) {
            QMessageBox::warning(this, QStringLiteral("无法导出 CSV"), QString::fromWCharArray(p2p_last_error()));
        }
    });

    const bool shared = agent_broker_ && agent_broker_->active();
    auto* share = menu.addAction(shared ? QStringLiteral("停止本机 Agent 共享")
        : QStringLiteral("共享诊断给本机 Agent…"));
    share->setEnabled(shared || (session->playing() && account_ != nullptr));
    connect(share, &QAction::triggered, this, [this, shared] {
        if (shared) stopAgentSharing();
        else shareAgentDiagnostics();
    });

    plugin_menu_.append(menu, QStringLiteral("actions"), pluginMenuInvoker(session));
}

void RemoteWindow::shareAgentDiagnostics() {
    auto* session = currentSession();
    if (!session || !session->playing() || !account_) return;
    if (agent_dialog_) agent_dialog_->close();
    if (!agent_broker_) agent_broker_ = std::make_unique<AgentBroker>(this);
    const QPointer<RemoteSessionTab> target(session);
    QMap<QString, QStringList> reports;
    for (const auto& plugin : AppSettings::discoverPlugins()) {
        reports.insert(plugin.agent_reports);
    }
    if (!agent_broker_->start(account_, session->device(), [target] {
        return target && target->playing() ? target->handle() : nullptr;
    }, session->diagnostics(), std::move(reports))) {
        QMessageBox::warning(this, QStringLiteral("AI Agent 诊断"), QStringLiteral("无法启动本机诊断共享。"));
        return;
    }
    agent_target_ = session;
    refreshChrome();

    auto* dialog = new FramelessDialog(QStringLiteral("本机 Agent 诊断共享"), false, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    agent_dialog_ = dialog;
    dialog->resize(470, 215);
    auto* summary = new QLabel(QStringLiteral("已选择 %1。每次查询仍须被控端批准。").arg(session->name()), dialog);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    dialog->contentLayout()->addWidget(summary);
    auto* token = new QLineEdit(agent_broker_->token(), dialog);
    token->setReadOnly(true);
    token->setAccessibleName(QStringLiteral("本机接入码"));
    dialog->contentLayout()->addWidget(token);
    auto* stop = new QPushButton(QStringLiteral("停止共享"), dialog);
    connect(stop, &QPushButton::clicked, this, &RemoteWindow::stopAgentSharing);
    dialog->contentLayout()->addWidget(stop);
    dialog->show();
    token->selectAll();
}

void RemoteWindow::stopAgentSharing() {
    if (agent_broker_) agent_broker_->stop();
    agent_target_.clear();
    if (agent_dialog_) agent_dialog_->close();
    refreshChrome();
}
