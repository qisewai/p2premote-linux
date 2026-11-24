#include "remote_window.h"
#include "plugin_dialog.h"

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
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QVBoxLayout>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace {

constexpr auto remote_stylesheet = R"(
QWidget#remoteWindow { background: #0f131a; color: #e2e8f0; }
QFrame#remoteTopbar {
    background: #151b26;
    border-bottom: 1px solid #232d3f;
}
QTabBar#remoteTabs {
    background: transparent;
    border: 0;
}
QTabBar#remoteTabs::tab {
    background: transparent;
    color: #94a3b8;
    border: 0;
    border-top: 2px solid transparent;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 7px 14px;
    margin-right: 3px;
    min-width: 120px;
    max-width: 240px;
    font-size: 13px;
}
QTabBar#remoteTabs::tab:hover {
    background: #1d2536;
    color: #e2e8f0;
}
QTabBar#remoteTabs::tab:selected {
    background: #212b3e;
    color: #ffffff;
    border-top: 2px solid #3b82f6;
    font-weight: 600;
}
QPushButton#perfPill {
    background: #1e2636;
    color: #cbd5e1;
    border: 1px solid #2b374d;
    border-radius: 11px;
    padding: 2px 10px;
    min-height: 18px;
    font-size: 12px;
}
QPushButton#perfPill:hover {
    background: #273247;
    color: #ffffff;
}
QPushButton#barAddBtn {
    background: transparent;
    color: #94a3b8;
    border: 0;
    border-radius: 5px;
    padding: 3px 8px;
    min-height: 20px;
    font-size: 16px;
    font-weight: 600;
}
QPushButton#barAddBtn:hover {
    background: #1e2636;
    color: #ffffff;
}
QPushButton#barMenuBtn {
    background: #1e2636;
    color: #e2e8f0;
    border: 1px solid #2d3a52;
    border-radius: 5px;
    padding: 4px 10px;
    min-height: 20px;
    font-size: 13px;
}
QPushButton#barMenuBtn:hover, QPushButton#barMenuBtn:focus {
    background: #28344b;
    border-color: #4b658f;
    color: #ffffff;
}
QPushButton#barMenuBtn:disabled {
    background: #171d29;
    color: #55647a;
    border-color: #20293a;
}
QPushButton#barMenuBtn[micLive="true"] {
    background: #3b1d26;
    color: #fecdd3;
    border-color: #e11d48;
}
QPushButton#barMenuBtn[micLive="true"]:hover {
    background: #4c222f;
    color: #ffffff;
}
QFrame#remoteStatusbar {
    background: #151b26;
    border-top: 1px solid #232d3f;
}
QLabel#remoteStatusText {
    color: #94a3b8;
    font-size: 12px;
}
QLabel#remoteMetricsText {
    color: #cbd5e1;
    font-size: 12px;
}
QWidget#video { background: #000000; }
)";

constexpr auto dark_menu_stylesheet = R"(
QMenu {
    background: #1a2230;
    color: #e2e8f0;
    border: 1px solid #2f3d56;
    border-radius: 6px;
    padding: 5px 0;
    font-size: 13px;
}
QMenu::item {
    padding: 6px 22px 6px 26px;
}
QMenu::item:selected {
    background: #283750;
    color: #ffffff;
}
QMenu::item:disabled {
    color: #5a6a82;
}
QMenu::separator {
    height: 1px;
    background: #29354a;
    margin: 4px 10px;
}
)";

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

QString route(int path) {
    switch (path) {
    case P2P_ICE_HOST: return QStringLiteral("主机直连");
    case P2P_ICE_NAT: return QStringLiteral("NAT 直连");
    case P2P_ICE_RELAY: return QStringLiteral("TURN 中继");
    default: return QStringLiteral("获取链路中");
    }
}

QString milliseconds(double value) {
    return value < 0 ? QStringLiteral("—") : QStringLiteral("%1 ms").arg(value, 0, 'f', 1);
}

