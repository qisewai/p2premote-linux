#include "account_window.h"
#include "app_settings.h"
#include "app_theme.h"
#include "frameless_chrome.h"
#include "settings_dialog.h"
#include "single_instance.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QVBoxLayout>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <dwmapi.h>
#else
#include <QSysInfo>
#endif

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#if defined(Q_OS_WIN)
constexpr int stop_hotkey = 1;
#endif
constexpr int service_poll_interval_ms = 1500;
#if defined(Q_OS_WIN)
constexpr int service_status_grace_ms = 2 * service_poll_interval_ms;
#endif
constexpr int device_state_role = Qt::UserRole + 1;
constexpr int device_name_role = Qt::UserRole + 2;
constexpr int lan_port_role = Qt::UserRole + 3;
constexpr int device_platform_role = Qt::UserRole + 4;

QString winText(const wchar_t* text) { return text ? QString::fromWCharArray(text) : QString{}; }

bool setStyledProperty(QWidget* widget, const char* name, const QVariant& value) {
    if (widget->property(name) == value) return false;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
    return true;
}

QString localComputerName() {
#if defined(Q_OS_WIN)
    wchar_t buf[256]{};
    DWORD size = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &size) && size > 0) {
        return QString::fromWCharArray(buf, static_cast<qsizetype>(size));
    }
    size = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameW(buf, &size) && size > 0) {
        return QString::fromWCharArray(buf, static_cast<qsizetype>(size));
    }
#else
    const auto name = QSysInfo::machineHostName();
    if (!name.isEmpty()) return name;
#endif
    return QStringLiteral("此电脑");
}

QString generateDefaultAccessCode() {
    static constexpr char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    constexpr int count = static_cast<int>(sizeof(alphabet) - 1);
    QString code;
    code.reserve(8);
    auto* rng = QRandomGenerator::system();
    for (int i = 0; i < 8; ++i) {
        code.append(QLatin1Char(alphabet[rng->bounded(count)]));
    }
    return code;
}

QIcon makeSidebarDeviceIcon(const QString& state, bool lan_badge = false) {
    QPixmap pix(28, 18);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    const auto& theme = AppTheme::current();
    QColor dot_color(theme.text_muted);
    QColor monitor_bg = theme.is_dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 20);
    QColor monitor_fg = QColor(theme.text_muted);
    if (state == QStringLiteral("online")) {
        dot_color = QColor(0x22, 0xc5, 0x5e);
        monitor_bg = QColor(theme.accent);
        monitor_fg = QColor(Qt::white);
    } else if (state == QStringLiteral("busy")) {
        dot_color = QColor(theme.accent);
        monitor_bg = QColor(theme.accent);
        monitor_fg = QColor(Qt::white);
    }

    // Status dot
    p.setPen(Qt::NoPen);
    p.setBrush(dot_color);
    p.drawEllipse(QRectF(1.0, 6.0, 6.0, 6.0));

    // Mini monitor/window icon (UU Remote style)
    const QRectF box(10.0, 1.5, 15.0, 15.0);
    p.setBrush(monitor_bg);
    p.drawRoundedRect(box, 3.5, 3.5);

    p.setPen(Qt::NoPen);
    p.setBrush(monitor_fg);
    if (lan_badge) {
        // Clean monitor + small base inside rounded icon
        p.drawRoundedRect(QRectF(13.0, 4.5, 9.0, 6.0), 1.0, 1.0);
        p.drawRect(QRectF(15.5, 11.5, 4.0, 1.5));
    } else {
        // 4-pane Windows glyph inside rounded icon
        p.drawRect(QRectF(13.5, 5.0, 3.5, 3.5));
        p.drawRect(QRectF(18.0, 5.0, 3.5, 3.5));
        p.drawRect(QRectF(13.5, 9.5, 3.5, 3.5));
        p.drawRect(QRectF(18.0, 9.5, 3.5, 3.5));
    }
    return QIcon(pix);
}

#if defined(Q_OS_WIN)
struct Monitor {
    HMONITOR handle{};
    QSize size;
    bool primary{};
};

BOOL CALLBACK enumMonitors(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    try {
        auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(context);
        const auto& rect = info.rcMonitor;
        monitors.push_back({monitor, QSize(rect.right - rect.left, rect.bottom - rect.top),
            (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

bool interactiveDesktop() {
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    wchar_t name[32]{};
    DWORD size{};
    const bool valid = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) &&
        _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return valid;
}

QByteArray captureAgentScreen(void* monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(static_cast<HMONITOR>(monitor), &info) || !interactiveDesktop()) return {};
    DwmFlush();
    if (!interactiveDesktop()) return {};
    const auto& area = info.rcMonitor;
    const QSize source(area.right - area.left, area.bottom - area.top);
    const auto output = source.width() > 1280 || source.height() > 720
        ? source.scaled(QSize(1280, 720), Qt::KeepAspectRatio) : source;
    if (output.isEmpty()) return {};

    struct DesktopDc {
        HDC dc{GetDC(nullptr)};
        ~DesktopDc() { if (dc) ReleaseDC(nullptr, dc); }
    } desktop;
    if (!desktop.dc) return {};
    const std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)> memory(CreateCompatibleDC(desktop.dc), DeleteDC);
    if (!memory) return {};
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = output.width();
    bitmap.bmiHeader.biHeight = -output.height();
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    const std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> dib(
        CreateDIBSection(desktop.dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0), DeleteObject);
    if (!dib || !pixels) return {};
    const auto previous = SelectObject(memory.get(), dib.get());
    if (!previous || previous == HGDI_ERROR) return {};
    SetStretchBltMode(memory.get(), HALFTONE);
    SetBrushOrgEx(memory.get(), 0, 0, nullptr);
    const auto copied = StretchBlt(memory.get(), 0, 0, output.width(), output.height(), desktop.dc,
        area.left, area.top, source.width(), source.height(), SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(memory.get(), previous);
    if (!copied || !interactiveDesktop()) return {};
    auto image = QImage(static_cast<const uchar*>(pixels), output.width(), output.height(), QImage::Format_RGB32).copy();
    if (image.isNull() || !interactiveDesktop()) return {};
    for (const auto& size : {QSize(1280, 720), QSize(960, 540), QSize(640, 360), QSize(480, 270)}) {
        if (image.width() > size.width() || image.height() > size.height()) {
            image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray png;
        QBuffer buffer(&png);
        if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") && png.size() <= P2P_AGENT_IMAGE_LIMIT) {
            return png;
        }
    }
    return {};
}
#endif

} // namespace

class ToggleSwitch final : public QAbstractButton {
public:
    explicit ToggleSwitch(const QString& label = {}, QWidget* parent = nullptr)
        : QAbstractButton(parent), label_(label) {
        setCheckable(true);
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(label_.isEmpty() ? 26 : 26);
        updateWidth();
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
            hovered_ = true;
            update();
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
            hovered_ = false;
            update();
        } else if (event->type() == QEvent::FontChange) {
            updateWidth();
        }
        return QAbstractButton::event(event);
    }

    void paintEvent(QPaintEvent*) override {
        const auto& theme = AppTheme::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const bool standalone = label_.isEmpty();
        const qreal track_w = standalone ? 46.0 : 36.0;
        const qreal track_h = standalone ? 24.0 : 20.0;
        const qreal track_y = (height() - track_h) / 2.0;
        const QRectF track(width() - track_w - 1.0, track_y, track_w, track_h);

        if (!standalone) {
            QFont f = font();
            f.setPixelSize(std::max(12, theme.font_size - 1));
            f.setWeight(isChecked() ? QFont::DemiBold : QFont::Normal);
            p.setFont(f);
            p.setPen(QColor(isChecked() || hovered_ ? theme.text : theme.text_secondary));
            p.drawText(QRectF(4.0, 0.0, track.left() - 10.0, height()),
                Qt::AlignVCenter | Qt::AlignRight, label_);
        }

        QColor track_color = isChecked()
            ? QColor(hovered_ ? theme.accent_hover : theme.accent)
            : (theme.is_dark ? QColor(255, 255, 255, hovered_ ? 64 : 42) : QColor(0, 0, 0, hovered_ ? 64 : 44));
        p.setPen(Qt::NoPen);
        p.setBrush(track_color);
        p.drawRoundedRect(track, track_h / 2.0, track_h / 2.0);

        const qreal pad = standalone ? 3.0 : 2.5;
        const qreal knob = track_h - pad * 2.0;
        const qreal kx = isChecked() ? (track.right() - knob - pad) : (track.left() + pad);
        const qreal ky = track.top() + pad;
        p.setBrush(QColor(Qt::white));
        p.drawEllipse(QRectF(kx, ky, knob, knob));
    }

private:
    void updateWidth() {
        if (label_.isEmpty()) {
            setFixedWidth(48);
        } else {
            const int text_w = fontMetrics().horizontalAdvance(label_);
            setFixedWidth(text_w + 52);
        }
    }

    QString label_;
    bool hovered_{};
};

class DeviceHeroWidget final : public QWidget {
public:
    explicit DeviceHeroWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(216);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(10);
        layout->addStretch(2);
        layout->addSpacing(58); // room for painted center icon

        subtitle_ = new QLabel(this);
        subtitle_->setAlignment(Qt::AlignCenter);
        subtitle_->setWordWrap(true);
        subtitle_->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.86); font-size: 13px; background: transparent;"));
        layout->addWidget(subtitle_);

        action_ = new QPushButton(QStringLiteral("开始远程控制"), this);
        action_->setObjectName(QStringLiteral("primary"));
        action_->setCursor(Qt::PointingHandCursor);
        action_->setMinimumWidth(148);
        action_->setMinimumHeight(34);
        layout->addWidget(action_, 0, Qt::AlignHCenter);
        layout->addStretch(1);
    }

    QPushButton* actionButton() const noexcept { return action_; }

    void setContent(const QString& subtitle, const QString& button_text, bool button_enabled, bool online_style) {
        subtitle_->setText(subtitle);
        action_->setText(button_text);
        action_->setEnabled(button_enabled);
        online_style_ = online_style;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const auto& theme = AppTheme::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath clip;
        clip.moveTo(0, 11);
        clip.arcTo(QRectF(0, 0, 22, 22), 180, -90);
        clip.lineTo(width() - 11, 0);
        clip.arcTo(QRectF(width() - 22, 0, 22, 22), 90, -90);
        clip.lineTo(width(), height());
        clip.lineTo(0, height());
        clip.closeSubpath();
        p.setClipPath(clip);

        QLinearGradient grad(0, 0, width(), height());
        if (online_style_) {
            const QColor accent(theme.accent);
            grad.setColorAt(0.0, accent.darker(155));
            grad.setColorAt(0.55, accent.darker(195));
            grad.setColorAt(1.0, QColor(0x0b, 0x20, 0x42));
        } else {
            grad.setColorAt(0.0, theme.is_dark ? QColor(0x1e, 0x26, 0x36) : QColor(0x47, 0x55, 0x69));
            grad.setColorAt(1.0, theme.is_dark ? QColor(0x13, 0x18, 0x22) : QColor(0x33, 0x41, 0x55));
        }
        p.fillRect(rect(), grad);

        // Subtle atmospheric Windows 4-pane watermark on right (UU Remote inspired)
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 16));
        const qreal wx = width() * 0.68;
        const qreal wy = height() * 0.28;
        p.drawRoundedRect(QRectF(wx, wy, 48, 42), 4, 4);
        p.drawRoundedRect(QRectF(wx + 54, wy - 4, 58, 46), 4, 4);
        p.drawRoundedRect(QRectF(wx, wy + 48, 48, 44), 4, 4);
        p.drawRoundedRect(QRectF(wx + 54, wy + 48, 58, 48), 4, 4);
        p.restore();

        // Centered remote monitor + signal arcs icon
        const qreal cx = width() / 2.0;
        const qreal cy = height() * 0.34;
        QPen icon_pen(QColor(255, 255, 255, online_style_ ? 235 : 150), 2.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(icon_pen);
        p.setBrush(Qt::NoBrush);
        // Monitor frame
        p.drawRoundedRect(QRectF(cx - 24, cy - 10, 48, 30), 6, 6);
        // Stand
        p.drawLine(QPointF(cx - 10, cy + 25), QPointF(cx + 10, cy + 25));
        // Signal waves above monitor
        p.drawArc(QRectF(cx - 11, cy - 22, 22, 16), 35 * 16, 110 * 16);
        p.drawArc(QRectF(cx - 18, cy - 29, 36, 24), 40 * 16, 100 * 16);
    }

private:
    QLabel* subtitle_{};
    QPushButton* action_{};
    bool online_style_{true};
};

#if defined(Q_OS_WIN)
class HostSession final {
public:
    using Callback = std::function<void(int state, const QString& status, bool desktop_control)>;
    using AgentCallback = std::function<void(std::uint32_t id, int tool)>;

