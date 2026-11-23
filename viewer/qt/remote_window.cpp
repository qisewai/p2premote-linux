#include "remote_window.h"
#include "plugin_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <windows.h>

#include <algorithm>
#include <functional>
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

QString route(int path) {
    switch (path) {
    case P2P_ICE_HOST: return QStringLiteral("主机候选直连");
    case P2P_ICE_NAT: return QStringLiteral("NAT 直连");
    case P2P_ICE_RELAY: return QStringLiteral("TURN 中继");
    default: return QStringLiteral("正在获取链路");
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

RemoteWindow::RemoteWindow(p2p_account* account, const QString& device, const QString& name,
    const LaunchOptions& options, QWidget* parent) : QWidget(parent, Qt::Window) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("%1 · P2P Remote").arg(name));
    setMinimumSize(760, 360);
    resize(1070, 690);

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    toolbar_ = new QFrame(this);
    toolbar_->setObjectName(QStringLiteral("streamToolbar"));
    auto* bar = new QVBoxLayout(toolbar_);
    bar->setContentsMargins(14, 10, 14, 10);
    bar->setSpacing(7);
    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    auto addButton = [&](const QString& label) {
        auto* button = new QPushButton(label, toolbar_);
        controls->addWidget(button);
        return button;
    };
    connect_ = addButton(QStringLiteral("重新连接"));
    stop_ = addButton(QStringLiteral("断开"));
    export_ = addButton(QStringLiteral("导出 CSV"));
    controls->addStretch();
    quality_ = new QComboBox(toolbar_);
    quality_->setAccessibleName(QStringLiteral("分辨率"));
    quality_->addItems({QStringLiteral("720p"), QStringLiteral("1080p"), QStringLiteral("1440p")});
    quality_->setCurrentIndex(1);
    controls->addWidget(quality_);
    bitrate_ = new QComboBox(toolbar_);
    bitrate_->setAccessibleName(QStringLiteral("画质"));
    bitrate_->addItems({QStringLiteral("省流 · 4 Mbps"), QStringLiteral("自动 · 8 Mbps"), QStringLiteral("清晰 · 16 Mbps")});
    bitrate_->setCurrentIndex(1);
    controls->addWidget(bitrate_);
    plugins_ = addButton(QStringLiteral("插件"));
    diagnostics_ = new QCheckBox(QStringLiteral("诊断"), toolbar_);
    diagnostics_->setChecked(true);
    controls->addWidget(diagnostics_);
    fullscreen_ = addButton(QStringLiteral("全屏"));
    fullscreen_->setToolTip(QStringLiteral("F11"));
    bar->addLayout(controls);
    status_ = new QLabel(QStringLiteral("正在连接设备…"), toolbar_);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    bar->addWidget(status_);
    metrics_ = new QLabel(toolbar_);
    metrics_->setTextFormat(Qt::PlainText);
    metrics_->setWordWrap(true);
    metrics_->hide();
    bar->addWidget(metrics_);
    layout_->addWidget(toolbar_);

    video_ = new VideoContainer([this] { toggleFullscreen(); }, this);
    video_->installEventFilter(this);
    layout_->addWidget(video_, 1);
    const auto target = device.toStdWString();
    driver_ = std::make_unique<StreamDriver>(account, false, target.c_str(),
        reinterpret_cast<void*>(video_->winId()), options, this);
    if (!driver_->handle()) {
        status_->setText(QString::fromWCharArray(p2p_last_error()));
        for (auto* button : {connect_, stop_, export_, plugins_}) button->setEnabled(false);
        quality_->setEnabled(false);
        bitrate_->setEnabled(false);
        return;
    }

    dialogs_ = new PluginDialogManager(this, this);
    connect(driver_.get(), &StreamDriver::changed, this, &RemoteWindow::showState);
    connect(driver_.get(), &StreamDriver::uiCommandsReady, this, [this] {
        if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
    });
    connect(connect_, &QPushButton::clicked, this, &RemoteWindow::start);
    connect(stop_, &QPushButton::clicked, this, [this] {
        driver_->update(p2p_stream_stop(driver_->handle(), nullptr));
    });
    connect(quality_, &QComboBox::currentIndexChanged, this, [this](int index) {
        driver_->update(p2p_stream_quality(driver_->handle(), P2P_720P + index));
    });
    connect(bitrate_, &QComboBox::currentIndexChanged, this, [this](int index) {
        driver_->update(p2p_stream_bitrate(driver_->handle(), P2P_ECONOMY + index));
    });
    connect(plugins_, &QPushButton::clicked, this, &RemoteWindow::showPlugins);
    connect(export_, &QPushButton::clicked, this, &RemoteWindow::exportCsv);
    connect(diagnostics_, &QCheckBox::toggled, this, [this](bool checked) {
        metrics_->setVisible(checked && !metrics_text_.isEmpty());
    });
    connect(fullscreen_, &QPushButton::clicked, this, &RemoteWindow::toggleFullscreen);
    auto* shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, &RemoteWindow::toggleFullscreen);
    hover_.setInterval(60);
    connect(&hover_, &QTimer::timeout, this, &RemoteWindow::positionFullscreenToolbar);
}