QString bandwidth(uint32_t value) {
    if (!value) return QStringLiteral("—");
    if (value < 1000000) return QStringLiteral("%1 Kbps").arg(value / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 Mbps").arg(value / 1000000.0, 0, 'f', 1);
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
    bool speakerEnabled() const { return speaker_enabled_; }
    bool micEnabled() const { return mic_enabled_; }
    float speakerVolume() const { return speaker_volume_; }
    bool micFallback() const { return mic_fallback_; }
    const QString& statusText() const { return status_text_; }
    const QString& noticeText() const { return notice_text_; }
    const QString& metricsText() const { return metrics_text_; }
    const QString& pillText() const { return pill_text_; }

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
    void onSnapshot(const p2p_stream_snapshot_v1& snapshot) {
        state_ = snapshot.state;
        seconds_ = snapshot.seconds;
        width_ = snapshot.width;
        height_ = snapshot.height;
        has_plugin_menu_ = snapshot.has_plugin_menu != 0;

        if (dialogs_) {
            if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
            if (!active()) dialogs_->closeAll();
        }

        status_text_ = QString::fromWCharArray(snapshot.status);
        if (snapshot.input_ready) status_text_ += QStringLiteral("  ·  点击画面接管键鼠，F12 释放");
        notice_text_ = *snapshot.plugin_notice ? QString::fromWCharArray(snapshot.plugin_notice) : QString{};

        metrics_text_.clear();
        pill_text_.clear();
        if (playing() && snapshot.width && snapshot.height) {
            if (snapshot.seconds > previous_seconds_ && snapshot.presented >= previous_frames_) {
                fps_ = (snapshot.presented - previous_frames_) / (snapshot.seconds - previous_seconds_);
                previous_frames_ = snapshot.presented;
                previous_seconds_ = snapshot.seconds;
            }
            const auto codec = QString::fromWCharArray(snapshot.codec);
            const auto protocol = QString::fromUtf8(snapshot.ice_protocol).toUpper();
            const auto path = route(snapshot.ice_path) + (protocol.isEmpty() ? QString{} : QStringLiteral("/") + protocol);
            const auto loss = snapshot.loss_fraction < 0 ? QStringLiteral("—")
                : QStringLiteral("%1%").arg(snapshot.loss_fraction * 100.0, 0, 'f', 1);
            pill_text_ = QStringLiteral("%1×%2 · %3 fps · %4")
                .arg(snapshot.width).arg(snapshot.height).arg(fps_, 0, 'f', 0).arg(milliseconds(snapshot.rtt_ms));
            metrics_text_ = QStringLiteral("%1 × %2 · %3 fps · %4 · %5 · ↓ %6 ↑ %7 · RTT %8 · 丢包 %9 · 解码 %10")
                .arg(snapshot.width).arg(snapshot.height).arg(fps_, 0, 'f', 1).arg(codec, path)
                .arg(bandwidth(snapshot.download_bps), bandwidth(snapshot.upload_bps), milliseconds(snapshot.rtt_ms),
                     loss, milliseconds(snapshot.decoded_frame_wait_ms));
        } else {
            previous_seconds_ = fps_ = 0;
            previous_frames_ = 0;
        }
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
    bool speaker_enabled_{true};
    bool mic_enabled_{false};
    float speaker_volume_{1.0f};
    bool mic_fallback_{true};
    bool tab_focused_{true};
    double previous_seconds_{}, fps_{};
    uint64_t previous_frames_{};
    QString status_text_{QStringLiteral("正在连接设备…")};
    QString notice_text_;
    QString metrics_text_;
    QString pill_text_;
};

RemoteWindow::RemoteWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("remoteWindow"));
    setStyleSheet(QString::fromUtf8(remote_stylesheet));
    setWindowTitle(QStringLiteral("P2P Remote"));
    setMinimumSize(820, 480);
    resize(1160, 720);

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    topbar_ = new QFrame(this);
    topbar_->setObjectName(QStringLiteral("remoteTopbar"));
    auto* top = new QHBoxLayout(topbar_);
    top->setContentsMargins(10, 5, 10, 0);
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

    perf_pill_ = new QPushButton(topbar_);
    perf_pill_->setObjectName(QStringLiteral("perfPill"));
    perf_pill_->setToolTip(QStringLiteral("点击展开/收起详细诊断"));
    perf_pill_->hide();
    top->addWidget(perf_pill_);

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

    layout_->addWidget(topbar_);

    stack_ = new QStackedWidget(this);
    layout_->addWidget(stack_, 1);

    statusbar_ = new QFrame(this);
    statusbar_->setObjectName(QStringLiteral("remoteStatusbar"));
    auto* bottom = new QHBoxLayout(statusbar_);
    bottom->setContentsMargins(12, 4, 12, 4);
    bottom->setSpacing(12);
    status_text_ = new QLabel(statusbar_);
    status_text_->setObjectName(QStringLiteral("remoteStatusText"));
    status_text_->setTextFormat(Qt::PlainText);
    bottom->addWidget(status_text_, 1);
    metrics_text_ = new QLabel(statusbar_);
    metrics_text_->setObjectName(QStringLiteral("remoteMetricsText"));
    metrics_text_->setTextFormat(Qt::PlainText);
    bottom->addWidget(metrics_text_);
    layout_->addWidget(statusbar_);

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
    connect(perf_pill_, &QPushButton::clicked, this, [this] {
        show_diagnostics_ = !show_diagnostics_;
        refreshChrome();
    });
    connect(display_menu_, &QPushButton::clicked, this, &RemoteWindow::showDisplayMenu);
    connect(audio_menu_, &QPushButton::clicked, this, &RemoteWindow::showAudioMenu);
    connect(plugins_menu_, &QPushButton::clicked, this, &RemoteWindow::showPluginsMenu);
    connect(actions_menu_, &QPushButton::clicked, this, &RemoteWindow::showActionsMenu);
    connect(fullscreen_, &QPushButton::clicked, this, &RemoteWindow::toggleFullscreen);

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

void RemoteWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (auto* session = currentSession()) session->syncVideo();
    }
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

    const auto* current = currentSession();
    if (!current) {
        setWindowTitle(QStringLiteral("P2P Remote"));
        perf_pill_->hide();
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

    const auto& pill = current->pillText();
    if (!pill.isEmpty()) {
        if (perf_pill_->text() != pill) perf_pill_->setText(pill);
        perf_pill_->show();
    } else {
        perf_pill_->hide();
    }

    QString left = current->statusText();
    if (!current->noticeText().isEmpty()) {
        left += QStringLiteral("  ·  ") + current->noticeText();
    }
    if (status_text_->text() != left) status_text_->setText(left);

    const bool show_metrics = show_diagnostics_ && !current->metricsText().isEmpty();
    if (show_metrics && metrics_text_->text() != current->metricsText()) {
        metrics_text_->setText(current->metricsText());
    }
    metrics_text_->setVisible(show_metrics);

    const bool need_statusbar = !fullscreen_mode_
        && (!current->playing() || !current->noticeText().isEmpty() || show_metrics);
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
        statusbar_->hide();
        showFullScreen();
        hover_.start();
        positionFullscreenToolbar();
    } else {
        hover_.stop();
        topbar_->hide();
        topbar_->setParent(this, Qt::Widget);
        layout_->insertWidget(0, topbar_);
        fullscreen_->setText(QStringLiteral("全屏"));
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
    menu->setStyleSheet(QString::fromUtf8(dark_menu_stylesheet));

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
    auto* br_header = menu->addAction(QStringLiteral("画质码率"));
    br_header->setEnabled(false);
    auto* br_group = new QActionGroup(menu);
    br_group->setExclusive(true);
    const QStringList bitrates = {
        QStringLiteral("省流 · 4 Mbps"),
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
    auto* diag = menu->addAction(QStringLiteral("显示底部详细诊断"));
    diag->setCheckable(true);
    diag->setChecked(show_diagnostics_);
    connect(diag, &QAction::toggled, this, [this](bool checked) {
        show_diagnostics_ = checked;
        refreshChrome();
    });

    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(display_menu_->mapToGlobal(QPoint(0, display_menu_->height())));
}

void RemoteWindow::showAudioMenu() {
    auto* session = currentSession();
    if (!session || !session->handle()) return;

    auto* menu = new QMenu(audio_menu_);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setStyleSheet(QString::fromUtf8(dark_menu_stylesheet));

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
    menu->setStyleSheet(QString::fromUtf8(dark_menu_stylesheet));

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
    menu->setStyleSheet(QString::fromUtf8(dark_menu_stylesheet));

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
    auto* close_tab = menu->addAction(QStringLiteral("关闭当前标签页"));
    connect(close_tab, &QAction::triggered, this, [this] {
        closeTab(tabs_->currentIndex());
    });

    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(actions_menu_->mapToGlobal(QPoint(0, actions_menu_->height())));
}