    HostSession(p2p_account* account, void* monitor, LaunchOptions options, Callback on_update,
        AgentCallback on_agent, QString* error)
        : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), on_update_(std::move(on_update)), on_agent_(std::move(on_agent)) {
        init(account, std::nullopt, monitor, std::move(options), error);
    }

    HostSession(const LanConnectOptions& lan, void* monitor, LaunchOptions options, Callback on_update, QString* error)
        : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), on_update_(std::move(on_update)) {
        init(nullptr, lan, monitor, std::move(options), error);
    }

    ~HostSession() {
        stop();
        if (wake_) CloseHandle(wake_);
    }

    bool valid() const { return started_; }
    bool stopped() const { return stopped_.load(std::memory_order_acquire); }

    void displayChanged() {
        display_changed_.store(true, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

    void answerAgent(std::uint32_t id, bool approved, QByteArray png = {}) {
        {
            const std::lock_guard lock(agent_mutex_);
            agent_answer_ = AgentAnswer{id, approved, std::move(png)};
        }
        if (wake_) SetEvent(wake_);
    }

    void revokeAgent(bool temporary_block) {
        agent_revoke_.store(temporary_block ? 2 : 1, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

private:
    struct AgentAnswer {
        std::uint32_t id;
        bool approved;
        QByteArray png;
    };

    void init(p2p_account* account, std::optional<LanConnectOptions> lan, void* monitor, LaunchOptions options,
        QString* error) {
        if (!wake_) {
            if (error) *error = QStringLiteral("无法创建接收线程唤醒事件。");
            return;
        }
        std::promise<QString> started;
        auto ready = started.get_future();
        thread_ = std::jthread([this, account, lan = std::move(lan), monitor, options = std::move(options),
            started = std::move(started)](std::stop_token stop) mutable {
            run(stop, account, lan, monitor, options, std::move(started));
        });
        const auto startup_error = ready.get();
        if (!startup_error.isEmpty()) {
            if (error) *error = startup_error;
            stop();
            return;
        }
        started_ = true;
    }

    void stop() {
        if (thread_.joinable()) {
            thread_.request_stop();
            if (wake_) SetEvent(wake_);
            thread_.join();
        }
    }

    void run(std::stop_token stop, p2p_account* account, const std::optional<LanConnectOptions>& lan,
        void* monitor, const LaunchOptions& options, std::promise<QString> started) {
        std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream(nullptr, p2p_stream_destroy);
        if (lan) {
            const p2p_lan_options_v1 lan_settings{
                lan->host.empty() ? nullptr : lan->host.c_str(), lan->port, lan->access_code.c_str(), 1,
                options.ice_trace.c_str(), options.latency_trace.c_str()};
            stream.reset(p2p_stream_create_lan(1, nullptr, &lan_settings));
        } else {
            const p2p_connection_options_v1 settings{
                options.ice_trace.c_str(), options.latency_trace.c_str(), options.stun.c_str(), options.stun_only, 1, 1};
            stream.reset(p2p_stream_create(account, 1, nullptr, nullptr, &settings));
        }
        if (!stream) {
            started.set_value(winText(p2p_last_error()));
            return;
        }
        if (p2p_stream_start(stream.get(), monitor, 60) != P2P_OK) {
            started.set_value(winText(p2p_last_error()));
            return;
        }
        started.set_value(QString{});

        int last_state = -1;
        bool last_desktop = false;
        bool input_ready = false;
        std::uint32_t agent_approval{};
        std::uint32_t input_blocked_for{};
        QString last_status;
        auto publish = [&](bool force) {
            p2p_stream_snapshot_v1 snapshot{};
            if (p2p_stream_snapshot(stream.get(), &snapshot) != P2P_OK) {
                const auto error = winText(p2p_last_error());
                const auto reason = error.toStdWString();
                p2p_stream_stop(stream.get(), reason.c_str());
                stopped_.store(true, std::memory_order_release);
                if (on_update_) on_update_(P2P_STOPPED, error, false);
                return false;
            }
            input_ready = snapshot.input_ready != 0;
            const auto status = winText(snapshot.status);
            const bool desktop = snapshot.desktop_control != 0;
            if (snapshot.state == P2P_STOPPED) stopped_.store(true, std::memory_order_release);
            if (force || snapshot.state != last_state || desktop != last_desktop || status != last_status) {
                last_state = snapshot.state;
                last_desktop = desktop;
                last_status = status;
                if (on_update_) on_update_(snapshot.state, status, desktop);
            }
            return snapshot.state != P2P_STOPPED;
        };

        auto fail = [&]() {
            const auto error = winText(p2p_last_error());
            const auto reason = error.toStdWString();
            p2p_stream_stop(stream.get(), reason.c_str());
            publish(true);
        };

        if (!publish(true)) return;

        constexpr ULONGLONG tick_interval_ms = 1000;
        constexpr ULONGLONG cursor_interval_ms = 33;
        auto now = GetTickCount64();
        auto next_tick = now + tick_interval_ms;
        auto next_cursor = now + cursor_interval_ms;

        while (!stop.stop_requested()) {
            HANDLE handles[3]{wake_};
            uint32_t kinds[3]{0};
            DWORD count = 1;
            if (const auto event = p2p_stream_wait_handle(stream.get(), P2P_WAIT_SESSION)) {
                handles[count] = event;
                kinds[count++] = P2P_WAIT_SESSION;
            }
            if (const auto capture = p2p_stream_wait_handle(stream.get(), P2P_WAIT_CAPTURE)) {
                handles[count] = capture;
                kinds[count++] = P2P_WAIT_CAPTURE;
            }

            now = GetTickCount64();
            ULONGLONG wait_ms = next_tick > now ? (next_tick - now) : 0;
            if (input_ready) {
                const ULONGLONG cursor_wait = next_cursor > now ? (next_cursor - now) : 0;
                wait_ms = std::min(wait_ms, cursor_wait);
            }

            const DWORD waited = MsgWaitForMultipleObjectsEx(count, handles, static_cast<DWORD>(wait_ms),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (stop.stop_requested()) break;

            bool check_snapshot = false;
            if (display_changed_.exchange(false, std::memory_order_acq_rel)) {
                if (p2p_stream_display_changed(stream.get()) != P2P_OK) {
                    fail();
                    break;
                }
                check_snapshot = true;
            }

            if (const auto revoke = agent_revoke_.exchange(0, std::memory_order_acq_rel)) {
                p2p_stream_revoke_agent_approval(stream.get(), revoke == 2);
                check_snapshot = true;
            }
            std::optional<AgentAnswer> answer;
            {
                const std::lock_guard lock(agent_mutex_);
                answer = std::exchange(agent_answer_, {});
            }
            if (answer) {
                const auto id = answer->id;
                p2p_stream_decide_agent_approval(stream.get(), id, answer->approved, answer->png.constData(),
                    static_cast<std::size_t>(answer->png.size()));
                if (input_blocked_for == id) {
                    if (p2p_stream_block_agent_prompt_input(stream.get(), 0) != P2P_OK) {
                        fail();
                        break;
                    }
                    input_blocked_for = 0;
                }
                check_snapshot = true;
            }

            if (waited >= WAIT_OBJECT_0 + 1 && waited < WAIT_OBJECT_0 + count) {
                const auto kind = kinds[waited - WAIT_OBJECT_0];
                if (p2p_stream_dispatch(stream.get(), kind) != P2P_OK) {
                    fail();
                    break;
                }
                if (kind == P2P_WAIT_SESSION) check_snapshot = true;
            }

            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (stop.stop_requested()) break;

            now = GetTickCount64();
            if (now >= next_tick) {
                next_tick = now + tick_interval_ms;
                if (p2p_stream_tick(stream.get()) != P2P_OK) {
                    fail();
                    break;
                }
                check_snapshot = true;
            }
            if (input_ready && now >= next_cursor) {
                next_cursor = now + cursor_interval_ms;
                if (p2p_stream_poll_cursor(stream.get()) != P2P_OK) {
                    fail();
                    break;
                }
            }

            if (check_snapshot || p2p_stream_state(stream.get()) != last_state
                || (p2p_stream_input_ready(stream.get()) != 0) != input_ready) {
                if (!publish(false)) break;
            }
            if (check_snapshot) {
                int tool{};
                const auto next = p2p_stream_pending_agent_approval(stream.get(), &tool);
                if (next != agent_approval) {
                    if (next) {
                        if (p2p_stream_block_agent_prompt_input(stream.get(), 1) != P2P_OK) {
                            fail();
                            break;
                        }
                        input_blocked_for = next;
                    }
                    agent_approval = next;
                    if (on_agent_) on_agent_(next, tool);
                }
            }
        }
    }

    HANDLE wake_{};
    Callback on_update_;
    AgentCallback on_agent_;
    std::mutex agent_mutex_;
    std::optional<AgentAnswer> agent_answer_;
    std::atomic<int> agent_revoke_{};
    std::atomic<bool> display_changed_{false};
    std::atomic<bool> stopped_{false};
    bool started_{false};
    std::jthread thread_;
};
#else
class HostSession final {};
#endif

AccountWindow::AccountWindow(LaunchOptions options)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), options_(std::move(options)) {
    account_.reset(p2p_account_create());
    setWindowTitle(QStringLiteral("P2P Remote"));
    resize(960, 640);
    setMinimumSize(820, 540);

    lan_host_code_ = readLanSecret(QStringLiteral("HostAccessCode"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* shell = new QFrame(this);
    shell->setObjectName(QStringLiteral("framelessShell"));
    auto* shell_layout = new QVBoxLayout(shell);
    shell_layout->setContentsMargins(1, 1, 1, 1);
    shell_layout->setSpacing(0);

    // Top TitleBar with Account dropdown button
    title_bar_ = new TitleBar(this, QStringLiteral("P2P Remote"), true, true, shell);

    account_btn_ = new QPushButton(QStringLiteral("登录账号 ▾"), title_bar_);
    account_btn_->setObjectName(QStringLiteral("titleToolBtn"));
    account_btn_->setFocusPolicy(Qt::NoFocus);
    connect(account_btn_, &QPushButton::clicked, this, [this] {
        if (p2p_account_signed_in(account_.get())) {
            showAccountMenu();
            return;
        }
        if (login_popup_ && login_popup_->isVisible()) {
            login_popup_->hide();
            return;
        }
        if (login_popup_closed_.isValid() && login_popup_closed_.elapsed() < 200) return;
        showLoginPopup();
    });
    title_bar_->addToolWidget(account_btn_);
    shell_layout->addWidget(title_bar_);

    // Main Master-Detail split (UU Remote style)
    auto* workspace = new QWidget(shell);
    auto* split = new QHBoxLayout(workspace);
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);

    split->addWidget(makeSidebar(workspace));

    auto* right_pane = new QWidget(workspace);
    auto* right_layout = new QVBoxLayout(right_pane);
    right_layout->setContentsMargins(28, 22, 28, 16);
    right_layout->setSpacing(14);

    right_stack_ = new QStackedWidget(right_pane);
    local_page_ = makeLocalPage(right_stack_);
    detail_page_ = makeDetailPage(right_stack_);
    right_stack_->addWidget(local_page_);
    right_stack_->addWidget(detail_page_);
    right_stack_->setCurrentWidget(local_page_);
    right_layout->addWidget(right_stack_, 1);
    makeLoginPopup();

    status_ = new QLabel(right_pane);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->hide();
    right_layout->addWidget(status_);

    split->addWidget(right_pane, 1);
    shell_layout->addWidget(workspace, 1);
    outer->addWidget(shell);

    if (!account_) {
        setStatus(winText(p2p_last_error()));
        return;
    }

#if defined(Q_OS_WIN)
    account_event_.setHandle(p2p_account_event(account_.get()));
    account_event_.setEnabled(true);
    connect(&account_event_, &QWinEventNotifier::activated, this, &AccountWindow::completeRequest);
#else
    account_event_.setInterval(50);
    connect(&account_event_, &QTimer::timeout, this, &AccountWindow::completeRequest);
#endif
    refresh_.setInterval(3000);
    connect(&refresh_, &QTimer::timeout, this, &AccountWindow::refresh);

    status_clear_timer_.setSingleShot(true);
    status_clear_timer_.setInterval(5000);
    connect(&status_clear_timer_, &QTimer::timeout, this, [this] {
        if (status_) status_->hide();
    });

    connect(&AppSettings::instance(), &AppSettings::changed, this, [this](const AppSettingsData&) {
        fillDevices();
        if (local_nav_btn_ && local_status_pill_) {
            local_nav_btn_->setIcon(makeSidebarDeviceIcon(local_status_pill_->property("state").toString()));
        }
        if (lan_allow_switch_) lan_allow_switch_->update();
        if (hero_widget_) hero_widget_->update();
    });

    makeTray();
#if defined(Q_OS_WIN)
    elevated_ = isProcessElevated();
#endif
    refreshServiceStatus();
    connect(&service_refresh_, &QTimer::timeout, this, &AccountWindow::refreshServiceStatus);
    service_refresh_.start(service_poll_interval_ms);
#if defined(Q_OS_WIN)
    refreshScreens();
#endif
    loadCredentials();
    fillDevices();
    updateControls();
}

AccountWindow::~AccountWindow() {
    service_refresh_.stop();
    if (service_status_thread_.joinable()) service_status_thread_.join();
#if defined(Q_OS_WIN)
    account_event_.setEnabled(false);
#else
    account_event_.stop();
#endif
    closeSessions();
}

QWidget* AccountWindow::makeSidebar(QWidget* parent) {
    auto* sidebar = new QFrame(parent);
    sidebar->setObjectName(QStringLiteral("navSidebar"));
    sidebar->setFixedWidth(236);

    auto* root = new QVBoxLayout(sidebar);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* nav_body = new QWidget(sidebar);
    auto* nav = new QVBoxLayout(nav_body);
    nav->setContentsMargins(10, 12, 10, 10);
    nav->setSpacing(6);

    // Standalone top navigation item: 本机 (independent of cloud login)
    local_nav_btn_ = new QPushButton(QStringLiteral("本机"), nav_body);
    local_nav_btn_->setObjectName(QStringLiteral("sidebarNavItem"));
    local_nav_btn_->setCheckable(true);
    local_nav_btn_->setChecked(true);
    local_nav_btn_->setFocusPolicy(Qt::NoFocus);
    local_nav_btn_->setIconSize(QSize(28, 18));
    local_nav_btn_->setIcon(makeSidebarDeviceIcon(QStringLiteral("offline"), false));
    local_nav_btn_->setToolTip(localComputerName());
    connect(local_nav_btn_, &QPushButton::clicked, this, [this] {
        viewing_local_ = true;
        selected_ = {};
        if (local_nav_btn_) local_nav_btn_->setChecked(true);
        if (cloud_list_) {
            const QSignalBlocker b(cloud_list_);
            cloud_list_->clearSelection();
            cloud_list_->setCurrentRow(-1);
        }
        right_stack_->setCurrentWidget(local_page_);
        updateControls();
    });
    nav->addWidget(local_nav_btn_);

    auto* nav_div = new QFrame(nav_body);
    nav_div->setObjectName(QStringLiteral("divider"));
    nav->addSpacing(2);
    nav->addWidget(nav_div);
    nav->addSpacing(2);

    // Cloud account section: 我的设备 (strictly cloud-bound devices after sign-in)
    auto* cloud_hdr = new QHBoxLayout;
    cloud_hdr->setContentsMargins(0, 0, 0, 0);
    auto* cloud_lbl = new QLabel(QStringLiteral("我的设备"), nav_body);
    cloud_lbl->setObjectName(QStringLiteral("sidebarSection"));
    cloud_hdr->addWidget(cloud_lbl, 1);
    cloud_refresh_btn_ = new QPushButton(QStringLiteral("↻"), nav_body);
    cloud_refresh_btn_->setObjectName(QStringLiteral("sidebarIconBtn"));
    cloud_refresh_btn_->setToolTip(QStringLiteral("刷新账号设备列表"));
    cloud_refresh_btn_->setFocusPolicy(Qt::NoFocus);
    connect(cloud_refresh_btn_, &QPushButton::clicked, this, &AccountWindow::refresh);
    cloud_hdr->addWidget(cloud_refresh_btn_);
    nav->addLayout(cloud_hdr);

    cloud_list_ = new QListWidget(nav_body);
    cloud_list_->setObjectName(QStringLiteral("sidebarList"));
    cloud_list_->setIconSize(QSize(28, 18));
    cloud_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav->addWidget(cloud_list_, 1);

    // Second Section: 远程直连 (LAN discovery + direct connect in dedicated dialog)
    auto* direct_lbl = new QLabel(QStringLiteral("远程直连"), nav_body);
    direct_lbl->setObjectName(QStringLiteral("sidebarSection"));
    nav->addSpacing(6);
    nav->addWidget(direct_lbl);

    auto* direct_btn = new QPushButton(QStringLiteral("＋  局域网直连…"), nav_body);
    direct_btn->setObjectName(QStringLiteral("sidebarActionBtn"));
    direct_btn->setFocusPolicy(Qt::NoFocus);
    connect(direct_btn, &QPushButton::clicked, this, &AccountWindow::openLanConnectDialog);
    nav->addWidget(direct_btn);

#if defined(Q_OS_WIN)
    auto* audit_btn = new QPushButton(QStringLiteral("AI Agent 诊断记录"), nav_body);
    audit_btn->setObjectName(QStringLiteral("sidebarActionBtn"));
    audit_btn->setFocusPolicy(Qt::NoFocus);
    connect(audit_btn, &QPushButton::clicked, this, &AccountWindow::openAgentAudit);
    nav->addWidget(audit_btn);
#endif

    root->addWidget(nav_body, 1);

    // Bottom pinned footer: 设置 (UU Remote style)
    auto* footer = new QFrame(sidebar);
    footer->setObjectName(QStringLiteral("navFooter"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(10, 8, 10, 8);
    auto* settings_btn = new QPushButton(QStringLiteral("⚙  设置"), footer);
    settings_btn->setObjectName(QStringLiteral("sidebarFooterBtn"));
    settings_btn->setFocusPolicy(Qt::NoFocus);
    connect(settings_btn, &QPushButton::clicked, this, &AccountWindow::openSettings);
    footer_layout->addWidget(settings_btn);
    root->addWidget(footer);

    connect(cloud_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        const auto id = item->data(Qt::UserRole).toString();
        const auto local = winText(p2p_account_device_id(account_.get()));
        const bool is_self = !id.isEmpty() &&
            (id == local || (service_info_.running && service_info_.cloud_enabled && id == service_info_.device_id));
        viewing_local_ = false;
        if (local_nav_btn_) local_nav_btn_->setChecked(false);
        selected_ = {
            id,
            item->data(device_name_role).toString(),
            item->data(device_state_role).toString(),
            item->data(device_platform_role).toString(),
            is_self
        };
        right_stack_->setCurrentWidget(detail_page_);
        updateControls();
    });
    connect(cloud_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        connectSelectedDevice(false);
    });

    return sidebar;
}

QWidget* AccountWindow::makeLocalPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);

    // Top Header: Local computer name + 本机 tag + live status pill + quick connect CTA
    auto* hdr = new QHBoxLayout;
    hdr->setSpacing(10);
    local_title_ = new QLabel(localComputerName(), page);
    local_title_->setObjectName(QStringLiteral("deviceTitle"));
    local_title_->setTextFormat(Qt::PlainText);
    hdr->addWidget(local_title_);

    auto* self_tag = new QLabel(QStringLiteral("本机"), page);
    self_tag->setObjectName(QStringLiteral("selfTag"));
    hdr->addWidget(self_tag);

    local_status_pill_ = new QLabel(QStringLiteral("● 未开启被控"), page);
    local_status_pill_->setObjectName(QStringLiteral("statusPill"));
    local_status_pill_->setProperty("state", QStringLiteral("offline"));
    hdr->addWidget(local_status_pill_);

    hdr->addStretch(1);

    auto* quick_lan_btn = new QPushButton(QStringLiteral("＋  局域网直连"), page);
    quick_lan_btn->setObjectName(QStringLiteral("primary"));
    quick_lan_btn->setCursor(Qt::PointingHandCursor);
    quick_lan_btn->setFocusPolicy(Qt::NoFocus);
    quick_lan_btn->setMinimumHeight(32);
    connect(quick_lan_btn, &QPushButton::clicked, this, &AccountWindow::openLanConnectDialog);
    hdr->addWidget(quick_lan_btn);
    layout->addLayout(hdr);

    // Card 1: 允许局域网连接 + IP & Large Verification Code
    local_lan_card_ = new QFrame(page);
    local_lan_card_->setObjectName(QStringLiteral("localLanCard"));
    auto* lan_vbox = new QVBoxLayout(local_lan_card_);
    lan_vbox->setContentsMargins(22, 20, 22, 20);
    lan_vbox->setSpacing(16);

    auto* lan_hdr = new QHBoxLayout;
    lan_hdr->setSpacing(16);
    auto* lan_title_col = new QVBoxLayout;
    lan_title_col->setContentsMargins(0, 0, 0, 0);
    lan_title_col->setSpacing(4);
    auto* lan_title = new QLabel(QStringLiteral("允许局域网连接"), local_lan_card_);
    lan_title->setObjectName(QStringLiteral("cardTitle"));
    lan_title_col->addWidget(lan_title);
    local_lan_desc_ = new QLabel(
        QStringLiteral("开启后，同网段设备可通过本机 IP 与连接验证码直接控制此电脑"), local_lan_card_);
    local_lan_desc_->setObjectName(QStringLiteral("muted"));
    local_lan_desc_->setWordWrap(true);
    lan_title_col->addWidget(local_lan_desc_);
    lan_hdr->addLayout(lan_title_col, 1);

    lan_allow_switch_ = new ToggleSwitch(QString{}, local_lan_card_);
    lan_allow_switch_->setToolTip(QStringLiteral("允许同网段设备通过 IP 与连接验证码直连本机"));
    connect(lan_allow_switch_, &QAbstractButton::toggled, this, &AccountWindow::setLanAllowEnabled);
    lan_hdr->addWidget(lan_allow_switch_, 0, Qt::AlignVCenter);
    lan_vbox->addLayout(lan_hdr);

    auto* lan_div = new QFrame(local_lan_card_);
    lan_div->setObjectName(QStringLiteral("divider"));
    lan_vbox->addWidget(lan_div);

    // Active LAN info & verification code area (generous 2-tile layout)
    lan_info_area_ = new QWidget(local_lan_card_);
    auto* info_vbox = new QVBoxLayout(lan_info_area_);
    info_vbox->setContentsMargins(0, 0, 0, 0);
    info_vbox->setSpacing(14);

    auto* tiles = new QHBoxLayout;
    tiles->setSpacing(14);

    auto* ip_tile = new QFrame(lan_info_area_);
    ip_tile->setObjectName(QStringLiteral("metricTile"));
    auto* ip_vbox = new QVBoxLayout(ip_tile);
    ip_vbox->setContentsMargins(18, 14, 18, 14);
    ip_vbox->setSpacing(6);
    auto* ip_caption = new QLabel(QStringLiteral("本机局域网 IP"), ip_tile);
    ip_caption->setObjectName(QStringLiteral("metricLabel"));
    ip_vbox->addWidget(ip_caption);
    local_ip_value_ = new QLabel(QStringLiteral("—"), ip_tile);
    local_ip_value_->setObjectName(QStringLiteral("ipDisplay"));
    local_ip_value_->setTextFormat(Qt::PlainText);
    local_ip_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ip_vbox->addWidget(local_ip_value_);
    local_ip_extra_ = new QLabel(QStringLiteral("同网段设备可自动发现或手动输入此地址"), ip_tile);
    local_ip_extra_->setObjectName(QStringLiteral("muted"));
    local_ip_extra_->setTextFormat(Qt::PlainText);
    local_ip_extra_->setWordWrap(true);
    ip_vbox->addWidget(local_ip_extra_);
    tiles->addWidget(ip_tile, 1);

    auto* code_tile = new QFrame(lan_info_area_);
    code_tile->setObjectName(QStringLiteral("metricTile"));
    auto* code_vbox = new QVBoxLayout(code_tile);
    code_vbox->setContentsMargins(18, 14, 18, 14);
    code_vbox->setSpacing(6);
    auto* code_caption = new QLabel(QStringLiteral("连接验证码"), code_tile);
    code_caption->setObjectName(QStringLiteral("metricLabel"));
    code_vbox->addWidget(code_caption);
    local_code_value_ = new QLabel(QStringLiteral("••••••••"), code_tile);
    local_code_value_->setObjectName(QStringLiteral("codeDisplay"));
    local_code_value_->setTextFormat(Qt::PlainText);
    local_code_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    code_vbox->addWidget(local_code_value_);
    auto* code_sub = new QLabel(QStringLiteral("固定连接验证码 · 校验通过后即可远控"), code_tile);
    code_sub->setObjectName(QStringLiteral("muted"));
    code_vbox->addWidget(code_sub);
    tiles->addWidget(code_tile, 1);

    info_vbox->addLayout(tiles);

    auto* lan_actions = new QHBoxLayout;
    lan_actions->setSpacing(10);
    lan_toggle_code_btn_ = new QPushButton(QStringLiteral("显示验证码"), lan_info_area_);
    lan_toggle_code_btn_->setFocusPolicy(Qt::NoFocus);
    lan_toggle_code_btn_->setMinimumHeight(30);
    connect(lan_toggle_code_btn_, &QPushButton::clicked, this, [this] {
        lan_code_visible_ = !lan_code_visible_;
        updateLocalPage();
    });
    lan_actions->addWidget(lan_toggle_code_btn_);

    lan_copy_btn_ = new QPushButton(QStringLiteral("复制连接信息"), lan_info_area_);
    lan_copy_btn_->setFocusPolicy(Qt::NoFocus);
    lan_copy_btn_->setMinimumHeight(30);
    connect(lan_copy_btn_, &QPushButton::clicked, this, &AccountWindow::copyLanConnectionInfo);
    lan_actions->addWidget(lan_copy_btn_);

    lan_edit_btn_ = new QPushButton(QStringLiteral("修改验证码"), lan_info_area_);
    lan_edit_btn_->setFocusPolicy(Qt::NoFocus);
    lan_edit_btn_->setMinimumHeight(30);
    connect(lan_edit_btn_, &QPushButton::clicked, this, &AccountWindow::editLanAccessCode);
    lan_actions->addWidget(lan_edit_btn_);

    lan_actions->addStretch(1);

#if defined(Q_OS_WIN)
    screens_ = new QComboBox(lan_info_area_);
    screens_->setAccessibleName(QStringLiteral("共享显示器"));
    screens_->setMinimumWidth(168);
    connect(screens_, qOverload<int>(&QComboBox::activated), this, [this](int) {
        if (service_info_.running && (service_info_.lan_enabled || service_info_.cloud_enabled)) {
            QString error;
            if (!installOrUpdateHostService(service_info_.lan_enabled, service_info_.cloud_enabled, &error)) {
                setStatus(error);
            } else {
                setStatus(QStringLiteral("系统服务已切换共享显示器。"));
            }
        } else if (lan_host_) {
            stopLanReceiving();
            startLanReceiving(lan_host_code_, lan_host_port_);
        }
        updateLocalPage();
    });
    lan_actions->addWidget(screens_);
#endif
    info_vbox->addLayout(lan_actions);
    lan_vbox->addWidget(lan_info_area_);

    lan_off_hint_ = new QWidget(local_lan_card_);
    auto* off_row = new QHBoxLayout(lan_off_hint_);
    off_row->setContentsMargins(0, 2, 0, 2);
    auto* off_label = new QLabel(
#if defined(Q_OS_WIN)
        QStringLiteral("当前未开启局域网连接。开启上方开关后，将显示本机 IP 与连接验证码；该功能与云端账号相互独立，可同时使用。"),
#else
        QStringLiteral("开启后随系统启动，支持在 Xorg 登录界面和桌面接收局域网远控。"),
#endif
        lan_off_hint_);
    off_label->setObjectName(QStringLiteral("muted"));
    off_label->setWordWrap(true);
    off_row->addWidget(off_label, 1);
    lan_vbox->addWidget(lan_off_hint_);

    layout->addWidget(local_lan_card_);

    // Card 2: 本机信息与云端连接状态
    auto* info_card = new QFrame(page);
    info_card->setObjectName(QStringLiteral("accountCard"));
    auto* info_card_vbox = new QVBoxLayout(info_card);
    info_card_vbox->setContentsMargins(22, 18, 22, 18);
    info_card_vbox->setSpacing(14);

    auto* info_hdr = new QHBoxLayout;
    auto* info_title = new QLabel(QStringLiteral("本机信息与云端状态"), info_card);
    info_title->setObjectName(QStringLiteral("cardTitle"));
    info_hdr->addWidget(info_title, 1);
#if defined(Q_OS_WIN)
    auto* audit_link = new QPushButton(QStringLiteral("AI Agent 诊断记录"), info_card);
    audit_link->setObjectName(QStringLiteral("miniGhostBtn"));
    audit_link->setFocusPolicy(Qt::NoFocus);
    connect(audit_link, &QPushButton::clicked, this, &AccountWindow::openAgentAudit);
    info_hdr->addWidget(audit_link);
#endif
    info_card_vbox->addLayout(info_hdr);

    auto makeSummaryCol = [&](const QString& label_text, QLabel*& value_out) {
        auto* col = new QVBoxLayout;
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(4);
        auto* lbl = new QLabel(label_text, info_card);
        lbl->setObjectName(QStringLiteral("metricLabel"));
        col->addWidget(lbl);
        value_out = new QLabel(QStringLiteral("—"), info_card);
        value_out->setObjectName(QStringLiteral("summaryValue"));
        value_out->setTextFormat(Qt::PlainText);
        value_out->setWordWrap(true);
        col->addWidget(value_out);
        return col;
    };

    auto* summary_row = new QHBoxLayout;
    summary_row->setSpacing(20);
    summary_row->addLayout(makeSummaryCol(QStringLiteral("局域网 IP"), local_summary_ip_), 1);
    summary_row->addLayout(makeSummaryCol(QStringLiteral("当前显示器"), local_summary_display_), 1);
#if defined(Q_OS_WIN)
    summary_row->addLayout(makeSummaryCol(QStringLiteral("运行权限"), privilege_status_), 1);
#else
    summary_row->addLayout(makeSummaryCol(QStringLiteral("云端账号"), local_summary_account_), 1);
#endif
    local_summary_ip_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_card_vbox->addLayout(summary_row);

    auto* info_div = new QFrame(info_card);
    info_div->setObjectName(QStringLiteral("divider"));
    info_card_vbox->addWidget(info_div);

#if defined(Q_OS_WIN)
    service_section_ = new QWidget(info_card);
    auto* svc_row = new QHBoxLayout(service_section_);
    svc_row->setContentsMargins(0, 0, 0, 0);
    svc_row->setSpacing(10);
    service_hint_ = new QLabel(service_section_);
    service_hint_->setObjectName(QStringLiteral("muted"));
    service_hint_->setTextFormat(Qt::PlainText);
    service_hint_->setWordWrap(true);
    svc_row->addWidget(service_hint_, 1);
    elevate_button_ = new QPushButton(QStringLiteral("管理员重启"), service_section_);
    elevate_button_->setFocusPolicy(Qt::NoFocus);
    connect(elevate_button_, &QPushButton::clicked, this, &AccountWindow::relaunchAsAdmin);
    svc_row->addWidget(elevate_button_);
    service_button_ = new QPushButton(QStringLiteral("启用系统服务"), service_section_);
    service_button_->setFocusPolicy(Qt::NoFocus);
    connect(service_button_, &QPushButton::clicked, this, &AccountWindow::toggleHostService);
    svc_row->addWidget(service_button_);
    info_card_vbox->addWidget(service_section_);
#endif

    cloud_host_section_ = new QWidget(info_card);
    auto* ch_row = new QHBoxLayout(cloud_host_section_);
    ch_row->setContentsMargins(0, 0, 0, 0);
    ch_row->setSpacing(10);
    cloud_host_status_ = new QLabel(cloud_host_section_);
    cloud_host_status_->setObjectName(QStringLiteral("muted"));
    cloud_host_status_->setTextFormat(Qt::PlainText);
    cloud_host_status_->setWordWrap(true);
    ch_row->addWidget(cloud_host_status_, 1);
    add_device_ = new QPushButton(QStringLiteral("添加到云端"), cloud_host_section_);
    add_device_->setFocusPolicy(Qt::NoFocus);
    connect(add_device_, &QPushButton::clicked, this, &AccountWindow::addLocalDevice);
    ch_row->addWidget(add_device_);
    receive_ = new QPushButton(QStringLiteral("开启云端接收"), cloud_host_section_);
    receive_->setFocusPolicy(Qt::NoFocus);
    connect(receive_, &QPushButton::clicked, this, &AccountWindow::toggleReceiving);
    ch_row->addWidget(receive_);
    info_card_vbox->addWidget(cloud_host_section_);
    cloud_guest_hint_ = new QLabel(
        QStringLiteral("云端跨网访问：未登录账号 · 点击右上角「登录账号」可跨外网管理个人设备，与局域网直连互不冲突。"),
        info_card);
    cloud_guest_hint_->setObjectName(QStringLiteral("muted"));
    cloud_guest_hint_->setWordWrap(true);
    info_card_vbox->addWidget(cloud_guest_hint_);

    layout->addWidget(info_card);
    layout->addStretch(1);
    return page;
}

QWidget* AccountWindow::makeDetailPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    // Header row: status pill + large bold device name + overflow menu (UU Remote style)
    auto* hdr = new QHBoxLayout;
    hdr->setSpacing(10);
    detail_status_pill_ = new QLabel(QStringLiteral("● 在线"), page);
    detail_status_pill_->setObjectName(QStringLiteral("statusPill"));
    hdr->addWidget(detail_status_pill_);

    detail_name_ = new QLabel(page);
    detail_name_->setObjectName(QStringLiteral("deviceTitle"));
    detail_name_->setTextFormat(Qt::PlainText);
    hdr->addWidget(detail_name_);

    detail_more_btn_ = new QPushButton(QStringLiteral("⋮"), page);
    detail_more_btn_->setObjectName(QStringLiteral("sidebarIconBtn"));
    detail_more_btn_->setFocusPolicy(Qt::NoFocus);
    detail_more_btn_->setToolTip(QStringLiteral("更多操作"));
    hdr->addWidget(detail_more_btn_);
    hdr->addStretch(1);
    layout->addLayout(hdr);

    // Hero card + attached quick-action strip
    auto* hero_container = new QFrame(page);
    hero_container->setObjectName(QStringLiteral("heroContainer"));
    auto* hero_vbox = new QVBoxLayout(hero_container);
    hero_vbox->setContentsMargins(0, 0, 0, 0);
    hero_vbox->setSpacing(0);

    hero_widget_ = new DeviceHeroWidget(hero_container);
    connect(hero_widget_->actionButton(), &QPushButton::clicked, this, [this] {
        connectSelectedDevice(false);
    });
    hero_vbox->addWidget(hero_widget_);

    auto* strip = new QFrame(hero_container);
    strip->setObjectName(QStringLiteral("heroActionBar"));
    auto* strip_row = new QHBoxLayout(strip);
    strip_row->setContentsMargins(10, 6, 10, 6);
    strip_row->setSpacing(8);

    auto makeStripBtn = [&](const QString& text) {
        auto* btn = new QPushButton(text, strip);
        btn->setObjectName(QStringLiteral("heroStripBtn"));
        btn->setFocusPolicy(Qt::NoFocus);
        return btn;
    };
    strip_btn_1_ = makeStripBtn(QStringLiteral("远程控制"));
    strip_btn_2_ = makeStripBtn(QStringLiteral("只读诊断"));
    strip_btn_3_ = makeStripBtn(QStringLiteral("移除设备"));

    auto addSep = [&] {
        auto* s = new QFrame(strip);
        s->setObjectName(QStringLiteral("titleSeparator"));
        strip_row->addWidget(s);
    };
    strip_row->addWidget(strip_btn_1_, 1);
    addSep();
    strip_row->addWidget(strip_btn_2_, 1);
    addSep();
    strip_row->addWidget(strip_btn_3_, 1);
    hero_vbox->addWidget(strip);

    layout->addWidget(hero_container);

    connect(strip_btn_1_, &QPushButton::clicked, this, [this] {
        connectSelectedDevice(false);
    });
    connect(strip_btn_2_, &QPushButton::clicked, this, [this] {
        connectSelectedDevice(true);
    });
    connect(strip_btn_3_, &QPushButton::clicked, this, &AccountWindow::removeSelectedDevice);
    connect(detail_more_btn_, &QPushButton::clicked, this, [this] {
        auto* menu = new QMenu(detail_more_btn_);
        menu->setObjectName(QStringLiteral("remoteMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(QStringLiteral("只读诊断"), this, [this] { connectSelectedDevice(true); })
            ->setEnabled(selected_.state == QStringLiteral("online") && !selected_.is_self);
        menu->addAction(QStringLiteral("移除此设备"), this, &AccountWindow::removeSelectedDevice);
        menu->popup(detail_more_btn_->mapToGlobal(QPoint(0, detail_more_btn_->height() + 4)));
    });

    // Lower section: Quick direct connect
    auto* quick_title = new QLabel(QStringLiteral("快速启动"), page);
    quick_title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(quick_title);

    auto* quick_row = new QHBoxLayout;
    quick_row->setSpacing(12);
    auto* add_direct = new QPushButton(QStringLiteral("＋  局域网直连"), page);
    add_direct->setObjectName(QStringLiteral("quickAddTile"));
    add_direct->setCursor(Qt::PointingHandCursor);
    add_direct->setFocusPolicy(Qt::NoFocus);
    connect(add_direct, &QPushButton::clicked, this, &AccountWindow::openLanConnectDialog);
    quick_row->addWidget(add_direct, 1);
    layout->addLayout(quick_row);
    layout->addStretch(1);
    return page;
}

void AccountWindow::makeLoginPopup() {
    class PopupFrame final : public QFrame {
    public:
        PopupFrame(QWidget* parent, std::function<void()> on_hide)
            : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), on_hide_(std::move(on_hide)) {
            setAttribute(Qt::WA_TranslucentBackground, true);
        }
    protected:
        void hideEvent(QHideEvent* event) override {
            QFrame::hideEvent(event);
            if (on_hide_) on_hide_();
        }
    private:
        std::function<void()> on_hide_;
    };

    login_popup_ = new PopupFrame(this, [this] {
        login_popup_closed_.start();
        if (account_btn_) {
            account_btn_->setProperty("menuOpen", false);
            account_btn_->style()->unpolish(account_btn_);
            account_btn_->style()->polish(account_btn_);
        }
    });
    login_popup_->setFixedWidth(348);

    auto* outer = new QVBoxLayout(login_popup_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* card = new QFrame(login_popup_);
    card->setObjectName(QStringLiteral("accountPopup"));
    outer->addWidget(card);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("登录云端账号"), login_popup_);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);

    auto* notice = new QLabel(
        QStringLiteral("登录后可跨外网管理云端设备；局域网直连无需登录。"), login_popup_);
    notice->setObjectName(QStringLiteral("muted"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto* fields = new QFormLayout;
    fields->setContentsMargins(0, 4, 0, 0);
    fields->setHorizontalSpacing(12);
    fields->setVerticalSpacing(10);
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QSettings settings;
    server_ = new QLineEdit(settings.value(QStringLiteral("lastServer"), winText(p2p_default_server())).toString(), login_popup_);
    server_->setObjectName(QStringLiteral("server"));
    server_->setMaxLength(256);
    server_->setPlaceholderText(QStringLiteral("https://"));
    user_ = new QLineEdit(login_popup_);
    user_->setObjectName(QStringLiteral("user"));
    user_->setMaxLength(32);
    user_->setPlaceholderText(QStringLiteral("输入账号"));
    password_ = new QLineEdit(login_popup_);
    password_->setObjectName(QStringLiteral("password"));
    password_->setMaxLength(72);
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(QStringLiteral("输入密码"));
    remember_ = new QCheckBox(QStringLiteral("记住密码"), login_popup_);
    fields->addRow(QStringLiteral("服务地址"), server_);
    fields->addRow(QStringLiteral("账号"), user_);
    fields->addRow(QStringLiteral("密码"), password_);
    fields->addRow(QString{}, remember_);
    layout->addLayout(fields);

    login_popup_status_ = new QLabel(login_popup_);
    login_popup_status_->setObjectName(QStringLiteral("muted"));
    login_popup_status_->setWordWrap(true);
    login_popup_status_->setTextFormat(Qt::PlainText);
    login_popup_status_->hide();
    layout->addWidget(login_popup_status_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    login_ = new QPushButton(QStringLiteral("登录"), login_popup_);
    login_->setObjectName(QStringLiteral("primary"));
    register_ = new QPushButton(QStringLiteral("注册并登录"), login_popup_);
    buttons->addWidget(login_, 1);
    buttons->addWidget(register_, 1);
    layout->addLayout(buttons);

    connect(login_, &QPushButton::clicked, this, [this] { signIn(false); });
    connect(register_, &QPushButton::clicked, this, [this] { signIn(true); });
    connect(password_, &QLineEdit::returnPressed, this, [this] { signIn(false); });
    connect(user_, &QLineEdit::returnPressed, password_, qOverload<>(&QLineEdit::setFocus));
    connect(server_, &QLineEdit::editingFinished, this, &AccountWindow::loadCredentials);
    connect(server_, &QLineEdit::textEdited, this, [this] {
        if (remember_->isChecked()) password_->clear();
        remember_->setChecked(false);
        loaded_server_.clear();
    });
    connect(user_, &QLineEdit::textEdited, this, [this] {
        if (remember_->isChecked()) password_->clear();
        remember_->setChecked(false);
    });
    connect(remember_, &QCheckBox::clicked, this, [this](bool checked) {
        if (!checked && !deleteCredentials(server_->text())) {
            if (login_popup_status_) {
                login_popup_status_->setText(QStringLiteral("无法删除已保存的凭据。"));
                login_popup_status_->show();
            }
        }
    });
}

void AccountWindow::makeTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    QApplication::setQuitOnLastWindowClosed(false);
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("打开 P2P Remote"), this, &AccountWindow::restore);
    tray_lan_allow_ = menu->addAction(QStringLiteral("允许局域网连接"), this, [this] {
        if (lan_allow_switch_ && lan_allow_switch_->isEnabled()) lan_allow_switch_->toggle();
    });
    tray_receive_ = menu->addAction(QStringLiteral("开启云端接收"), this, &AccountWindow::toggleReceiving);
    menu->addAction(QStringLiteral("局域网直连…"), this, [this] {
        restore();
        openLanConnectDialog();
    });
#if defined(Q_OS_WIN)
    menu->addAction(QStringLiteral("AI Agent 诊断记录…"), this, &AccountWindow::openAgentAudit);
#endif
    menu->addAction(QStringLiteral("设置…"), this, &AccountWindow::openSettings);
    menu->addSeparator();
    menu->addAction(QStringLiteral("退出程序"), this, &AccountWindow::quit);
    tray_->setContextMenu(menu);
    tray_->setToolTip(QStringLiteral("P2P Remote"));
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restore();
    });
    tray_->show();
}

void AccountWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyWin32FramelessChrome(this);
    if (title_bar_) title_bar_->syncMaximizeState();
}

void AccountWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && title_bar_) {
        title_bar_->syncMaximizeState();
    }
}

void AccountWindow::closeEvent(QCloseEvent* event) {
    if (tray_ && !quitting_ && AppSettings::instance().data().close_to_tray) {
        event->ignore();
        hide();
    } else {
        closeSessions();
        event->accept();
        if (!quitting_) QApplication::quit();
    }
}

bool AccountWindow::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
    if (handleWin32FramelessChrome(this, message, result)) return true;
#if defined(Q_OS_WIN)
    const auto* msg = static_cast<MSG*>(message);
    if (SingleInstance::isActivationMessage(msg->message)) {
        if (!quitting_) restore();
        *result = quitting_ ? 0 : 1;
        return true;
    }
    if (msg->message == WM_HOTKEY && msg->wParam == stop_hotkey && (host_ || lan_host_)) {
        if (host_) stopReceiving();
        if (lan_host_) {
            const QSignalBlocker b(lan_allow_switch_);
            if (lan_allow_switch_) lan_allow_switch_->setChecked(false);
            stopLanReceiving();
        }
        setStatus(QStringLiteral("已用本机快捷键停止接收。"));
        *result = 0;
        return true;
    }
    if (msg->message == WM_DISPLAYCHANGE) {
        QMetaObject::invokeMethod(this, [this] {
            if (host_) host_->displayChanged();
            if (lan_host_) lan_host_->displayChanged();
            refreshScreens();
        }, Qt::QueuedConnection);
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}

