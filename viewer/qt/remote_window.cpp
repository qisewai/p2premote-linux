#include "remote_window.h"
#include "app_settings.h"
#include "frameless_chrome.h"
#include "plugin_dialog.h"
#include "settings_dialog.h"
#include "../connection_overlay.h"

#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWindow>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace {

class VideoContainer final : public QWidget {
public:
    explicit VideoContainer(std::function<void()> toggle, QWidget* parent)
        : QWidget(parent), toggle_(std::move(toggle)) {
        setObjectName(QStringLiteral("video"));
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        const auto* msg = static_cast<MSG*>(message);
        if (msg->message == P2P_VIDEO_FULLSCREEN_MESSAGE) {
            toggle_();
            *result = 0;
            return true;
        }
        return QWidget::nativeEvent(type, message, result);
    }

private:
    std::function<void()> toggle_;
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

} // namespace

class RemoteSessionTab final : public QWidget {
public:
    RemoteSessionTab(p2p_account* account, const QString& device, const QString& name,
        const LaunchOptions& options, std::function<void()> on_fullscreen,
        std::function<void()> on_changed, QWidget* dialog_parent, QWidget* parent)
        : QWidget(parent), device_(device), name_(name), on_changed_(std::move(on_changed)) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        video_ = new VideoContainer(std::move(on_fullscreen), this);
        video_->installEventFilter(this);
        layout->addWidget(video_, 1);