RemoteWindow::~RemoteWindow() {
    if (dialogs_) dialogs_->closeAll();
}

void RemoteWindow::start() {
    if (driver_ && driver_->handle()) {
        syncVideo();
        driver_->update(p2p_stream_start(driver_->handle(), nullptr, 60));
    }
}

bool RemoteWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == video_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show
        || event->type() == QEvent::Hide || event->type() == QEvent::DevicePixelRatioChange)) {
        syncVideo();
    }
    return QWidget::eventFilter(watched, event);
}

void RemoteWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) syncVideo();
}

void RemoteWindow::syncVideo() {
    if (driver_) driver_->resize(video_->isVisible() && !isMinimized());
}

void RemoteWindow::showState(const p2p_stream_snapshot_v1& snapshot) {
    const auto active = snapshot.state != P2P_IDLE && snapshot.state != P2P_STOPPED;
    playing_ = snapshot.state == P2P_STREAMING;
    if (dialogs_) {
        if (driver_ && driver_->handle()) dialogs_->poll(driver_->handle());
        if (!active) dialogs_->closeAll();
    }
    connect_->setEnabled(!active);
    stop_->setEnabled(active);
    export_->setEnabled(!active && snapshot.seconds > 0);
    plugins_->setEnabled(snapshot.has_plugin_menu != 0);

    auto text = QString::fromWCharArray(snapshot.status);
    if (snapshot.input_ready) text += QStringLiteral("  ·  点击画面接管键鼠，F12 释放");
    if (*snapshot.plugin_notice) text += QStringLiteral("\n") + QString::fromWCharArray(snapshot.plugin_notice);
    if (status_->text() != text) status_->setText(text);

    metrics_text_.clear();
    if (playing_ && snapshot.width && snapshot.height) {
        if (snapshot.seconds > previous_seconds_ && snapshot.presented >= previous_frames_) {
            fps_ = (snapshot.presented - previous_frames_) / (snapshot.seconds - previous_seconds_);
            previous_frames_ = snapshot.presented;
            previous_seconds_ = snapshot.seconds;
        }
        const auto codec = QString::fromWCharArray(snapshot.codec);
        const auto protocol = QString::fromUtf8(snapshot.ice_protocol).toUpper();
        const auto path = route(snapshot.ice_path) + (protocol.isEmpty() ? QString{} : QStringLiteral(" / ") + protocol);
        const auto loss = snapshot.loss_fraction < 0 ? QStringLiteral("—")
            : QStringLiteral("%1%").arg(snapshot.loss_fraction * 100.0, 0, 'f', 1);
        metrics_text_ = QStringLiteral("%1 × %2  ·  %3 fps  ·  %4  ·  %5\n↓ %6  ↑ %7  ·  RTT %8  ·  丢包 %9  ·  解码等待 %10")
            .arg(snapshot.width).arg(snapshot.height).arg(fps_, 0, 'f', 1).arg(codec, path)
            .arg(bandwidth(snapshot.download_bps), bandwidth(snapshot.upload_bps), milliseconds(snapshot.rtt_ms),
                 loss, milliseconds(snapshot.decoded_frame_wait_ms));
    } else if (!playing_) {
        previous_seconds_ = fps_ = 0;
        previous_frames_ = 0;
    }
    if (metrics_->text() != metrics_text_) metrics_->setText(metrics_text_);
    metrics_->setVisible(diagnostics_->isChecked() && !metrics_text_.isEmpty());
}