void AccountWindow::openSettings() {
    if (!settings_dialog_) {
        settings_dialog_ = new SettingsDialog(this);
        settings_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    settings_dialog_->show();
    settings_dialog_->raise();
    settings_dialog_->activateWindow();
}

void AccountWindow::restore() {
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void AccountWindow::quit() {
    quitting_ = true;
    close();
    QApplication::quit();
}

void AccountWindow::setStatus(const QString& text, bool persistent) {
    if (!status_) return;
    status_->setText(text);
    const bool show = !text.trimmed().isEmpty();
    status_->setVisible(show);
    if (show && !persistent) status_clear_timer_.start();
    else status_clear_timer_.stop();
}

void AccountWindow::showLoginPopup() {
    if (!login_popup_ || !account_btn_) return;
    if (login_popup_status_ && request_ == Request::none) login_popup_status_->hide();
    login_popup_->adjustSize();
    const QPoint bottom_right = account_btn_->mapToGlobal(QPoint(account_btn_->width(), account_btn_->height() + 4));
    QPoint pos(bottom_right.x() - login_popup_->width(), bottom_right.y());
    if (const auto* scr = screen()) {
        const QRect avail = scr->availableGeometry();
        pos.setX(std::clamp(pos.x(), avail.left() + 8, std::max(avail.left() + 8, avail.right() - login_popup_->width() - 8)));
        pos.setY(std::clamp(pos.y(), avail.top() + 8, std::max(avail.top() + 8, avail.bottom() - login_popup_->height() - 8)));
    }
    account_btn_->setProperty("menuOpen", true);
    account_btn_->style()->unpolish(account_btn_);
    account_btn_->style()->polish(account_btn_);
    login_popup_->move(pos);
    login_popup_->show();
    login_popup_->raise();
    login_popup_->activateWindow();
    (user_->text().isEmpty() ? user_ : password_)->setFocus();
}

void AccountWindow::showAccountMenu() {
    auto* menu = new QMenu(account_btn_);
    menu->setObjectName(QStringLiteral("remoteMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const bool has_local = p2p_account_device_id(account_.get()) != nullptr;
    if (!has_local) {
        menu->addAction(QStringLiteral("添加本机到云端设备"), this, &AccountWindow::addLocalDevice);
    }
#if defined(Q_OS_WIN)
    const bool service_cloud_active = service_info_.running && service_info_.cloud_enabled;
    if (!service_cloud_active) {
        auto* rec = menu->addAction(host_ ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收"),
            this, &AccountWindow::toggleReceiving);
        rec->setEnabled(has_local && (screens_->count() > 0 || host_));
    }
    if (serviceExecutableExists() || service_info_.installed) {
        menu->addAction(service_info_.installed ? QStringLiteral("卸载系统服务") : QStringLiteral("启用系统服务"),
            this, &AccountWindow::toggleHostService);
    }
    if (!elevated_ && !service_info_.running) {
        menu->addAction(QStringLiteral("以管理员重启"), this, &AccountWindow::relaunchAsAdmin);
    }
    menu->addSeparator();
    menu->addAction(QStringLiteral("暂停 AI Agent 诊断"), this, &AccountWindow::stopAgentDiagnostics);
    menu->addSeparator();
#else
    if (has_local || service_info_.cloud_enabled) {
        const bool enabled = service_info_.cloud_enabled;
        auto* receive = menu->addAction(enabled ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收"),
            this, &AccountWindow::toggleReceiving);
        receive->setEnabled(receive_->isEnabled());
    }
    menu->addSeparator();
#endif
    menu->addAction(QStringLiteral("退出账号"), this, &AccountWindow::signOut);

    account_btn_->setProperty("menuOpen", true);
    account_btn_->style()->unpolish(account_btn_);
    account_btn_->style()->polish(account_btn_);
    connect(menu, &QMenu::aboutToHide, this, [this] {
        if (account_btn_) {
            account_btn_->setProperty("menuOpen", false);
            account_btn_->style()->unpolish(account_btn_);
            account_btn_->style()->polish(account_btn_);
        }
    });

    const int x = account_btn_->width() - menu->sizeHint().width();
    menu->popup(account_btn_->mapToGlobal(QPoint(x, account_btn_->height() + 4)));
}

void AccountWindow::updateLocalPage() {
    const bool service_lan_active = service_info_.running && service_info_.lan_enabled;
    const bool service_cloud_active = service_info_.running && service_info_.cloud_enabled
#if defined(Q_OS_LINUX)
        && service_info_.cloud_online
#endif
        ;
    const bool lan_active = lan_host_ != nullptr || service_lan_active;
    const bool lan_controlled = lan_host_controlled_
        || (service_lan_active && service_info_.lan_desktop_control && service_info_.lan_state == P2P_STREAMING);
    const bool cloud_active = host_ != nullptr || service_cloud_active;
    bool lan_checked = lan_active;
#if defined(Q_OS_LINUX)
    lan_checked = lan_active || service_info_.enabled;
    const bool service_available = serviceExecutableExists() && service_info_.installed;
    const bool service_controls_enabled = service_available && !service_operation_pending_;
    lan_allow_switch_->setEnabled(service_controls_enabled);
    if (tray_lan_allow_) {
        tray_lan_allow_->setEnabled(service_controls_enabled);
        tray_lan_allow_->setText(lan_checked ? QStringLiteral("关闭局域网连接") : QStringLiteral("允许局域网连接"));
    }
#endif

    if (local_lan_card_) {
        setStyledProperty(local_lan_card_, "active", lan_active);
    }
    if (lan_allow_switch_ && lan_allow_switch_->isChecked() != lan_checked) {
        const QSignalBlocker b(lan_allow_switch_);
        lan_allow_switch_->setChecked(lan_checked);
    }

    if (local_status_pill_) {
        const auto status = lan_controlled ? QStringLiteral("busy")
            : ((lan_active || cloud_active) ? QStringLiteral("online") : QStringLiteral("offline"));
        if (lan_controlled) {
            local_status_pill_->setText(QStringLiteral("● 正在被控"));
        } else if (lan_active && cloud_active) {
            local_status_pill_->setText(QStringLiteral("● 局域网与云端就绪"));
        } else if (lan_active) {
            local_status_pill_->setText(QStringLiteral("● 局域网可连接"));
        } else if (cloud_active) {
            local_status_pill_->setText(QStringLiteral("● 云端在线"));
        } else {
            local_status_pill_->setText(QStringLiteral("● 未开启被控"));
        }
        if (setStyledProperty(local_status_pill_, "state", status) && local_nav_btn_) {
            local_nav_btn_->setIcon(makeSidebarDeviceIcon(status));
        }
    }

    if (local_lan_desc_) {
#if defined(Q_OS_WIN)
        if (lan_controlled) {
            local_lan_desc_->setText(service_lan_active
                ? QStringLiteral("[系统服务] 远程主控已接入（SYSTEM 权限，支持控制虚拟机与管理员窗口）")
                : QStringLiteral("远程主控已接入，按 Ctrl+Alt+F11 可随时断开本机被控"));
        } else if (service_lan_active) {
            local_lan_desc_->setText(QStringLiteral("[系统服务] 同网段设备已可发现此电脑，支持控制虚拟机与管理员窗口"));
        } else if (lan_active) {
            local_lan_desc_->setText(QStringLiteral("同网段设备已可发现此电脑，使用下方 IP 与连接验证码即可直连"));
        } else {
            local_lan_desc_->setText(QStringLiteral("开启后，同网段设备可通过本机 IP 与连接验证码直接控制此电脑"));
        }
#else
        if (service_operation_pending_) {
            local_lan_desc_->setText(QStringLiteral("正在更新系统服务，请完成管理员授权"));
        } else if (!service_info_.lan_status.isEmpty()) {
            local_lan_desc_->setText(service_info_.lan_status);
        } else if (service_available) {
            local_lan_desc_->setText(QStringLiteral("同网段设备可通过本机 IP 和连接验证码控制此电脑"));
        } else {
            local_lan_desc_->setText(QStringLiteral("安装 P2P Remote 系统服务并启用系统授权后，即可开启本机接收"));
        }
#endif
    }

    if (lan_info_area_) lan_info_area_->setVisible(lan_active);
    if (lan_off_hint_) lan_off_hint_->setVisible(!lan_active);

    if (lan_active) {
        const bool has_access_code = !lan_host_code_.isEmpty();
        if (local_code_value_) {
            local_code_value_->setText(!has_access_code ? QStringLiteral("未保存") : lan_code_visible_
                ? lan_host_code_
                : QString(std::max<qsizetype>(6, lan_host_code_.size()), QChar(0x2022)));
        }
        if (lan_toggle_code_btn_) {
            lan_toggle_code_btn_->setEnabled(has_access_code);
            lan_toggle_code_btn_->setText(lan_code_visible_ ? QStringLiteral("隐藏验证码") : QStringLiteral("显示验证码"));
        }
        if (lan_copy_btn_) lan_copy_btn_->setEnabled(has_access_code);
#if defined(Q_OS_LINUX)
        lan_edit_btn_->setEnabled(service_controls_enabled);
#endif
        if (screens_) {
            screens_->setVisible(screens_->count() > 1);
            screens_->setEnabled(!lan_controlled);
        }
    }

    updateLocalAddresses();
#if defined(Q_OS_WIN)
    if (local_summary_display_ && screens_) {
        if (screens_->count() == 0) {
            local_summary_display_->setText(QStringLiteral("未检测到显示器"));
        } else if (screens_->count() == 1) {
            local_summary_display_->setText(screens_->currentText());
        } else {
            local_summary_display_->setText(QStringLiteral("%1（共 %2 台）")
                .arg(screens_->currentText()).arg(screens_->count()));
        }
    }
#else
    const auto* screen = QGuiApplication::primaryScreen();
    if (screen) {
        const auto size = screen->size();
        local_summary_display_->setText(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
    }
    const bool signed_in = p2p_account_signed_in(account_.get());
    local_summary_account_->setText(signed_in ? QStringLiteral("已登录") : QStringLiteral("未登录"));
    cloud_guest_hint_->setVisible(!signed_in);
#endif

    if (local_nav_btn_) local_nav_btn_->setChecked(viewing_local_);
}

void AccountWindow::updateLocalAddresses() {
    const auto ips = winText(p2p_lan_local_addresses());
    const auto first_ip = ips.section(QStringLiteral(","), 0, 0).trimmed();
    const auto address = first_ip.isEmpty() ? QStringLiteral("未检测到局域网") : first_ip;
    local_summary_ip_->setText(address);
    const bool lan_active = lan_host_ != nullptr || (service_info_.running && service_info_.lan_enabled);
    if (lan_active && local_ip_value_ && local_ip_extra_) {
        const auto other_ips = ips.section(QStringLiteral(","), 1).trimmed();
        const auto port = service_info_.running ? service_info_.lan_port : lan_host_port_;
        local_ip_value_->setText(!first_ip.isEmpty() && port != P2P_LAN_DEFAULT_PORT
            ? QStringLiteral("%1:%2").arg(address).arg(port) : address);
        local_ip_extra_->setText(other_ips.isEmpty()
            ? QStringLiteral("同网段设备可自动发现或手动输入此地址")
            : QStringLiteral("其他地址：%1").arg(other_ips));
    }
}

void AccountWindow::updateDetailView() {
    if (viewing_local_ || !selected_.valid()) {
        if (right_stack_ && right_stack_->currentWidget() != local_page_) {
            right_stack_->setCurrentWidget(local_page_);
        }
        return;
    }

    detail_name_->setText(selected_.name);
    const bool online = selected_.state == QStringLiteral("online");
    const bool busy = selected_.state == QStringLiteral("busy");
    if (online) {
        detail_status_pill_->setText(QStringLiteral("● 在线"));
        detail_status_pill_->setProperty("state", QStringLiteral("online"));
    } else if (busy) {
        detail_status_pill_->setText(QStringLiteral("● 控制中"));
        detail_status_pill_->setProperty("state", QStringLiteral("busy"));
    } else {
        detail_status_pill_->setText(QStringLiteral("● 离线"));
        detail_status_pill_->setProperty("state", QStringLiteral("offline"));
    }
    detail_status_pill_->style()->unpolish(detail_status_pill_);
    detail_status_pill_->style()->polish(detail_status_pill_);

    const bool can_connect = online && !selected_.is_self;
    QString subtitle = selected_.platform;
    if (selected_.is_self) subtitle += QStringLiteral(" · 本机云端设备");
    else if (online) subtitle += QStringLiteral(" · 云端在线，可建立远程控制");
    else if (busy) subtitle += QStringLiteral(" · 设备正在被控制");
    else subtitle += QStringLiteral(" · 设备离线");
    hero_widget_->setContent(subtitle,
        selected_.is_self ? QStringLiteral("当前为本机") : QStringLiteral("开始远程控制"),
        can_connect, online || busy);
    strip_btn_1_->setText(QStringLiteral("远程控制"));
    strip_btn_1_->setEnabled(can_connect);
    strip_btn_2_->setText(QStringLiteral("只读诊断"));
    strip_btn_2_->setEnabled(can_connect);
    strip_btn_3_->setText(QStringLiteral("移除设备"));
    strip_btn_3_->setEnabled(request_ == Request::none);
}

void AccountWindow::updateControls() {
    if (!account_) return;
    const auto* local = p2p_account_device_id(account_.get());
    const bool signed_in = p2p_account_signed_in(account_.get()) != 0;
    const bool busy = request_ != Request::none;
    const bool changing_account = busy && request_ != Request::devices;

    if (account_btn_) {
        account_btn_->setText(signed_in && active_credentials_
            ? QStringLiteral("%1 ▾").arg(active_credentials_->user)
            : QStringLiteral("登录账号 ▾"));
    }
    if (cloud_refresh_btn_) {
        cloud_refresh_btn_->setVisible(signed_in);
        cloud_refresh_btn_->setEnabled(!changing_account && !add_device_after_refresh_);
    }
    if (login_popup_) login_popup_->setEnabled(!busy);

#if defined(Q_OS_WIN)
    const bool service_active = service_info_.running;
    const bool service_cloud_active = service_active && service_info_.cloud_enabled;
    const bool service_lan_active = service_active && service_info_.lan_enabled;
    const bool lan_active = lan_host_ != nullptr || service_lan_active;
    if (service_hint_) {
        if (service_active) {
            if (service_lan_active && !service_info_.lan_status.isEmpty()) {
                service_hint_->setText(service_info_.lan_status);
            } else if (service_cloud_active && !service_info_.status.isEmpty()) {
                service_hint_->setText(service_info_.status);
            } else {
                service_hint_->setText(QStringLiteral("[系统服务] 已以 SYSTEM 权限后台托管，支持控制虚拟机与管理员窗口"));
            }
        } else {
            service_hint_->setText(QStringLiteral("启用系统服务后将以 SYSTEM 权限托管被控（支持控制 VMware、UAC 与管理员窗口）"));
        }
    }
    if (cloud_host_section_) cloud_host_section_->setVisible(signed_in);
    if (cloud_guest_hint_) cloud_guest_hint_->setVisible(!signed_in);
    if (add_device_) {
        add_device_->setVisible(!local);
        add_device_->setEnabled(signed_in && !changing_account && !add_device_after_refresh_);
    }
    if (receive_) {
        receive_->setVisible(!service_cloud_active);
        receive_->setEnabled(local && (screens_->count() || host_));
        receive_->setText(host_ ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收"));
    }
    if (cloud_host_status_) {
        if (service_cloud_active) {
            cloud_host_status_->setText(service_info_.status.isEmpty()
                ? QStringLiteral("[系统服务] 后台云端接收已启动") : service_info_.status);
        } else if (host_) {
            cloud_host_status_->setText(host_status_text_.isEmpty()
                ? QStringLiteral("本机云端接收运行中") : host_status_text_);
        } else {
            cloud_host_status_->setText(local ? QStringLiteral("云端接收已暂停")
                : QStringLiteral("尚未将本机添加到云端设备"));
        }
    }
    if (service_button_) {
        const bool has_service_exe = serviceExecutableExists();
        service_button_->setVisible(has_service_exe || service_info_.installed);
        service_button_->setText(service_info_.installed ? QStringLiteral("卸载系统服务") : QStringLiteral("启用系统服务"));
        service_button_->setEnabled(has_service_exe || service_info_.installed);
    }
    if (elevate_button_) elevate_button_->setVisible(!elevated_ && !service_active);
    if (privilege_status_) {
        if (service_active) {
            privilege_status_->setText(QStringLiteral("SYSTEM 系统服务"));
        } else if (elevated_) {
            privilege_status_->setText(QStringLiteral("管理员权限"));
        } else {
            privilege_status_->setText(QStringLiteral("普通用户权限"));
        }
    }

    if (tray_lan_allow_) {
        tray_lan_allow_->setText(lan_active ? QStringLiteral("关闭局域网连接") : QStringLiteral("允许局域网连接"));
    }
    if (tray_receive_) {
        tray_receive_->setVisible(signed_in);
        tray_receive_->setText(service_cloud_active ? QStringLiteral("云端接收由系统服务托管")
            : (host_ ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收")));
        tray_receive_->setEnabled(signed_in && !service_cloud_active && local);
    }
#else
    if (busy && !account_event_.isActive()) account_event_.start();
    if (!busy) account_event_.stop();
    const bool receiving = service_info_.cloud_enabled;
    const bool available = service_info_.cloud_installed && serviceExecutableExists();
    cloud_host_section_->setVisible(signed_in || receiving);
    add_device_->setVisible(signed_in && !local);
    add_device_->setEnabled(signed_in && !changing_account && !add_device_after_refresh_);
    receive_->setVisible(receiving || local);
    receive_->setEnabled((receiving || (signed_in && local)) && available && !changing_account && !service_operation_pending_);
    const auto action = receiving ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收");
    receive_->setText(action);
    if (receiving) cloud_host_status_->setText(service_info_.status);
    else if (!local) cloud_host_status_->setText(QStringLiteral("尚未将本机添加到云端设备"));
    else if (!available) cloud_host_status_->setText(QStringLiteral("需安装云端被控服务才能接收连接"));
    else cloud_host_status_->setText(QStringLiteral("本机云端接收已暂停"));
    if (tray_receive_) {
        tray_receive_->setVisible(receiving || (signed_in && local));
        tray_receive_->setEnabled(receive_->isEnabled());
        tray_receive_->setText(action);
    }
#endif

    updateLocalPage();
    updateDetailView();
}

#if defined(Q_OS_WIN)
void AccountWindow::setLanAllowEnabled(bool enabled) {
    if (service_info_.running && service_info_.lan_enabled) {
        if (!enabled) {
            if (!service_info_.cloud_enabled) {
                toggleHostService();
            } else {
                QString error;
                if (!installOrUpdateHostService(false, true, &error)) setStatus(error);
                else setStatus(QStringLiteral("已从系统服务中关闭局域网连接。"));
            }
        }
        updateLocalPage();
        return;
    }
    if (enabled) {
        if (!lan_host_) startLanReceiving();
    } else {
        if (lan_host_) stopLanReceiving();
    }
}

void AccountWindow::startLanHostFromCli(const QString& code, std::uint16_t port) {
    lan_host_port_ = port ? port : P2P_LAN_DEFAULT_PORT;
    if (!code.isEmpty()) {
        lan_host_code_ = code;
        saveLanSecret(QStringLiteral("HostAccessCode"), code);
    }
    const QSignalBlocker b(lan_allow_switch_);
    if (lan_allow_switch_) lan_allow_switch_->setChecked(true);
    startLanReceiving(lan_host_code_, lan_host_port_);
}
#endif

void AccountWindow::connectLanFromCli(const QString& target, const QString& code) {
    QString host = target.trimmed();
    std::uint16_t port = P2P_LAN_DEFAULT_PORT;
    const int colon = host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && host.indexOf(QLatin1Char(':')) == colon) {
        bool ok = false;
        const int parsed = host.mid(colon + 1).toInt(&ok);
        if (ok && parsed > 0 && parsed <= 65535) {
            port = static_cast<std::uint16_t>(parsed);
            host = host.left(colon).trimmed();
        }
    }
    if (host.isEmpty() || code.size() < 6 || code.size() > 32) return;
    LanConnectOptions lan{host.toStdWString(), port, code.toStdWString()};
    if (!viewer_) viewer_ = new RemoteWindow(this);
    viewer_->openLanSession(lan, host, options_);
}

#if defined(Q_OS_WIN)
void AccountWindow::startLanReceiving(const QString& override_code, std::uint16_t port) {
    if (lan_host_ || (service_info_.running && service_info_.lan_enabled) || !screens_ || !screens_->count()) return;
    QString code = !override_code.isEmpty() ? override_code : lan_host_code_;
    if (code.size() < 6 || code.size() > 32) {
        code = generateDefaultAccessCode();
    }
    lan_host_code_ = code;
    lan_host_port_ = port ? port : P2P_LAN_DEFAULT_PORT;
    saveLanSecret(QStringLiteral("HostAccessCode"), lan_host_code_);

    const auto screen = reinterpret_cast<void*>(static_cast<quintptr>(screens_->currentData().toULongLong()));
    const auto generation = ++lan_host_generation_;
    LanConnectOptions lan{{}, lan_host_port_, lan_host_code_.toStdWString()};
    QString error;
    auto session = std::make_unique<HostSession>(lan, screen, options_,
        [this, generation](int state, const QString& status, bool desktop_control) {
            QMetaObject::invokeMethod(this, [this, generation, state, status, desktop_control] {
                if (generation != lan_host_generation_ || !lan_host_) return;
                lan_host_status_text_ = status;
                lan_host_controlled_ = desktop_control && state == P2P_STREAMING;
                updateLocalPage();
                if (state == P2P_STOPPED) {
                    const QSignalBlocker b(lan_allow_switch_);
                    if (lan_allow_switch_) lan_allow_switch_->setChecked(false);
                    stopLanReceiving();
                    setStatus(status);
                }
            }, Qt::QueuedConnection);
        }, &error);
    if (!session->valid()) {
        const QSignalBlocker b(lan_allow_switch_);
        if (lan_allow_switch_) lan_allow_switch_->setChecked(false);
        setStatus(error);
        updateControls();
        return;
    }
    if (!hotkey_) {
        hotkey_ = RegisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey,
            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11) != FALSE;
    }
    lan_host_ = std::move(session);
    lan_host_controlled_ = false;
    setStatus(QString{});
    updateControls();
}

void AccountWindow::stopLanReceiving() {
    ++lan_host_generation_;
    lan_host_.reset();
    lan_host_controlled_ = false;
    lan_host_status_text_.clear();
    if (hotkey_ && !host_) {
        UnregisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey);
        hotkey_ = false;
    }
    updateControls();
}
#else
void AccountWindow::setLanAllowEnabled(bool enabled) {
    changeLinuxHostService(enabled, lan_host_code_);
}

void AccountWindow::changeLinuxHostService(bool enabled, QString code) {
    if (service_operation_pending_) return;
    if (enabled && (code.toUtf8().size() < 6 || code.toUtf8().size() > 32)) code = generateDefaultAccessCode();
    service_operation_pending_ = true;
    ++service_status_generation_;
    updateLocalPage();
    manageLinuxHostService(this, enabled, lan_host_port_, code, [this, enabled, code](const QString& error) {
        service_operation_pending_ = false;
        if (error.isEmpty()) {
            service_info_.enabled = enabled;
            if (enabled) {
                lan_host_code_ = code;
                saveLanSecret(QStringLiteral("HostAccessCode"), lan_host_code_);
            } else {
                service_info_.running = service_info_.cloud_enabled;
                service_info_.lan_enabled = false;
                service_info_.lan_status.clear();
            }
            setStatus(enabled ? QStringLiteral("已启用系统服务和开机接收") : QStringLiteral("已关闭本机接收"));
        } else {
            setStatus(error);
        }
        updateLocalPage();
        refreshServiceStatus();
    });
}
#endif

void AccountWindow::editLanAccessCode() {
    FramelessDialog dialog(QStringLiteral("修改连接验证码"), false, this);
    dialog.resize(380, 190);
    auto* form = new QFormLayout;
    form->setSpacing(12);
    auto* edit = new QLineEdit(lan_host_code_, &dialog);
    edit->setMaxLength(32);
    edit->setPlaceholderText(QStringLiteral("6–32 位字母或数字"));
    form->addRow(QStringLiteral("连接验证码"), edit);
    dialog.contentLayout()->addLayout(form);

    auto* hint = new QLabel(
#if defined(Q_OS_WIN)
        QStringLiteral("修改后将立即生效，主控端需使用新验证码连接本机。"),
#else
        QStringLiteral("保存后会重启被控服务，并断开当前远程连接。"),
#endif
        &dialog);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    dialog.contentLayout()->addWidget(hint);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消"), &dialog);
    auto* ok = new QPushButton(QStringLiteral("保存"), &dialog);
    ok->setObjectName(QStringLiteral("primary"));
    btns->addWidget(cancel);
    btns->addWidget(ok);
    dialog.contentLayout()->addLayout(btns);

    QString next_code;
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dialog, [&] {
        const auto next = edit->text().trimmed();
#if defined(Q_OS_WIN)
        const auto length = next.size();
#else
        const auto length = next.toUtf8().size();
#endif
        if (length < 6 || length > 32 || next.contains(u'\r') || next.contains(u'\n') || next.contains(QChar::Null)) {
            QMessageBox::warning(&dialog, QStringLiteral("连接验证码"), QStringLiteral("连接验证码长度需为 6 至 32 位。"));
            return;
        }
        next_code = next;
        dialog.accept();
    });
    connect(edit, &QLineEdit::returnPressed, ok, &QPushButton::click);
    edit->selectAll();
    edit->setFocus();

    if (dialog.exec() == QDialog::Accepted) {
#if defined(Q_OS_WIN)
        lan_host_code_ = next_code;
        saveLanSecret(QStringLiteral("HostAccessCode"), lan_host_code_);
        if (service_info_.running && service_info_.lan_enabled) {
            QString error;
            if (!installOrUpdateHostService(true, service_info_.cloud_enabled, &error)) {
                setStatus(error);
            } else {
                setStatus(QStringLiteral("连接验证码已更新并同步至系统服务。"));
            }
        } else {
            if (lan_host_) {
                stopLanReceiving();
                startLanReceiving(lan_host_code_, lan_host_port_);
            }
            setStatus(QStringLiteral("连接验证码已更新。"));
        }
        updateLocalPage();
#else
        changeLinuxHostService(true, std::move(next_code));
#endif
    }
}

void AccountWindow::copyLanConnectionInfo() {
    const auto ips = winText(p2p_lan_local_addresses());
    const auto first = ips.section(QStringLiteral(","), 0, 0).trimmed();
    QString text;
    const auto port = service_info_.running ? service_info_.lan_port : lan_host_port_;
    const auto target = port == P2P_LAN_DEFAULT_PORT ? first : QStringLiteral("%1:%2").arg(first).arg(port);
    if (!first.isEmpty()) text = QStringLiteral("IP: %1  验证码: %2").arg(target, lan_host_code_);
    else text = lan_host_code_;
    if (auto* cb = QApplication::clipboard()) cb->setText(text);
    setStatus(QStringLiteral("已复制本机局域网连接信息。"));
}

void AccountWindow::openLanConnectDialog() {
    FramelessDialog dialog(QStringLiteral("局域网直连"), false, this);
    dialog.resize(450, 420);

    std::unique_ptr<p2p_lan_scanner, decltype(&p2p_lan_scanner_destroy)> scanner(
        p2p_lan_scanner_create(), p2p_lan_scanner_destroy);
    if (scanner) p2p_lan_scanner_probe(scanner.get());

    // Section 1: Auto-discovered LAN hosts inside the dialog
    auto* scan_hdr = new QHBoxLayout;
    auto* scan_title = new QLabel(QStringLiteral("局域网在线设备（自动发现）"), &dialog);
    scan_title->setObjectName(QStringLiteral("sectionTitle"));
    scan_hdr->addWidget(scan_title, 1);
    auto* rescan_btn = new QPushButton(QStringLiteral("↻ 重新扫描"), &dialog);
    rescan_btn->setObjectName(QStringLiteral("miniGhostBtn"));
    rescan_btn->setFocusPolicy(Qt::NoFocus);
    scan_hdr->addWidget(rescan_btn);
    dialog.contentLayout()->addLayout(scan_hdr);

    auto* peer_card = new QFrame(&dialog);
    peer_card->setObjectName(QStringLiteral("accountCard"));
    auto* peer_card_layout = new QVBoxLayout(peer_card);
    peer_card_layout->setContentsMargins(6, 6, 6, 6);
    peer_card_layout->setSpacing(0);

    auto* peer_list = new QListWidget(peer_card);
    peer_list->setObjectName(QStringLiteral("sidebarList"));
    peer_list->setIconSize(QSize(28, 18));
    peer_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    peer_list->setMinimumHeight(130);
    peer_list->setMaximumHeight(160);
    peer_card_layout->addWidget(peer_list);
    dialog.contentLayout()->addWidget(peer_card);

    auto* divider = new QFrame(&dialog);
    divider->setObjectName(QStringLiteral("divider"));
    dialog.contentLayout()->addWidget(divider);

    // Section 2: Direct connection parameters
    const QString initial_host = QSettings().value(QStringLiteral("lan/lastTarget")).toString();
    const QString initial_code = !initial_host.isEmpty()
        ? readLanSecret(QStringLiteral("Peer/") + initial_host) : QString{};

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);

    auto* host_edit = new QLineEdit(initial_host, &dialog);
    host_edit->setPlaceholderText(QStringLiteral("点击上方设备或输入 IP（如 192.168.0.32）"));
    host_edit->setMaxLength(128);
    form->addRow(QStringLiteral("目标 IP 地址"), host_edit);

    auto* code_row = new QHBoxLayout;
    code_row->setSpacing(8);
    auto* code_edit = new QLineEdit(initial_code, &dialog);
    code_edit->setPlaceholderText(QStringLiteral("输入远端连接验证码"));
    code_edit->setMaxLength(32);
    code_edit->setEchoMode(QLineEdit::Password);
    code_row->addWidget(code_edit, 1);
    auto* show_cb = new QCheckBox(QStringLiteral("显示"), &dialog);
    connect(show_cb, &QCheckBox::toggled, &dialog, [code_edit](bool checked) {
        code_edit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    });
    code_row->addWidget(show_cb);
    form->addRow(QStringLiteral("连接验证码"), code_row);

    auto* remember_cb = new QCheckBox(QStringLiteral("记住此设备的连接验证码"), &dialog);
    remember_cb->setChecked(true);
    form->addRow(QString{}, remember_cb);

    dialog.contentLayout()->addLayout(form);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消"), &dialog);
    auto* go = new QPushButton(QStringLiteral("立即连接"), &dialog);
    go->setObjectName(QStringLiteral("primary"));
    btns->addWidget(cancel);
    btns->addWidget(go);
    dialog.contentLayout()->addLayout(btns);

    QString chosen_name;
    QString chosen_addr;
    std::uint16_t chosen_port{};
    int scan_ticks{};
    QStringList last_peer_sig;
    int last_empty_state = -1;
    auto populatePeers = [&] {
        if (!scanner) return;
        const auto count = p2p_lan_scanner_read(scanner.get());
        if (count == 0) {
            const int empty_state = scan_ticks < 2 ? 0 : 1;
            if (last_empty_state == empty_state) return;
            last_empty_state = empty_state;
            last_peer_sig.clear();
            const QSignalBlocker b(peer_list);
            peer_list->clear();
            const auto placeholder = empty_state == 0
                ? QStringLiteral("正在搜索同网段在线设备…")
                : QStringLiteral("暂未发现同网段在线设备，可点击「重新扫描」或在下方输入 IP");
            auto* empty = new QListWidgetItem(placeholder, peer_list);
            empty->setFlags(Qt::NoItemFlags);
            return;
        }
        struct PeerRow {
            QString name, addr, state, status_txt;
            int port;
        };
        QVector<PeerRow> rows;
        rows.reserve(static_cast<qsizetype>(count));
        QStringList sig;
        sig.reserve(static_cast<qsizetype>(count));
        for (size_t i = 0; i < count; ++i) {
            p2p_lan_peer_v1 peer{};
            if (p2p_lan_scanner_peer(scanner.get(), i, &peer) != P2P_OK) continue;
            const auto name = winText(peer.name);
            const auto addr = winText(peer.address);
            const auto state = peer.busy ? QStringLiteral("busy") : QStringLiteral("online");
            const auto status_txt = peer.busy ? QStringLiteral("控制中") : QStringLiteral("在线");
            rows.push_back({name, addr, state, status_txt, static_cast<int>(peer.port)});
            sig.push_back(QStringLiteral("%1|%2|%3|%4").arg(addr, name).arg(peer.port).arg(peer.busy ? 1 : 0));
        }
        if (last_empty_state == 2 && sig == last_peer_sig) return;
        last_empty_state = 2;
        last_peer_sig = sig;

        const auto prev_addr = peer_list->currentItem() ? peer_list->currentItem()->data(Qt::UserRole).toString() : QString{};
        const QSignalBlocker b(peer_list);
        peer_list->clear();
        for (const auto& row : rows) {
            auto* item = new QListWidgetItem(makeSidebarDeviceIcon(row.state, true),
                QStringLiteral("%1   ·   %2   (%3)").arg(row.name, row.addr, row.status_txt), peer_list);
            item->setData(Qt::UserRole, row.addr);
            item->setData(device_name_role, row.name);
            item->setData(lan_port_role, row.port);
            if (row.addr == prev_addr) peer_list->setCurrentItem(item);
        }
    };

    QTimer scan_timer(&dialog);
    scan_timer.setInterval(800);
    connect(&scan_timer, &QTimer::timeout, &dialog, [&] {
        ++scan_ticks;
        populatePeers();
    });
    connect(rescan_btn, &QPushButton::clicked, &dialog, [&] {
        scan_ticks = 0;
        if (scanner) p2p_lan_scanner_probe(scanner.get());
        populatePeers();
    });
    populatePeers();
    scan_timer.start();

    connect(peer_list, &QListWidget::itemClicked, &dialog, [&](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        const auto addr = item->data(Qt::UserRole).toString();
        chosen_addr = addr;
        chosen_name = item->data(device_name_role).toString();
        chosen_port = static_cast<std::uint16_t>(item->data(lan_port_role).toInt());
        host_edit->setText(addr);
        const auto saved = readLanSecret(QStringLiteral("Peer/") + addr);
        if (!saved.isEmpty()) code_edit->setText(saved);
        else code_edit->clear();
        code_edit->setFocus();
    });
    connect(peer_list, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        const auto addr = item->data(Qt::UserRole).toString();
        chosen_addr = addr;
        chosen_name = item->data(device_name_role).toString();
        chosen_port = static_cast<std::uint16_t>(item->data(lan_port_role).toInt());
        host_edit->setText(addr);
        const auto saved = readLanSecret(QStringLiteral("Peer/") + addr);
        if (!saved.isEmpty()) {
            code_edit->setText(saved);
            go->click();
        } else {
            code_edit->clear();
            code_edit->setFocus();
        }
    });

    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(host_edit, &QLineEdit::returnPressed, code_edit, qOverload<>(&QLineEdit::setFocus));

    LanConnectOptions result;
    QString display_name;
    connect(go, &QPushButton::clicked, &dialog, [&] {
        const auto raw = host_edit->text().trimmed();
        const auto code = code_edit->text();
        if (raw.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("局域网直连"), QStringLiteral("请输入目标设备的 IP 地址。"));
            host_edit->setFocus();
            return;
        }
        if (code.size() < 6 || code.size() > 32) {
            QMessageBox::warning(&dialog, QStringLiteral("局域网直连"), QStringLiteral("请输入 6 至 32 位连接验证码。"));
            code_edit->setFocus();
            return;
        }
        QString host = raw;
        std::uint16_t port = (host == chosen_addr && chosen_port) ? chosen_port : P2P_LAN_DEFAULT_PORT;
        const int colon = raw.lastIndexOf(QLatin1Char(':'));
        if (colon > 0 && raw.indexOf(QLatin1Char(':')) == colon) {
            bool ok = false;
            const int parsed = raw.mid(colon + 1).toInt(&ok);
            if (!ok || parsed <= 0 || parsed > 65535) {
                QMessageBox::warning(&dialog, QStringLiteral("局域网直连"), QStringLiteral("地址格式无效。"));
                host_edit->setFocus();
                return;
            }
            host = raw.left(colon).trimmed();
            port = static_cast<std::uint16_t>(parsed);
        }
        QSettings().setValue(QStringLiteral("lan/lastTarget"), host);
        if (remember_cb->isChecked()) saveLanSecret(QStringLiteral("Peer/") + host, code);
        else deleteLanSecret(QStringLiteral("Peer/") + host);

        result = {host.toStdWString(), port, code.toStdWString()};
        display_name = (!chosen_name.isEmpty() && host == chosen_addr) ? chosen_name : host;
        dialog.accept();
    });
    connect(code_edit, &QLineEdit::returnPressed, go, &QPushButton::click);

    if (initial_host.isEmpty()) host_edit->setFocus();
    else code_edit->setFocus();

    if (dialog.exec() == QDialog::Accepted) {
        if (!viewer_) viewer_ = new RemoteWindow(this);
        viewer_->openLanSession(result, display_name, options_);
    }
}

void AccountWindow::connectSelectedDevice(bool diagnostics) {
    if (!selected_.valid() || selected_.state != QStringLiteral("online") || selected_.is_self) return;
    if (!viewer_) viewer_ = new RemoteWindow(this);
    viewer_->openSession(account_.get(), selected_.id, selected_.name, options_, diagnostics);
}

void AccountWindow::loadCredentials() {
    const auto server = server_->text().trimmed();
    if (server.isEmpty() || server == loaded_server_) return;
    loaded_server_ = server;
    if (auto saved = readCredentials(server)) {
        user_->setText(saved->user);
        password_->setText(QString::fromStdWString(saved->password));
        remember_->setChecked(true);
    }
}

void AccountWindow::signIn(bool create) {
    if (request_ != Request::none || p2p_account_signed_in(account_.get())) return;
    pending_.emplace(server_->text().trimmed(), user_->text(), password_->text().toStdWString(), remember_->isChecked());
    const auto endpoint = pending_->server.toStdWString();
    const auto user = pending_->user.toStdWString();
    if (p2p_account_login(account_.get(), endpoint.c_str(), user.c_str(), pending_->password.c_str(),
        create, options_.compatible_http, options_.relay_only) != P2P_OK) {
        const auto err = winText(p2p_last_error());
        if (login_popup_status_ && login_popup_ && login_popup_->isVisible()) {
            login_popup_status_->setText(err);
            login_popup_status_->show();
            login_popup_->adjustSize();
        } else {
            setStatus(err, true);
        }
        pending_.reset();
        return;
    }
    password_->clear();
    request_ = Request::login;
    const auto msg = create ? QStringLiteral("正在注册账号…") : QStringLiteral("正在登录…");
    if (login_popup_status_ && login_popup_ && login_popup_->isVisible()) {
        login_popup_status_->setText(msg);
        login_popup_status_->show();
        login_popup_->adjustSize();
    } else {
        setStatus(msg);
    }
    updateControls();
}

void AccountWindow::refresh() {
#if defined(Q_OS_WIN)
    refreshServiceStatus();
#endif
    if (request_ != Request::none || !p2p_account_signed_in(account_.get())) return;
    if (p2p_account_refresh(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::devices;
    updateControls();
}

void AccountWindow::addLocalDevice() {
    if (!p2p_account_signed_in(account_.get()) || p2p_account_device_id(account_.get())) return;
    if (request_ == Request::devices) {
        if (!add_device_after_refresh_) {
            add_device_after_refresh_ = true;
            setStatus(QStringLiteral("正在添加本机…"));
            updateControls();
        }
        return;
    }
    if (request_ != Request::none) return;
    if (p2p_account_add_local_device(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::add_device;
    setStatus(QStringLiteral("正在添加本机…"));
    updateControls();
}

void AccountWindow::removeSelectedDevice() {
    if (request_ != Request::none || !p2p_account_signed_in(account_.get()) || !selected_.valid()) return;
    const auto id = selected_.id.toStdWString();
    const auto name = selected_.name;
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("移除设备"),
        QStringLiteral("确定移除「%1」？它将停止接收当前账号的远程连接，现有会话也会结束。").arg(name),
        QMessageBox::Yes | QMessageBox::No, this);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::No);
    refresh_.stop();
    const bool accepted = confirm.exec() == QMessageBox::Yes;
    refresh_.start();
    if (!accepted) return;
    if (p2p_account_remove_device(account_.get(), id.c_str()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::remove_device;
    setStatus(QStringLiteral("正在移除设备…"));
    updateControls();
}

void AccountWindow::completeRequest() {
    const auto registered_device = winText(p2p_account_device_id(account_.get()));
    const auto result = p2p_account_complete(account_.get());
    if (result == P2P_PENDING) return;
    const auto request = std::exchange(request_, Request::none);
    if (result == P2P_OK) {
        if (request == Request::login) {
            QSettings().setValue(QStringLiteral("lastServer"), pending_->server);
            if (pending_->remember && !saveCredentials(*pending_)) {
                setStatus(QStringLiteral("已登录，无法保存账号凭据。"));
            } else {
                setStatus(QString{});
            }
            active_credentials_.emplace(pending_->server, pending_->user, pending_->password, pending_->remember);
            pending_.reset();
            if (login_popup_status_) login_popup_status_->hide();
            if (login_popup_) login_popup_->hide();
#if defined(Q_OS_WIN)
            refreshServiceStatus();
            if (!(service_info_.running && service_info_.cloud_enabled)
                && AppSettings::instance().data().auto_receive_on_login) startReceiving();
#endif
            right_stack_->setCurrentWidget(viewing_local_ || !selected_.valid() ? local_page_ : detail_page_);
            refresh_.start();
            refresh();
        } else if (request == Request::devices || request == Request::add_device || request == Request::remove_device) {
            if (!registered_device.isEmpty() && !p2p_account_device_id(account_.get())) {
#if defined(Q_OS_WIN)
                if (host_) stopReceiving();
#else
                if (request == Request::remove_device && service_info_.cloud_enabled &&
                    service_info_.device_id == registered_device) changeLinuxCloudReceiving(false);
#endif
                selected_ = {};
                setStatus(QStringLiteral("本机已从账号移除，云端接收已停止。"));
            } else if (request == Request::add_device) {
                setStatus(QStringLiteral("已添加本机到云端设备。"));
            } else if (request == Request::remove_device) {
                selected_ = {};
                setStatus(QStringLiteral("已移除设备。"));
            }
            fillDevices();
            if (request != Request::devices) refresh();
        } else {
            setStatus(QStringLiteral("已退出账号。"));
        }
    } else {
        const auto error = winText(p2p_last_error());
        if (result == P2P_LOGIN_EXPIRED) {
            refresh_.stop();
#if defined(Q_OS_WIN)
            stopReceiving();
#endif
            active_credentials_.reset();
            selected_ = {};
            fillDevices();
        }
        pending_.reset();
        if (request == Request::login && login_popup_ && login_popup_->isVisible() && login_popup_status_) {
            login_popup_status_->setText(error);
            login_popup_status_->show();
            login_popup_->adjustSize();
        } else {
            setStatus(error, true);
        }
    }
    if (request == Request::devices && std::exchange(add_device_after_refresh_, false)) {
        addLocalDevice();
    }
    updateControls();
}

void AccountWindow::signOut() {
    if (!p2p_account_signed_in(account_.get())) return;
#if defined(Q_OS_LINUX)
    if (service_operation_pending_) return;
    if (service_info_.cloud_enabled && !service_info_.device_id.isEmpty() &&
        service_info_.device_id == winText(p2p_account_device_id(account_.get()))) {
        changeLinuxCloudReceiving(false, [this](bool stopped) { if (stopped) signOut(); });
        return;
    }
#endif
    refresh_.stop();
    p2p_account_cancel(account_.get());
    request_ = Request::none;
    add_device_after_refresh_ = false;
#if defined(Q_OS_WIN)
    stopReceiving();
#endif
    active_credentials_.reset();
    if (p2p_account_logout(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()), true);
    } else {
        request_ = Request::logout;
        setStatus(QStringLiteral("正在退出账号…"));
    }
    selected_ = {};
    fillDevices();
    loaded_server_.clear();
    loadCredentials();
    updateControls();
}

void AccountWindow::fillDevices() {
    if (!cloud_list_) return;
    const QSignalBlocker block(cloud_list_);
    cloud_list_->clear();

    if (!p2p_account_signed_in(account_.get())) {
        auto* hint = new QListWidgetItem(QStringLiteral("登录后可查看云端设备"), cloud_list_);
        hint->setFlags(Qt::NoItemFlags);
        viewing_local_ = true;
        selected_ = {};
        if (local_nav_btn_) local_nav_btn_->setChecked(true);
        right_stack_->setCurrentWidget(local_page_);
        return;
    }

    const auto local = winText(p2p_account_device_id(account_.get()));
    const auto count = p2p_account_device_count(account_.get());
    if (count == 0) {
        auto* empty = new QListWidgetItem(QStringLiteral("暂无已绑定设备"), cloud_list_);
        empty->setFlags(Qt::NoItemFlags);
        viewing_local_ = true;
        selected_ = {};
        if (local_nav_btn_) local_nav_btn_->setChecked(true);
        right_stack_->setCurrentWidget(local_page_);
        return;
    }

    QListWidgetItem* to_select{};
    for (size_t i = 0; i < count; ++i) {
        p2p_device device{};
        if (p2p_account_device(account_.get(), i, &device) != P2P_OK) continue;
        const auto id = winText(device.id);
        const auto state = winText(device.state);
        const auto name = winText(device.name);
        const auto platform = winText(device.platform);
        const bool is_self = id == local
            || (service_info_.running && service_info_.cloud_enabled && id == service_info_.device_id);
        const auto label = is_self ? QStringLiteral("%1 (本机)").arg(name) : name;

        auto* item = new QListWidgetItem(makeSidebarDeviceIcon(state, false), label, cloud_list_);
        item->setData(Qt::UserRole, id);
        item->setData(device_state_role, state);
        item->setData(device_name_role, name);
        item->setData(device_platform_role, platform);
        if (!viewing_local_ && selected_.valid() && selected_.id == id) {
            to_select = item;
            selected_.name = name;
            selected_.state = state;
            selected_.platform = platform;
            selected_.is_self = is_self;
        }
    }

    if (to_select) {
        cloud_list_->setCurrentItem(to_select);
        if (local_nav_btn_) local_nav_btn_->setChecked(false);
        right_stack_->setCurrentWidget(detail_page_);
        updateDetailView();
    } else {
        viewing_local_ = true;
        selected_ = {};
        cloud_list_->clearSelection();
        cloud_list_->setCurrentRow(-1);
        if (local_nav_btn_) local_nav_btn_->setChecked(true);
        right_stack_->setCurrentWidget(local_page_);
    }
}

#if defined(Q_OS_WIN)
void AccountWindow::refreshScreens() {
    if ((host_ && !host_->stopped()) || (lan_host_ && !lan_host_->stopped())) return;
    const auto previous = screens_->currentData();
    std::vector<Monitor> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, enumMonitors, reinterpret_cast<LPARAM>(&monitors))) return;
    screens_->clear();
    int primary{};
    for (const auto& monitor : monitors) {
        if (monitor.primary) primary = screens_->count();
        const auto label = QStringLiteral("显示器 %1 · %2 × %3").arg(screens_->count() + 1)
            .arg(monitor.size.width()).arg(monitor.size.height());
        const auto screen_data = QVariant::fromValue(static_cast<qulonglong>(reinterpret_cast<quintptr>(monitor.handle)));
        screens_->addItem(label, screen_data);
    }
    const auto match = screens_->findData(previous);
    screens_->setCurrentIndex(match >= 0 ? match : primary);
    updateControls();
}

void AccountWindow::toggleReceiving() {
    if (host_) stopReceiving();
    else startReceiving();
}

void AccountWindow::startReceiving() {
    if (!p2p_account_device_id(account_.get()) || host_
        || (service_info_.running && service_info_.cloud_enabled) || !screens_->count()) return;
    const auto screen = reinterpret_cast<void*>(static_cast<quintptr>(screens_->currentData().toULongLong()));
    const auto generation = ++host_generation_;
    QString error;
    auto session = std::make_unique<HostSession>(account_.get(), screen, options_,
        [this, generation](int state, const QString& status, bool desktop_control) {
            QMetaObject::invokeMethod(this, [this, generation, state, status, desktop_control] {
                if (generation != host_generation_ || !host_) return;
                host_status_text_ = status;
                if (desktop_control) {
                    host_status_text_ += hotkey_ ? QStringLiteral(" · Ctrl+Alt+F11 可停止")
                        : QStringLiteral(" · 可在菜单或托盘停止");
                }
                if (cloud_host_status_) cloud_host_status_->setText(host_status_text_);
                if (state == P2P_STOPPED) {
                    stopReceiving();
                    setStatus(status);
                }
            }, Qt::QueuedConnection);
        }, [this, generation, screen](std::uint32_t id, int tool) {
            QMetaObject::invokeMethod(this, [this, generation, id, tool, screen] {
                showAgentApproval(id, tool, generation, screen);
            }, Qt::QueuedConnection);
        }, &error);
    if (!session->valid()) {
        setStatus(error);
        return;
    }
    if (!hotkey_) {
        hotkey_ = RegisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey,
            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11) != FALSE;
    }
    host_ = std::move(session);
    updateControls();
}

void AccountWindow::stopReceiving() {
    ++host_generation_;
    clearAgentApproval();
    host_.reset();
    host_status_text_.clear();
    if (hotkey_ && !lan_host_) {
        UnregisterHotKey(reinterpret_cast<HWND>(winId()), stop_hotkey);
        hotkey_ = false;
    }
    updateControls();
}

std::uint32_t AccountWindow::clearAgentApproval() {
    const auto previous = std::exchange(agent_prompt_id_, 0);
    if (auto* prompt = agent_prompt_.data()) {
        prompt->hide();
        prompt->deleteLater();
        agent_prompt_.clear();
    }
    return previous;
}

void AccountWindow::showAgentApproval(std::uint32_t id, int tool, std::uint64_t generation, void* monitor) {
    if (generation != host_generation_ || !host_ || id == agent_prompt_id_) return;
    const auto previous = clearAgentApproval();
    if (previous) host_->answerAgent(previous, false);
    if (!id) return;
    const bool screen = tool == P2P_AGENT_SCREEN_SNAPSHOT;
    auto* prompt = new QMessageBox(QMessageBox::Question, QStringLiteral("AI Agent 诊断"), screen
        ? QStringLiteral("已连接的主控请求截取当前共享显示器的一帧。是否批准？")
        : QStringLiteral("已连接的主控请求查看本机系统摘要。是否批准本次请求？"), QMessageBox::NoButton, this);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    prompt->setWindowFlag(Qt::WindowStaysOnTopHint);
    prompt->setInformativeText(screen
        ? QStringLiteral("批准后，将把所选显示器当前可见内容发送给主控端。请确认屏幕上可见的内容可以共享。")
        : QStringLiteral("摘要包括系统版本、硬件、开机时长、系统盘容量、主机名和被控程序运行账户及权限。"
        "\n批准仅对本次生效，未确认将自动拒绝。确认期间本会话的远程键鼠暂停。"));
    auto* approve = prompt->addButton(QStringLiteral("批准本次"), QMessageBox::AcceptRole);
    auto* reject = prompt->addButton(QStringLiteral("拒绝"), QMessageBox::RejectRole);
    prompt->setDefaultButton(reject);
    prompt->setEscapeButton(reject);
    agent_prompt_ = prompt;
    agent_prompt_id_ = id;
    connect(prompt, &QMessageBox::finished, this, [this, generation, id, prompt, approve, screen, monitor] {
        if (generation != host_generation_ || agent_prompt_id_ != id || !host_) return;
        const bool approved = prompt->clickedButton() == approve;
        clearAgentApproval();
        host_->answerAgent(id, approved, approved && screen ? captureAgentScreen(monitor) : QByteArray{});
    });
    prompt->show();
    prompt->raise();
}

void AccountWindow::stopAgentDiagnostics() {
    const bool saved = AppSettings::savePluginConfig(QStringLiteral("ai-agent"),
        {{QStringLiteral("allow_host_access"), false}, {QStringLiteral("allow_screen_snapshot"), false}}, true);
    if (host_) {
        host_->revokeAgent(!saved);
        if (const auto id = clearAgentApproval()) host_->answerAgent(id, false);
    }
    const auto message = saved ? QStringLiteral("已暂停 AI Agent 诊断。可在设置中重新开启。")
        : QStringLiteral("无法保存 AI Agent 授权设置。");
    if (saved) setStatus(message);
    else QMessageBox::warning(this, QStringLiteral("AI Agent 诊断"), message);
}

void AccountWindow::openAgentAudit() {
    const auto directory = AppSettings::pluginConfigDirectory();
    QVector<QJsonObject> records;
    if (!directory.isEmpty()) {
        const QDir logs(directory);
        for (const auto& name : {QStringLiteral("ai-agent.audit.previous.jsonl"), QStringLiteral("ai-agent.audit.jsonl")}) {
            QFile file(logs.filePath(name));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) continue;
            for (const auto& line : file.read(256 * 1024).split('\n')) {
                const auto event = QJsonDocument::fromJson(line).object();
                if (event.value(QStringLiteral("tool")).toString() == QStringLiteral("system_info")
                    || event.value(QStringLiteral("tool")).toString() == QStringLiteral("screen_snapshot")) {
                    records.append(event);
                }
            }
        }
    }
    auto* dialog = new FramelessDialog(QStringLiteral("AI Agent 诊断记录"), true, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(650, 370);
    const auto count = std::min<qsizetype>(records.size(), 200);
    if (!count) {
        auto* empty = new QLabel(QStringLiteral("暂无本机诊断记录。"), dialog);
        empty->setObjectName(QStringLiteral("muted"));
        empty->setAlignment(Qt::AlignCenter);
        dialog->contentLayout()->addStretch(1);
        dialog->contentLayout()->addWidget(empty);
        dialog->contentLayout()->addStretch(1);
    } else {
        auto* table = new QTableWidget(static_cast<int>(count), 5, dialog);
        table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("请求"), QStringLiteral("操作"), QStringLiteral("结果"),
            QStringLiteral("耗时 (ms)")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->hide();
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        const QHash<QString, QString> outcomes{
            {QStringLiteral("approved"), QStringLiteral("已批准")},
            {QStringLiteral("denied"), QStringLiteral("已拒绝")},
            {QStringLiteral("expired"), QStringLiteral("超时")},
            {QStringLiteral("revoked"), QStringLiteral("已撤销")},
            {QStringLiteral("cancelled"), QStringLiteral("主控已取消")},
            {QStringLiteral("disconnected"), QStringLiteral("会话已断开")},
            {QStringLiteral("unavailable"), QStringLiteral("不可用")}
        };
        for (int row = 0; row < count; ++row) {
            const auto& record = records[records.size() - 1 - row];
            const auto date = QDateTime::fromString(record.value(QStringLiteral("utc")).toString(), Qt::ISODate).toLocalTime();
            table->setItem(row, 0, new QTableWidgetItem(date.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            table->setItem(row, 1, new QTableWidgetItem(QString::number(record.value(QStringLiteral("id")).toInteger())));
            table->setItem(row, 2, new QTableWidgetItem(record.value(QStringLiteral("tool")).toString() ==
                QStringLiteral("screen_snapshot") ? QStringLiteral("屏幕截图") : QStringLiteral("系统摘要")));
            table->setItem(row, 3, new QTableWidgetItem(outcomes.value(record.value(QStringLiteral("result")).toString(),
                QStringLiteral("未知"))));
            table->setItem(row, 4, new QTableWidgetItem(QString::number(record.value(QStringLiteral("elapsed_ms")).toInteger())));
        }
        dialog->contentLayout()->addWidget(table);
    }
    dialog->show();
}

void AccountWindow::refreshServiceStatus() {
    auto status = queryHostServiceInfo();
    if (status.running != service_info_.running || status.installed != service_info_.installed) {
        ++service_status_generation_;
        service_status_age_.invalidate();
        service_info_ = status;
        updateControls();
    } else {
        updateLocalAddresses();
    }
    if (service_info_.running && service_info_.cloud_enabled && host_) {
        stopReceiving();
    }
    if (service_info_.running && service_info_.lan_enabled && lan_host_) {
        stopLanReceiving();
    }
    if (!status.running || service_status_pending_) return;
    service_status_pending_ = true;
    const auto generation = service_status_generation_;
    service_status_thread_ = std::jthread([this, generation, status = std::move(status)]() mutable {
        auto info = queryHostWorkerStatus(std::move(status));
        QMetaObject::invokeMethod(this, [this, generation, info = std::move(info)]() mutable {
            service_status_pending_ = false;
            if (generation != service_status_generation_) return;
            if (info.pipe_connected) {
                service_status_age_.start();
            } else if (service_status_age_.isValid() && !service_status_age_.hasExpired(service_status_grace_ms)) {
                return;
            }
            const bool status_changed = service_info_.status != info.status
                || service_info_.lan_status != info.lan_status
                || service_info_.cloud_enabled != info.cloud_enabled
                || service_info_.lan_enabled != info.lan_enabled
                || service_info_.state != info.state
                || service_info_.desktop_control != info.desktop_control
                || service_info_.lan_state != info.lan_state
                || service_info_.lan_desktop_control != info.lan_desktop_control
                || service_info_.device_id != info.device_id;
            service_info_ = std::move(info);
            if (service_info_.running && service_info_.cloud_enabled && host_) stopReceiving();
            if (service_info_.running && service_info_.lan_enabled && lan_host_) stopLanReceiving();
            if (status_changed) updateControls();
        }, Qt::QueuedConnection);
    });
}

bool AccountWindow::installOrUpdateHostService(bool enable_lan, bool enable_cloud, QString* error) {
    HostServiceConfig config;
    config.monitor_index = std::max(0, screens_ ? screens_->currentIndex() : 0);

    const auto* device_id = p2p_account_device_id(account_.get());
    const bool has_cloud = enable_cloud && active_credentials_ && device_id;
    if (has_cloud) {
        config.server = active_credentials_->server;
        config.user = active_credentials_->user;
        config.password = active_credentials_->password;
        config.installation_id = winText(p2p_account_installation_id(account_.get()));
        config.device_id = winText(device_id);
        config.compatible_http = options_.compatible_http;
        config.relay_only = options_.relay_only;
    }

    if (enable_lan || !has_cloud) {
        if (lan_host_code_.size() < 6 || lan_host_code_.size() > 32) {
            lan_host_code_ = generateDefaultAccessCode();
            saveLanSecret(QStringLiteral("HostAccessCode"), lan_host_code_);
        }
        config.lan_enabled = true;
        config.lan_port = lan_host_port_ ? lan_host_port_ : static_cast<std::uint16_t>(P2P_LAN_DEFAULT_PORT);
        config.lan_access_code = lan_host_code_.toStdWString();
    }

    const bool had_cloud = host_ != nullptr;
    const bool had_lan = lan_host_ != nullptr;
    if (has_cloud) stopReceiving();
    if (config.lan_enabled) stopLanReceiving();
    setStatus(QStringLiteral("正在安装并启动系统服务（请在弹出的 UAC 窗口确认）…"));

    if (!installHostService(config, error)) {
        refreshServiceStatus();
        if (had_cloud && (!service_info_.running || !service_info_.cloud_enabled) && p2p_account_device_id(account_.get())) {
            startReceiving();
        }
        if (had_lan && (!service_info_.running || !service_info_.lan_enabled)) {
            startLanReceiving(lan_host_code_, lan_host_port_);
        }
        updateControls();
        return false;
    }
    refreshServiceStatus();
    updateControls();
    refresh();
    return true;
}

void AccountWindow::toggleHostService() {
    QString error;
    if (service_info_.installed) {
        const bool was_lan = service_info_.lan_enabled;
        const bool was_cloud = service_info_.cloud_enabled;
        setStatus(QStringLiteral("正在停止并卸载系统服务…"));
        if (!uninstallHostService(&error)) {
            setStatus(error);
            return;
        }
        refreshServiceStatus();
        if (was_cloud && p2p_account_device_id(account_.get()) && !service_info_.running) {
            startReceiving();
        }
        if (was_lan && !service_info_.running && lan_host_code_.size() >= 6 && lan_host_code_.size() <= 32) {
            startLanReceiving(lan_host_code_, lan_host_port_);
        }
        setStatus(QStringLiteral("已卸载系统服务，已恢复绿色便携接收模式。"));
        updateControls();
        return;
    }

    const bool has_cloud = active_credentials_ && p2p_account_device_id(account_.get()) != nullptr;
    const bool want_lan = lan_host_ != nullptr || !has_cloud;
    if (!installOrUpdateHostService(want_lan, has_cloud, &error)) {
        setStatus(error);
        return;
    }
    setStatus(QStringLiteral("系统服务已启动并启用开机自启（SYSTEM 权限）。"));
}

void AccountWindow::relaunchAsAdmin() {
    QString error;
    if (!relaunchClientElevated(&error)) {
        setStatus(error);
        return;
    }
    quit();
}
#else
void AccountWindow::toggleReceiving() {
    if (service_operation_pending_) return;
    if (service_info_.cloud_enabled) changeLinuxCloudReceiving(false);
    else if (p2p_account_device_id(account_.get())) changeLinuxCloudReceiving(true);
}

void AccountWindow::changeLinuxCloudReceiving(bool enabled, std::function<void(bool)> completed) {
    if (service_operation_pending_) {
        if (completed) completed(false);
        return;
    }
    std::optional<HostServiceConfig> config;
    if (enabled) {
        const auto* device = p2p_account_device_id(account_.get());
        const auto* installation = p2p_account_installation_id(account_.get());
        if (!active_credentials_ || !device || !installation || !service_info_.cloud_installed) {
            setStatus(QStringLiteral("请先添加本机并安装云端被控服务。"));
            if (completed) completed(false);
            return;
        }
        config.emplace();
        config->server = active_credentials_->server;
        config->user = active_credentials_->user;
        config->password = active_credentials_->password;
        config->installation_id = winText(installation);
        config->device_id = winText(device);
        config->relay_only = options_.relay_only;
    }
    const auto device = config ? config->device_id : QString{};
    service_operation_pending_ = true;
    ++service_status_generation_;
    updateControls();
    manageLinuxCloudService(this, config, [this, enabled, device, completed = std::move(completed)](const QString& error) {
        service_operation_pending_ = false;
        if (error.isEmpty()) {
            service_info_.cloud_enabled = enabled;
            service_info_.cloud_online = false;
            service_info_.device_id = device;
            service_info_.status = enabled ? QStringLiteral("正在连接云端服务器…") : QString{};
            setStatus(enabled ? QStringLiteral("已启用开机云端接收") : QStringLiteral("已停止云端接收"));
        } else {
            setStatus(error, true);
        }
        updateControls();
        refreshServiceStatus();
        if (completed) completed(error.isEmpty());
    });
}

void AccountWindow::refreshServiceStatus() {
    if (service_status_pending_ || service_operation_pending_) return;
    const auto generation = service_status_generation_;
    service_status_pending_ = true;
    service_status_thread_ = std::jthread([this, generation] {
        const auto info = queryLinuxHostServiceInfo();
        QMetaObject::invokeMethod(this, [this, generation, info = std::move(info)] {
            service_status_pending_ = false;
            if (generation != service_status_generation_) {
                refreshServiceStatus();
                return;
            }
            if (!info || *info == service_info_) {
                updateLocalAddresses();
                return;
            }
            if (info->lan_enabled) lan_host_port_ = info->lan_port;
            const bool online_changed = service_info_.cloud_online != info->cloud_online;
            service_info_ = *info;
            updateControls();
            if (online_changed) refresh();
        }, Qt::QueuedConnection);
    });
}
#endif

void AccountWindow::closeSessions() {
    refresh_.stop();
    status_clear_timer_.stop();
    delete viewer_.data();
#if defined(Q_OS_WIN)
    stopReceiving();
    stopLanReceiving();
#endif
}