        const auto target = device.toStdWString();
        driver_ = std::make_unique<StreamDriver>(account, false, target.c_str(),
            reinterpret_cast<void*>(video_->winId()), options, this);
        if (!driver_->handle()) {
            status_text_ = QString::fromWCharArray(p2p_last_error());
            state_ = P2P_STOPPED;
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
        });
        connect(&AppSettings::instance(), &AppSettings::pluginConfigChanged, this, [this](const QString& id) {
            if (overlay_configs_.erase(id.toStdWString())) updateConnectionOverlays();
        });

        dialogs_ = new PluginDialogManager(dialog_parent, this);
        connect(driver_.get(), &StreamDriver::changed, this, [this](const p2p_stream_snapshot_v1& snap) {
            onSnapshot(snap);
        });
        connect(driver_.get(), &StreamDriver::uiCommandsReady, this, [this] {
            if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
        });
    }

    ~RemoteSessionTab() override {
        if (dialogs_) dialogs_->closeAll();
    }

    const QString& device() const { return device_; }
    const QString& name() const { return name_; }
    p2p_stream* handle() const { return driver_ ? driver_->handle() : nullptr; }
    StreamDriver* driver() const { return driver_.get(); }
    int state() const { return state_; }
    bool active() const { return state_ != P2P_IDLE && state_ != P2P_STOPPED; }
    bool playing() const { return state_ == P2P_STREAMING; }
    bool hasPluginMenu() const { return has_plugin_menu_; }
    bool canExport() const { return !active() && seconds_ > 0; }
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
        if (playing() && width_ && height_) {
            return QStringLiteral("%1 · 屏幕1").arg(name_);
        }
        return name_;
    }

    QIcon tabIcon() const {
        if (playing()) return statusDotIcon(QColor(0x22, 0xc5, 0x5e));
        if (active()) return statusDotIcon(QColor(0xf5, 0x9e, 0x0b));
        return statusDotIcon(QColor(0x64, 0x74, 0x8b));
    }

    void start() {
        if (driver_ && driver_->handle()) {
            syncVideo();
            driver_->update(p2p_stream_start(driver_->handle(), nullptr, 60));
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
        if (!driver_ || !driver_->handle()) return;
        const p2p_audio_options_v1 opt{
            (speaker_enabled_ && tab_focused_) ? 1 : 0,
            (mic_enabled_ && tab_focused_) ? 1 : 0,
            speaker_volume_,
            mic_fallback_ ? 1 : 0
        };
        p2p_stream_set_audio(driver_->handle(), &opt);
    }

    void syncVideo() {
        if (driver_) driver_->resize(video_->isVisible() && !window()->isMinimized());
        updateConnectionOverlays();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == video_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::Hide || event->type() == QEvent::DevicePixelRatioChange)) {
            syncVideo();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void updateConnectionOverlays(bool force = true) {
        if (!AppSettings::instance().data().show_diagnostics || !playing() || !video_->isVisible() || window()->isMinimized()) {
            overlay_timer_.stop();
            overlays_.Hide();
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
        overlays_.Render(reinterpret_cast<HWND>(video_->winId()), width_, height_, items);
        overlay_dirty_ = false;
    }

    void onSnapshot(const p2p_stream_snapshot_v1& snapshot) {
        state_ = snapshot.state;
        seconds_ = snapshot.seconds;
        width_ = snapshot.width;
        height_ = snapshot.height;
        confirmed_bitrate_ = snapshot.bitrate_limit_bps;
        has_plugin_menu_ = snapshot.has_plugin_menu != 0;

        if (dialogs_) {
            if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
            if (!active()) dialogs_->closeAll();
        }

        status_text_ = QString::fromWCharArray(snapshot.status);
        if (snapshot.input_ready) status_text_ += QStringLiteral("  ·  点击画面接管键鼠，F12 释放");
        notice_text_ = *snapshot.plugin_notice ? QString::fromWCharArray(snapshot.plugin_notice) : QString{};

        updateConnectionOverlays();
        if (on_changed_) on_changed_();
    }

    QString device_;
    QString name_;
    std::function<void()> on_changed_;
    QWidget* video_{};
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
    QString status_text_{QStringLiteral("正在连接设备…")};
    QString notice_text_;
    QTimer overlay_timer_;
    p2premote::viewer::OverlayManager overlays_;
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
    topbar_->installEventFilter(this);
    auto* top = new QHBoxLayout(topbar_);
    top->setContentsMargins(10, 4, 6, 0);
    top->setSpacing(6);

    tabs_ = new QTabBar(topbar_);
    tabs_->setObjectName(QStringLiteral("remoteTabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    tabs_->setElideMode(Qt::ElideRight);
    top->addWidget(tabs_);

    add_tab_ = new QPushButton(QStringLiteral("+"), topbar_);
    add_tab_->setObjectName(QStringLiteral("barAddBtn"));
    add_tab_->setToolTip(QStringLiteral("连接其他设备"));
    top->addWidget(add_tab_);
    top->addStretch(1);


    auto addMenuBtn = [&](const QString& label) {
        auto* btn = new QPushButton(label, topbar_);
        btn->setObjectName(QStringLiteral("barMenuBtn"));
        top->addWidget(btn);
        return btn;
    };
    display_menu_ = addMenuBtn(QStringLiteral("画面 ▾"));
    audio_menu_ = addMenuBtn(QStringLiteral("音频 ▾"));
    plugins_menu_ = addMenuBtn(QStringLiteral("插件 ▾"));
    actions_menu_ = addMenuBtn(QStringLiteral("菜单 ▾"));
    fullscreen_ = addMenuBtn(QStringLiteral("全屏"));
    fullscreen_->setToolTip(QStringLiteral("F11"));

    win_min_ = new QPushButton(QStringLiteral("—"), topbar_);
    win_min_->setObjectName(QStringLiteral("titleBtn"));
    win_min_->setToolTip(QStringLiteral("最小化"));
    win_min_->setFocusPolicy(Qt::NoFocus);
    top->addWidget(win_min_);

    win_max_ = new QPushButton(QStringLiteral("□"), topbar_);
    win_max_->setObjectName(QStringLiteral("titleBtn"));
    win_max_->setToolTip(QStringLiteral("最大化"));
    win_max_->setFocusPolicy(Qt::NoFocus);
    top->addWidget(win_max_);

    win_close_ = new QPushButton(QStringLiteral("✕"), topbar_);
    win_close_->setObjectName(QStringLiteral("titleCloseBtn"));
    win_close_->setToolTip(QStringLiteral("关闭窗口"));
    win_close_->setFocusPolicy(Qt::NoFocus);
    top->addWidget(win_close_);

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
    bottom->addWidget(status_text_, 1);
    layout_->addWidget(statusbar_);

    outer->addWidget(shell_);

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
    connect(display_menu_, &QPushButton::clicked, this, &RemoteWindow::showDisplayMenu);
    connect(audio_menu_, &QPushButton::clicked, this, &RemoteWindow::showAudioMenu);
    connect(plugins_menu_, &QPushButton::clicked, this, &RemoteWindow::showPluginsMenu);
    connect(actions_menu_, &QPushButton::clicked, this, &RemoteWindow::showActionsMenu);
    connect(fullscreen_, &QPushButton::clicked, this, &RemoteWindow::toggleFullscreen);
    connect(win_min_, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(win_max_, &QPushButton::clicked, this, [this] {
        if (isMaximized()) showNormal();
        else showMaximized();
        syncWindowControls();
    });
    connect(win_close_, &QPushButton::clicked, this, &QWidget::close);

    auto* shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, &RemoteWindow::toggleFullscreen);
    hover_.setInterval(60);
    connect(&hover_, &QTimer::timeout, this, &RemoteWindow::positionFullscreenToolbar);
}

RemoteWindow::~RemoteWindow() = default;

void RemoteWindow::openSession(p2p_account* account, const QString& device, const QString& name,
    const LaunchOptions& options) {
    if (const int existing = findSession(device); existing >= 0) {
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
        account, device, name, options,
        [this] { toggleFullscreen(); },
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

void RemoteWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!dwm_applied_) {
        dwm_applied_ = true;
        applyWin32FramelessChrome(this);
    }
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
    if (watched == topbar_ && !fullscreen_mode_) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && !topbar_->childAt(me->position().toPoint()) && windowHandle()) {
                if (windowHandle()->startSystemMove()) return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && !topbar_->childAt(me->position().toPoint())) {
                if (isMaximized()) showNormal();
                else showMaximized();
                syncWindowControls();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool RemoteWindow::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
    if (handleFramelessResizeHitTest(this, message, result)) return true;
    return QWidget::nativeEvent(type, message, result);
}

void RemoteWindow::syncWindowControls() {
    const bool show_controls = !fullscreen_mode_;
    if (win_min_) win_min_->setVisible(show_controls);
    if (win_max_) {
        win_max_->setVisible(show_controls);
        const bool maxed = isMaximized();
        win_max_->setText(maxed ? QStringLiteral("❐") : QStringLiteral("□"));
        win_max_->setToolTip(maxed ? QStringLiteral("还原") : QStringLiteral("最大化"));
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

int RemoteWindow::findSession(const QString& device) const {
    for (int i = 0; i < static_cast<int>(sessions_.size()); ++i) {
        if (sessions_[static_cast<size_t>(i)]->device() == device) return i;
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
    refreshChrome();
}

void RemoteWindow::closeTab(int index) {
    auto* session = sessionAt(index);
    if (!session) return;
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
        tabs_->setTabIcon(i, session->tabIcon());
    }

    auto* current = currentSession();
    if (!current) {
        setWindowTitle(QStringLiteral("P2P Remote"));
        display_menu_->setEnabled(false);
        audio_menu_->setEnabled(false);
        plugins_menu_->setEnabled(false);
        actions_menu_->setEnabled(false);
        statusbar_->hide();
        return;
    }

    setWindowTitle(QStringLiteral("%1 · P2P Remote").arg(current->name()));
    const bool has_handle = current->handle() != nullptr;
    display_menu_->setEnabled(has_handle);
    audio_menu_->setEnabled(has_handle);
    plugins_menu_->setEnabled(has_handle && current->hasPluginMenu());
    actions_menu_->setEnabled(has_handle);

    const bool mic_live = has_handle && current->playing() && current->micEnabled();
    const auto audio_label = mic_live ? QStringLiteral("音频 · 麦开 ▾")
        : (!current->speakerEnabled() ? QStringLiteral("音频 · 静音 ▾") : QStringLiteral("音频 ▾"));
    if (audio_menu_->text() != audio_label) audio_menu_->setText(audio_label);
    if (audio_menu_->property("micLive").toBool() != mic_live) {
        audio_menu_->setProperty("micLive", mic_live);
        audio_menu_->style()->unpolish(audio_menu_);
        audio_menu_->style()->polish(audio_menu_);
    }

    QString left = current->statusText();
    if (!current->noticeText().isEmpty()) {
        left += QStringLiteral("  ·  ") + current->noticeText();
    }
    if (status_text_->text() != left) status_text_->setText(left);

    const bool need_statusbar = !fullscreen_mode_
        && (!current->playing() || !current->noticeText().isEmpty());
    statusbar_->setVisible(need_statusbar);
}

void RemoteWindow::toggleFullscreen() {
    fullscreen_mode_ = !fullscreen_mode_;
    if (fullscreen_mode_) {
        was_maximized_ = isMaximized();
        layout_->removeWidget(topbar_);
        topbar_->setParent(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        topbar_->setAttribute(Qt::WA_ShowWithoutActivating);
        fullscreen_->setText(QStringLiteral("退出全屏"));
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
        fullscreen_->setText(QStringLiteral("全屏"));
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

void RemoteWindow::showDisplayMenu() {
    auto* session = currentSession();
    if (!session || !session->handle()) return;

    auto* menu = new QMenu(display_menu_);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* res_header = menu->addAction(QStringLiteral("分辨率"));
    res_header->setEnabled(false);
    auto* res_group = new QActionGroup(menu);
    res_group->setExclusive(true);
    const QStringList resolutions = {QStringLiteral("720p"), QStringLiteral("1080p"), QStringLiteral("1440p")};
    for (int i = 0; i < resolutions.size(); ++i) {
        auto* action = menu->addAction(resolutions[i]);
        action->setCheckable(true);
        action->setChecked(session->qualityIndex() == i);
        res_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, session, i] {
            if (currentSession() == session) session->setQuality(i);
        });
    }

    menu->addSeparator();
    auto* br_header = menu->addAction(session->playing() && session->confirmedBitrate()
        ? QStringLiteral("画质码率 · 被控端上限 %1 Mbps").arg(session->confirmedBitrate() / 1'000'000.0, 0, 'g', 4)
        : QStringLiteral("画质码率"));
    br_header->setEnabled(false);
    auto* br_group = new QActionGroup(menu);
    br_group->setExclusive(true);
    const QStringList bitrates = {
        QStringLiteral("省流 · 2 Mbps"),
        QStringLiteral("自动 · 8 Mbps"),
        QStringLiteral("清晰 · 16 Mbps")
    };
    for (int i = 0; i < bitrates.size(); ++i) {
        auto* action = menu->addAction(bitrates[i]);
        action->setCheckable(true);
        action->setChecked(session->bitrateIndex() == i);
        br_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, session, i] {
            if (currentSession() == session) session->setBitrate(i);
        });
    }

    menu->addSeparator();
    auto* overlays = menu->addMenu(QStringLiteral("画面浮层"));
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

    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(display_menu_->mapToGlobal(QPoint(0, display_menu_->height())));
}

void RemoteWindow::showAudioMenu() {
    auto* session = currentSession();
    if (!session || !session->handle()) return;

    auto* menu = new QMenu(audio_menu_);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* speaker = menu->addAction(QStringLiteral("远端扬声器声音"));
    speaker->setCheckable(true);
    speaker->setChecked(session->speakerEnabled());
    connect(speaker, &QAction::toggled, this, [this, session](bool checked) {
        if (currentSession() == session) {
            session->setSpeakerEnabled(checked);
            refreshChrome();
        }
    });

    auto* mic = menu->addAction(QStringLiteral("本机麦克风（对讲 / 虚拟麦）"));
    mic->setCheckable(true);
    mic->setChecked(session->micEnabled());
    connect(mic, &QAction::toggled, this, [this, session](bool checked) {
        if (currentSession() == session) {
            session->setMicEnabled(checked);
            refreshChrome();
        }
    });

    menu->addSeparator();
    auto* vol_header = menu->addAction(QStringLiteral("扬声器音量"));
    vol_header->setEnabled(false);
    auto* vol_group = new QActionGroup(menu);
    vol_group->setExclusive(true);
    struct VolumePreset { const char* label; float value; };
    const VolumePreset presets[] = {{"100%", 1.0f}, {"75%", 0.75f}, {"50%", 0.5f}, {"25%", 0.25f}};
    for (const auto& preset : presets) {
        auto* action = menu->addAction(QString::fromUtf8(preset.label));
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

    menu->addSeparator();
    auto* fallback = menu->addAction(QStringLiteral("远端无虚拟声卡时由扬声器播放"));
    fallback->setCheckable(true);
    fallback->setChecked(session->micFallback());
    connect(fallback, &QAction::toggled, this, [this, session](bool checked) {
        if (currentSession() == session) session->setMicFallback(checked);
    });

    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(audio_menu_->mapToGlobal(QPoint(0, audio_menu_->height())));
}

void RemoteWindow::showPluginsMenu() {
    auto* session = currentSession();
    if (!session || !session->handle()) return;

    auto* menu = new QMenu(plugins_menu_);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const auto count = p2p_stream_read_menu(session->handle());
    for (size_t i = 0; i < count; ++i) {
        p2p_menu_item entry{};
        if (p2p_stream_menu_item(session->handle(), i, &entry) != P2P_OK) continue;
        auto* action = menu->addAction(QString::fromWCharArray(entry.label));
        action->setCheckable(entry.checked >= 0);
        action->setChecked(entry.checked > 0);
        connect(action, &QAction::triggered, this, [this, session, id = entry.id] {
            if (currentSession() == session && session->driver() && session->handle()) {
                session->driver()->update(p2p_stream_invoke_menu(session->handle(), id));
            }
        });
    }
    if (menu->isEmpty()) {
        auto* empty = menu->addAction(QStringLiteral("当前无可用插件"));
        empty->setEnabled(false);
    }
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(plugins_menu_->mapToGlobal(QPoint(0, plugins_menu_->height())));
}

void RemoteWindow::showActionsMenu() {
    auto* session = currentSession();
    if (!session) return;

    auto* menu = new QMenu(actions_menu_);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* reconnect = menu->addAction(QStringLiteral("重新连接"));
    reconnect->setEnabled(session->handle() && !session->active());
    connect(reconnect, &QAction::triggered, this, [this, session] {
        if (currentSession() == session) session->start();
    });

    auto* disconnect = menu->addAction(QStringLiteral("断开连接"));
    disconnect->setEnabled(session->handle() && session->active());
    connect(disconnect, &QAction::triggered, this, [this, session] {
        if (currentSession() == session) session->stop();
    });

    menu->addSeparator();
    auto* csv = menu->addAction(QStringLiteral("导出连接诊断 CSV…"));
    csv->setEnabled(session->canExport());
    connect(csv, &QAction::triggered, this, [this, session] {
        if (currentSession() != session || !session->handle()) return;
        const auto file = QFileDialog::getSaveFileName(this, QStringLiteral("导出连接诊断"),
            QStringLiteral("viewer-stream.csv"), QStringLiteral("CSV 文件 (*.csv)"));
        if (!file.isEmpty() && p2p_stream_export_csv(session->handle(), file.toStdWString().c_str()) != P2P_OK) {
            QMessageBox::warning(this, QStringLiteral("无法导出 CSV"), QString::fromWCharArray(p2p_last_error()));
        }
    });

    menu->addSeparator();
    auto* open_settings = menu->addAction(QStringLiteral("设置…"));
    connect(open_settings, &QAction::triggered, this, &RemoteWindow::openSettings);

    auto* close_tab = menu->addAction(QStringLiteral("关闭当前标签页"));
    connect(close_tab, &QAction::triggered, this, [this] {
        closeTab(tabs_->currentIndex());
    });

    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(actions_menu_->mapToGlobal(QPoint(0, actions_menu_->height())));
}