void RemoteWindow::toggleFullscreen() {
    fullscreen_mode_ = !fullscreen_mode_;
    if (fullscreen_mode_) {
        was_maximized_ = isMaximized();
        layout_->removeWidget(toolbar_);
        toolbar_->setParent(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        toolbar_->setAttribute(Qt::WA_ShowWithoutActivating);
        fullscreen_->setText(QStringLiteral("退出全屏"));
        showFullScreen();
        hover_.start();
        positionFullscreenToolbar();
    } else {
        hover_.stop();
        toolbar_->hide();
        toolbar_->setParent(this, Qt::Widget);
        layout_->insertWidget(0, toolbar_);
        fullscreen_->setText(QStringLiteral("全屏"));
        toolbar_->show();
        if (was_maximized_) showMaximized();
        else showNormal();
    }
    syncVideo();
}

void RemoteWindow::positionFullscreenToolbar() {
    bool associated_popup{};
    for (auto* widget = QApplication::activePopupWidget(); widget; widget = widget->parentWidget()) {
        if (widget == toolbar_) {
            associated_popup = true;
            break;
        }
    }
    if (!isActiveWindow() && !toolbar_->isActiveWindow() && !associated_popup) {
        toolbar_->hide();
        return;
    }
    const auto top = mapToGlobal(QPoint(0, 0));
    const auto point = QCursor::pos();
    const auto inside_top = QRect(top, QSize(width(), 8)).contains(point);
    const auto inside_toolbar = toolbar_->isVisible() && toolbar_->frameGeometry().contains(point);
    if (!playing_ || inside_top || inside_toolbar || associated_popup) {
        const auto toolbar_width = std::min(width(), 960);
        toolbar_->setGeometry(top.x() + (width() - toolbar_width) / 2, top.y(),
            toolbar_width, toolbar_->sizeHint().height());
        toolbar_->show();
    } else {
        toolbar_->hide();
    }
}

void RemoteWindow::showPlugins() {
    auto* menu = new QMenu(plugins_);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto count = p2p_stream_read_menu(driver_->handle());
    for (size_t i = 0; i < count; ++i) {
        p2p_menu_item entry{};
        if (p2p_stream_menu_item(driver_->handle(), i, &entry) != P2P_OK) continue;
        auto* action = menu->addAction(QString::fromWCharArray(entry.label));
        action->setCheckable(entry.checked >= 0);
        action->setChecked(entry.checked > 0);
        connect(action, &QAction::triggered, this, [this, id = entry.id] {
            driver_->update(p2p_stream_invoke_menu(driver_->handle(), id));
        });
    }
    if (menu->isEmpty()) {
        menu->deleteLater();
        return;
    }
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(plugins_->mapToGlobal(QPoint(0, plugins_->height())));
}

void RemoteWindow::exportCsv() {
    const auto file = QFileDialog::getSaveFileName(this, QStringLiteral("导出连接诊断"),
        QStringLiteral("viewer-stream.csv"), QStringLiteral("CSV 文件 (*.csv)"));
    if (!file.isEmpty() && p2p_stream_export_csv(driver_->handle(), file.toStdWString().c_str()) != P2P_OK) {
        QMessageBox::warning(this, QStringLiteral("无法导出 CSV"), QString::fromWCharArray(p2p_last_error()));
    }
}
