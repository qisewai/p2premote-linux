#include "account_window.h"
#include "app_settings.h"
#include "app_theme.h"
#include "frameless_chrome.h"
#include "settings_dialog.h"

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
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QVBoxLayout>

#include <windows.h>
#include <dwmapi.h>

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

constexpr int stop_hotkey = 1;
constexpr int device_state_role = Qt::UserRole + 1;
constexpr int device_name_role = Qt::UserRole + 2;
constexpr int lan_port_role = Qt::UserRole + 3;
constexpr int device_platform_role = Qt::UserRole + 4;

QString winText(const wchar_t* text) { return text ? QString::fromWCharArray(text) : QString{}; }

QLabel* heading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("heading"));
    label->setTextFormat(Qt::PlainText);
    return label;
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

} // namespace

class ToggleSwitch final : public QAbstractButton {
public:
    explicit ToggleSwitch(const QString& label, QWidget* parent = nullptr)
        : QAbstractButton(parent), label_(label) {
        setCheckable(true);
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(26);
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

        const qreal track_w = 34.0;
        const qreal track_h = 18.0;
        const qreal track_y = (height() - track_h) / 2.0;
        const QRectF track(width() - track_w - 2.0, track_y, track_w, track_h);

        QFont f = font();
        f.setPixelSize(std::max(12, theme.font_size - 1));
        f.setWeight(isChecked() ? QFont::DemiBold : QFont::Normal);
        p.setFont(f);
        p.setPen(QColor(isChecked() || hovered_ ? theme.text : theme.text_secondary));
        p.drawText(QRectF(4.0, 0.0, track.left() - 10.0, height()),
            Qt::AlignVCenter | Qt::AlignRight, label_);

        QColor track_color = isChecked()
            ? QColor(hovered_ ? theme.accent_hover : theme.accent)
            : (theme.is_dark ? QColor(255, 255, 255, hovered_ ? 55 : 36) : QColor(0, 0, 0, hovered_ ? 58 : 40));
        p.setPen(Qt::NoPen);
        p.setBrush(track_color);
        p.drawRoundedRect(track, track_h / 2.0, track_h / 2.0);

        const qreal knob = 14.0;
        const qreal kx = isChecked() ? (track.right() - knob - 2.0) : (track.left() + 2.0);
        const qreal ky = track.top() + (track_h - knob) / 2.0;
        p.setBrush(QColor(Qt::white));
        p.drawEllipse(QRectF(kx, ky, knob, knob));
    }

private:
    void updateWidth() {
        const int text_w = fontMetrics().horizontalAdvance(label_);
        setFixedWidth(text_w + 50);
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

AccountWindow::AccountWindow(LaunchOptions options)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), options_(std::move(options)) {
    account_.reset(p2p_account_create());
    setWindowTitle(QStringLiteral("P2P Remote"));
    resize(920, 620);
    setMinimumSize(780, 520);

    lan_host_code_ = readLanSecret(QStringLiteral("HostAccessCode"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* shell = new QFrame(this);
    shell->setObjectName(QStringLiteral("framelessShell"));
    auto* shell_layout = new QVBoxLayout(shell);
    shell_layout->setContentsMargins(1, 1, 1, 1);
    shell_layout->setSpacing(0);

    // Top TitleBar with Unattended Mode toggle switch + Account button
    title_bar_ = new TitleBar(this, QStringLiteral("P2P Remote"), true, true, shell);

    unattended_switch_ = new ToggleSwitch(QStringLiteral("无人值守"), title_bar_);
    unattended_switch_->setToolTip(QStringLiteral("开启后本机进入局域网无人值守模式并显示固定被控码"));
    connect(unattended_switch_, &QAbstractButton::toggled, this, &AccountWindow::setUnattendedEnabled);
    title_bar_->addToolWidget(unattended_switch_);

    auto* sep = new QFrame(title_bar_);
    sep->setObjectName(QStringLiteral("titleSeparator"));
    title_bar_->addToolWidget(sep);

    account_btn_ = new QPushButton(QStringLiteral("登录账号"), title_bar_);
    account_btn_->setObjectName(QStringLiteral("titleToolBtn"));
    account_btn_->setFocusPolicy(Qt::NoFocus);
    connect(account_btn_, &QPushButton::clicked, this, [this] {
        if (p2p_account_signed_in(account_.get())) showAccountMenu();
        else showLoginPage();
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
    right_layout->setContentsMargins(26, 18, 26, 14);
    right_layout->setSpacing(14);

    right_layout->addWidget(makeUnattendedBanner(right_pane));

    right_stack_ = new QStackedWidget(right_pane);
    detail_page_ = makeDetailPage(right_stack_);
    welcome_page_ = makeWelcomePage(right_stack_);
    login_page_ = makeLoginPage(right_stack_);
    right_stack_->addWidget(detail_page_);
    right_stack_->addWidget(welcome_page_);
    right_stack_->addWidget(login_page_);
    right_stack_->setCurrentWidget(welcome_page_);
    right_layout->addWidget(right_stack_, 1);

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

    account_event_.setHandle(p2p_account_event(account_.get()));
    account_event_.setEnabled(true);
    connect(&account_event_, &QWinEventNotifier::activated, this, &AccountWindow::completeRequest);
    refresh_.setInterval(3000);
    connect(&refresh_, &QTimer::timeout, this, &AccountWindow::refresh);

    // Always run lightweight LAN auto-discovery so nearby hosts show up immediately in sidebar
    lan_scanner_.reset(p2p_lan_scanner_create());
    if (lan_scanner_) p2p_lan_scanner_probe(lan_scanner_.get());
    lan_scan_timer_.setInterval(2000);
    connect(&lan_scan_timer_, &QTimer::timeout, this, &AccountWindow::refreshLanPeers);
    lan_scan_timer_.start();

    connect(&AppSettings::instance(), &AppSettings::changed, this, [this](const AppSettingsData&) {
        refreshLanPeers();
        fillDevices();
        if (unattended_switch_) unattended_switch_->update();
        if (hero_widget_) hero_widget_->update();
    });

    makeTray();
    elevated_ = isProcessElevated();
    refreshServiceStatus();
    connect(&service_refresh_, &QTimer::timeout, this, &AccountWindow::refreshServiceStatus);
    service_refresh_.start(1500);
    refreshScreens();
    loadCredentials();
    refreshLanPeers();
    updateControls();
}

AccountWindow::~AccountWindow() {
    service_refresh_.stop();
    if (service_status_thread_.joinable()) service_status_thread_.join();
    account_event_.setEnabled(false);
    closeSessions();
}

QWidget* AccountWindow::makeSidebar(QWidget* parent) {
    auto* sidebar = new QFrame(parent);
    sidebar->setObjectName(QStringLiteral("navSidebar"));
    sidebar->setFixedWidth(232);

    auto* root = new QVBoxLayout(sidebar);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* nav_body = new QWidget(sidebar);
    auto* nav = new QVBoxLayout(nav_body);
    nav->setContentsMargins(10, 12, 10, 10);
    nav->setSpacing(6);

    // Section 1: 局域网设备 (auto-discovery)
    auto* lan_hdr = new QHBoxLayout;
    lan_hdr->setContentsMargins(0, 0, 0, 0);
    auto* lan_lbl = new QLabel(QStringLiteral("局域网设备"), nav_body);
    lan_lbl->setObjectName(QStringLiteral("sidebarSection"));
    lan_hdr->addWidget(lan_lbl, 1);
    auto* lan_refresh = new QPushButton(QStringLiteral("↻"), nav_body);
    lan_refresh->setObjectName(QStringLiteral("sidebarIconBtn"));
    lan_refresh->setToolTip(QStringLiteral("重新扫描局域网设备"));
    lan_refresh->setFocusPolicy(Qt::NoFocus);
    connect(lan_refresh, &QPushButton::clicked, this, [this] {
        if (lan_scanner_) p2p_lan_scanner_probe(lan_scanner_.get());
        refreshLanPeers();
        setStatus(QStringLiteral("正在扫描局域网在线设备…"));
    });
    lan_hdr->addWidget(lan_refresh);
    nav->addLayout(lan_hdr);

    lan_list_ = new QListWidget(nav_body);
    lan_list_->setObjectName(QStringLiteral("sidebarList"));
    lan_list_->setIconSize(QSize(28, 18));
    lan_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lan_list_->setMaximumHeight(185);
    nav->addWidget(lan_list_);

    // Section 2: 我的设备 (Cloud account)
    auto* cloud_hdr = new QHBoxLayout;
    cloud_hdr->setContentsMargins(0, 8, 0, 0);
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

    cloud_login_sidebar_btn_ = new QPushButton(QStringLiteral("登录云端账号…"), nav_body);
    cloud_login_sidebar_btn_->setObjectName(QStringLiteral("sidebarActionBtn"));
    cloud_login_sidebar_btn_->setFocusPolicy(Qt::NoFocus);
    connect(cloud_login_sidebar_btn_, &QPushButton::clicked, this, &AccountWindow::showLoginPage);
    nav->addWidget(cloud_login_sidebar_btn_);

    // Section 3: 远程协助 / 直连入口
    auto* direct_lbl = new QLabel(QStringLiteral("远程直连"), nav_body);
    direct_lbl->setObjectName(QStringLiteral("sidebarSection"));
    nav->addSpacing(6);
    nav->addWidget(direct_lbl);

    auto* direct_btn = new QPushButton(QStringLiteral("＋  IP 局域网直连…"), nav_body);
    direct_btn->setObjectName(QStringLiteral("sidebarActionBtn"));
    direct_btn->setFocusPolicy(Qt::NoFocus);
    connect(direct_btn, &QPushButton::clicked, this, [this] { openLanConnectDialog(); });
    nav->addWidget(direct_btn);

    auto* audit_btn = new QPushButton(QStringLiteral("AI Agent 诊断记录"), nav_body);
    audit_btn->setObjectName(QStringLiteral("sidebarActionBtn"));
    audit_btn->setFocusPolicy(Qt::NoFocus);
    connect(audit_btn, &QPushButton::clicked, this, &AccountWindow::openAgentAudit);
    nav->addWidget(audit_btn);

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

    // Selection wiring (mutually exclusive between LAN list & Cloud list)
    connect(lan_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        const QSignalBlocker b(cloud_list_);
        cloud_list_->clearSelection();
        cloud_list_->setCurrentItem(nullptr);
        selected_ = {
            SelectionKind::lan,
            item->data(Qt::UserRole).toString(),
            item->data(device_name_role).toString(),
            item->data(device_state_role).toString(),
            item->data(Qt::UserRole).toString(),
            static_cast<std::uint16_t>(item->data(lan_port_role).toInt()),
            false
        };
        right_stack_->setCurrentWidget(detail_page_);
        updateDetailView();
    });
    connect(lan_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        connectSelectedDevice(false);
    });
    connect(cloud_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        const QSignalBlocker b(lan_list_);
        lan_list_->clearSelection();
        lan_list_->setCurrentItem(nullptr);
        const auto id = item->data(Qt::UserRole).toString();
        const auto local = winText(p2p_account_device_id(account_.get()));
        const bool is_self = !id.isEmpty() &&
            (id == local || (service_info_.running && id == service_info_.device_id));
        selected_ = {
            SelectionKind::cloud,
            id,
            item->data(device_name_role).toString(),
            item->data(device_state_role).toString(),
            item->data(device_platform_role).toString(),
            0,
            is_self
        };
        right_stack_->setCurrentWidget(detail_page_);
        updateDetailView();
    });
    connect(cloud_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
        connectSelectedDevice(false);
    });

    return sidebar;
}

QWidget* AccountWindow::makeUnattendedBanner(QWidget* parent) {
    unattended_banner_ = new QFrame(parent);
    unattended_banner_->setObjectName(QStringLiteral("unattendedBanner"));
    auto* row = new QHBoxLayout(unattended_banner_);
    row->setContentsMargins(14, 10, 12, 10);
    row->setSpacing(10);

    unattended_pill_ = new QLabel(QStringLiteral("● 无人值守"), unattended_banner_);
    unattended_pill_->setObjectName(QStringLiteral("statusPill"));
    unattended_pill_->setProperty("state", QStringLiteral("online"));
    row->addWidget(unattended_pill_);

    unattended_ip_label_ = new QLabel(unattended_banner_);
    unattended_ip_label_->setTextFormat(Qt::PlainText);
    unattended_ip_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(unattended_ip_label_);

    auto* code_title = new QLabel(QStringLiteral("被控码"), unattended_banner_);
    code_title->setObjectName(QStringLiteral("muted"));
    row->addWidget(code_title);

    unattended_code_label_ = new QLabel(unattended_banner_);
    unattended_code_label_->setObjectName(QStringLiteral("codeBadge"));
    unattended_code_label_->setTextFormat(Qt::PlainText);
    unattended_code_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(unattended_code_label_);

    unattended_toggle_code_btn_ = new QPushButton(QStringLiteral("显示"), unattended_banner_);
    unattended_toggle_code_btn_->setObjectName(QStringLiteral("miniGhostBtn"));
    unattended_toggle_code_btn_->setFocusPolicy(Qt::NoFocus);
    connect(unattended_toggle_code_btn_, &QPushButton::clicked, this, [this] {
        lan_code_visible_ = !lan_code_visible_;
        updateUnattendedBanner();
    });
    row->addWidget(unattended_toggle_code_btn_);

    unattended_copy_btn_ = new QPushButton(QStringLiteral("复制"), unattended_banner_);
    unattended_copy_btn_->setObjectName(QStringLiteral("miniGhostBtn"));
    unattended_copy_btn_->setFocusPolicy(Qt::NoFocus);
    connect(unattended_copy_btn_, &QPushButton::clicked, this, &AccountWindow::copyUnattendedInfo);
    row->addWidget(unattended_copy_btn_);

    unattended_edit_btn_ = new QPushButton(QStringLiteral("修改"), unattended_banner_);
    unattended_edit_btn_->setObjectName(QStringLiteral("miniGhostBtn"));
    unattended_edit_btn_->setFocusPolicy(Qt::NoFocus);
    connect(unattended_edit_btn_, &QPushButton::clicked, this, &AccountWindow::editUnattendedCode);
    row->addWidget(unattended_edit_btn_);

    row->addStretch(1);

    screens_ = new QComboBox(unattended_banner_);
    screens_->setAccessibleName(QStringLiteral("共享显示器"));
    screens_->setMaximumWidth(160);
    row->addWidget(screens_);

    unattended_banner_->hide();
    return unattended_banner_;
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
    strip_btn_1_ = makeStripBtn(QStringLiteral("立即直连"));
    strip_btn_2_ = makeStripBtn(QStringLiteral("输入参数连接"));
    strip_btn_3_ = makeStripBtn(QStringLiteral("复制 IP"));

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
        if (selected_.kind == SelectionKind::lan) {
            openLanConnectDialog(selected_.id, selected_.name);
        } else if (selected_.kind == SelectionKind::cloud) {
            connectSelectedDevice(true);
        }
    });
    connect(strip_btn_3_, &QPushButton::clicked, this, [this] {
        if (selected_.kind == SelectionKind::lan) {
            if (auto* cb = QApplication::clipboard()) cb->setText(selected_.id);
            setStatus(QStringLiteral("已复制设备 IP：%1").arg(selected_.id));
        } else if (selected_.kind == SelectionKind::cloud) {
            removeSelectedDevice();
        }
    });
    connect(detail_more_btn_, &QPushButton::clicked, this, [this] {
        auto* menu = new QMenu(detail_more_btn_);
        menu->setObjectName(QStringLiteral("remoteMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        if (selected_.kind == SelectionKind::lan) {
            menu->addAction(QStringLiteral("输入被控码并直连…"), this, [this] {
                openLanConnectDialog(selected_.id, selected_.name);
            });
            menu->addAction(QStringLiteral("复制设备 IP"), this, [this] {
                if (auto* cb = QApplication::clipboard()) cb->setText(selected_.id);
                setStatus(QStringLiteral("已复制设备 IP：%1").arg(selected_.id));
            });
            menu->addAction(QStringLiteral("清除已记住的被控码"), this, [this] {
                deleteLanSecret(QStringLiteral("Peer/") + selected_.id);
                setStatus(QStringLiteral("已清除该设备保存的被控码。"));
            });
        } else if (selected_.kind == SelectionKind::cloud) {
            menu->addAction(QStringLiteral("只读诊断"), this, [this] { connectSelectedDevice(true); })
                ->setEnabled(selected_.state == QStringLiteral("online") && !selected_.is_self);
            menu->addAction(QStringLiteral("移除此设备"), this, &AccountWindow::removeSelectedDevice);
        }
        menu->popup(detail_more_btn_->mapToGlobal(QPoint(0, detail_more_btn_->height() + 4)));
    });

    // Lower section: Quick direct connect + Cloud host status (only shown when signed in)
    auto* quick_title = new QLabel(QStringLiteral("快速操作"), page);
    quick_title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(quick_title);

    auto* quick_row = new QHBoxLayout;
    quick_row->setSpacing(12);
    auto* add_direct = new QPushButton(QStringLiteral("＋  输入 IP 局域网直连"), page);
    add_direct->setObjectName(QStringLiteral("quickAddTile"));
    add_direct->setCursor(Qt::PointingHandCursor);
    add_direct->setFocusPolicy(Qt::NoFocus);
    connect(add_direct, &QPushButton::clicked, this, [this] { openLanConnectDialog(); });
    quick_row->addWidget(add_direct, 1);
    layout->addLayout(quick_row);

    // Compact cloud local-receiver card (visible when signed in and viewing cloud self/devices)
    cloud_host_card_ = new QFrame(page);
    cloud_host_card_->setObjectName(QStringLiteral("accountCard"));
    auto* ch_layout = new QVBoxLayout(cloud_host_card_);
    ch_layout->setContentsMargins(14, 10, 14, 10);
    ch_layout->setSpacing(6);
    auto* ch_top = new QHBoxLayout;
    ch_top->setSpacing(8);
    cloud_host_status_ = new QLabel(cloud_host_card_);
    cloud_host_status_->setObjectName(QStringLiteral("muted"));
    cloud_host_status_->setTextFormat(Qt::PlainText);
    cloud_host_status_->setWordWrap(true);
    ch_top->addWidget(cloud_host_status_, 1);
    add_device_ = new QPushButton(QStringLiteral("添加本机"), cloud_host_card_);
    connect(add_device_, &QPushButton::clicked, this, &AccountWindow::addLocalDevice);
    ch_top->addWidget(add_device_);
    receive_ = new QPushButton(QStringLiteral("开启云端接收"), cloud_host_card_);
    connect(receive_, &QPushButton::clicked, this, &AccountWindow::toggleReceiving);
    ch_top->addWidget(receive_);
    ch_layout->addLayout(ch_top);

    auto* ch_bot = new QHBoxLayout;
    ch_bot->setSpacing(8);
    privilege_status_ = new QLabel(cloud_host_card_);
    privilege_status_->setObjectName(QStringLiteral("muted"));
    privilege_status_->setTextFormat(Qt::PlainText);
    privilege_status_->setWordWrap(true);
    ch_bot->addWidget(privilege_status_, 1);
    elevate_button_ = new QPushButton(QStringLiteral("管理员重启"), cloud_host_card_);
    connect(elevate_button_, &QPushButton::clicked, this, &AccountWindow::relaunchAsAdmin);
    ch_bot->addWidget(elevate_button_);
    service_button_ = new QPushButton(QStringLiteral("启用系统服务"), cloud_host_card_);
    connect(service_button_, &QPushButton::clicked, this, &AccountWindow::toggleHostService);
    ch_bot->addWidget(service_button_);
    ch_layout->addLayout(ch_bot);

    cloud_host_card_->hide();
    layout->addWidget(cloud_host_card_);
    layout->addStretch(1);
    return page;
}

QWidget* AccountWindow::makeWelcomePage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(16);

    auto* hero_box = new QFrame(page);
    hero_box->setObjectName(QStringLiteral("heroContainer"));
    auto* hb = new QVBoxLayout(hero_box);
    hb->setContentsMargins(0, 0, 0, 0);
    hb->setSpacing(0);
    auto* welcome_hero = new DeviceHeroWidget(hero_box);
    welcome_hero->setContent(
        QStringLiteral("选择左侧设备开始控制，或直接通过局域网 IP 连接远程电脑"),
        QStringLiteral("＋  局域网直连"), true, true);
    connect(welcome_hero->actionButton(), &QPushButton::clicked, this, [this] {
        openLanConnectDialog();
    });
    hb->addWidget(welcome_hero);

    auto* bar = new QFrame(hero_box);
    bar->setObjectName(QStringLiteral("heroActionBar"));
    auto* bar_row = new QHBoxLayout(bar);
    bar_row->setContentsMargins(12, 6, 12, 6);
    bar_row->setSpacing(8);
    auto* scan_btn = new QPushButton(QStringLiteral("重新扫描局域网"), bar);
    scan_btn->setObjectName(QStringLiteral("heroStripBtn"));
    connect(scan_btn, &QPushButton::clicked, this, [this] {
        if (lan_scanner_) p2p_lan_scanner_probe(lan_scanner_.get());
        refreshLanPeers();
        setStatus(QStringLiteral("正在扫描局域网在线设备…"));
    });
    welcome_login_btn_ = new QPushButton(QStringLiteral("登录云端账号"), bar);
    welcome_login_btn_->setObjectName(QStringLiteral("heroStripBtn"));
    connect(welcome_login_btn_, &QPushButton::clicked, this, &AccountWindow::showLoginPage);
    bar_row->addWidget(scan_btn, 1);
    auto* sep = new QFrame(bar);
    sep->setObjectName(QStringLiteral("titleSeparator"));
    bar_row->addWidget(sep);
    bar_row->addWidget(welcome_login_btn_, 1);
    hb->addWidget(bar);

    layout->addWidget(hero_box);

    auto* tip = new QLabel(
        QStringLiteral("提示：打开右上角「无人值守」开关即可让本机在局域网内等待直连，并显示本机固定被控码。"), page);
    tip->setObjectName(QStringLiteral("muted"));
    tip->setWordWrap(true);
    layout->addWidget(tip);
    layout->addStretch(1);
    return page;
}

QWidget* AccountWindow::makeLoginPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 4);
    layout->setSpacing(14);

    auto* hdr = new QHBoxLayout;
    hdr->addWidget(heading(QStringLiteral("登录云端账号"), page), 1);
    auto* back = new QPushButton(QStringLiteral("返回"), page);
    back->setObjectName(QStringLiteral("miniGhostBtn"));
    connect(back, &QPushButton::clicked, this, [this] {
        right_stack_->setCurrentWidget(selected_.kind == SelectionKind::none ? welcome_page_ : detail_page_);
    });
    hdr->addWidget(back);
    layout->addLayout(hdr);

    auto* notice = new QLabel(
        QStringLiteral("登录后可跨外网管理账号下的全部设备；纯局域网直连无需登录。"), page);
    notice->setObjectName(QStringLiteral("muted"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto* card = new QFrame(page);
    card->setObjectName(QStringLiteral("accountCard"));
    auto* fields = new QFormLayout(card);
    fields->setContentsMargins(18, 18, 18, 18);
    fields->setHorizontalSpacing(18);
    fields->setVerticalSpacing(12);
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QSettings settings;
    server_ = new QLineEdit(settings.value(QStringLiteral("lastServer"), winText(p2p_default_server())).toString(), card);
    server_->setObjectName(QStringLiteral("server"));
    server_->setMaxLength(256);
    server_->setPlaceholderText(QStringLiteral("https://"));
    user_ = new QLineEdit(card);
    user_->setObjectName(QStringLiteral("user"));
    user_->setMaxLength(32);
    password_ = new QLineEdit(card);
    password_->setObjectName(QStringLiteral("password"));
    password_->setMaxLength(72);
    password_->setEchoMode(QLineEdit::Password);
    remember_ = new QCheckBox(QStringLiteral("记住密码"), card);
    fields->addRow(QStringLiteral("服务地址"), server_);
    fields->addRow(QStringLiteral("账号"), user_);
    fields->addRow(QStringLiteral("密码"), password_);
    fields->addRow(QString{}, remember_);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    login_ = new QPushButton(QStringLiteral("登录"), card);
    login_->setObjectName(QStringLiteral("primary"));
    register_ = new QPushButton(QStringLiteral("注册并登录"), card);
    buttons->addWidget(login_);
    buttons->addWidget(register_);
    buttons->addStretch();
    fields->addRow(QString{}, buttons);
    layout->addWidget(card);
    layout->addStretch(1);

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
        if (!checked && !deleteCredentials(server_->text())) setStatus(QStringLiteral("无法删除 Windows 凭据。"));
    });
    return page;
}

void AccountWindow::makeTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    QApplication::setQuitOnLastWindowClosed(false);
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("打开 P2P Remote"), this, &AccountWindow::restore);
    tray_unattended_ = menu->addAction(QStringLiteral("开启无人值守"), this, [this] {
        if (unattended_switch_) unattended_switch_->toggle();
    });
    tray_receive_ = menu->addAction(QStringLiteral("开启云端接收"), this, &AccountWindow::toggleReceiving);
    menu->addAction(QStringLiteral("IP 局域网直连…"), this, [this] {
        restore();
        openLanConnectDialog();
    });
    menu->addAction(QStringLiteral("AI Agent 诊断记录…"), this, &AccountWindow::openAgentAudit);
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
    const auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == stop_hotkey && (host_ || lan_host_)) {
        if (host_) stopReceiving();
        if (lan_host_) {
            const QSignalBlocker b(unattended_switch_);
            if (unattended_switch_) unattended_switch_->setChecked(false);
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
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
}

void AccountWindow::quit() {
    quitting_ = true;
    close();
    QApplication::quit();
}

void AccountWindow::setStatus(const QString& text) {
    if (!status_) return;
    status_->setText(text);
    status_->setVisible(!text.trimmed().isEmpty());
}

void AccountWindow::showLoginPage() {
    right_stack_->setCurrentWidget(login_page_);
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
    if (!service_info_.running) {
        auto* rec = menu->addAction(host_ ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收"),
            this, &AccountWindow::toggleReceiving);
        rec->setEnabled(has_local && (screens_->count() > 0 || host_));
    }
    if (serviceExecutableExists() || service_info_.installed) {
        auto* svc = menu->addAction(service_info_.installed ? QStringLiteral("卸载系统服务") : QStringLiteral("启用系统服务"),
            this, &AccountWindow::toggleHostService);
        svc->setEnabled(has_local || service_info_.installed);
    }
    if (!elevated_ && !service_info_.running) {
        menu->addAction(QStringLiteral("以管理员重启"), this, &AccountWindow::relaunchAsAdmin);
    }
    menu->addSeparator();
    menu->addAction(QStringLiteral("暂停 AI Agent 诊断"), this, &AccountWindow::stopAgentDiagnostics);
    menu->addSeparator();
    menu->addAction(QStringLiteral("退出账号"), this, &AccountWindow::signOut);

    const int x = account_btn_->width() - menu->sizeHint().width();
    menu->popup(account_btn_->mapToGlobal(QPoint(x, account_btn_->height() + 4)));
}

void AccountWindow::updateUnattendedBanner() {
    if (!unattended_banner_) return;
    const bool active = lan_host_ != nullptr;
    unattended_banner_->setVisible(active);
    if (!active) return;

    const auto ips = winText(p2p_lan_local_addresses());
    const auto first_ip = ips.section(QStringLiteral(","), 0, 0).trimmed();
    unattended_ip_label_->setText(first_ip.isEmpty()
        ? QStringLiteral("本机 IP：未检测到局域网")
        : QStringLiteral("本机 IP：%1").arg(first_ip));

    if (lan_host_controlled_) {
        unattended_pill_->setText(QStringLiteral("● 被控中"));
        unattended_pill_->setProperty("state", QStringLiteral("busy"));
    } else {
        unattended_pill_->setText(QStringLiteral("● 无人值守"));
        unattended_pill_->setProperty("state", QStringLiteral("online"));
    }
    unattended_pill_->style()->unpolish(unattended_pill_);
    unattended_pill_->style()->polish(unattended_pill_);

    unattended_code_label_->setText(lan_code_visible_
        ? lan_host_code_ : QString(std::max<qsizetype>(6, lan_host_code_.size()), QChar(0x2022)));
    unattended_toggle_code_btn_->setText(lan_code_visible_ ? QStringLiteral("隐藏") : QStringLiteral("显示"));
    screens_->setVisible(screens_->count() > 1);
    screens_->setEnabled(false);
}

void AccountWindow::updateDetailView() {
    if (selected_.kind == SelectionKind::none) {
        if (right_stack_->currentWidget() == detail_page_) {
            right_stack_->setCurrentWidget(welcome_page_);
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

    if (selected_.kind == SelectionKind::lan) {
        const bool has_saved_code = !readLanSecret(QStringLiteral("Peer/") + selected_.id).isEmpty();
        const QString subtitle = busy
            ? QStringLiteral("局域网设备正被控制 · %1").arg(selected_.id)
            : QStringLiteral("局域网直连就绪 · %1").arg(selected_.id);
        const QString cta = has_saved_code ? QStringLiteral("开始远程控制") : QStringLiteral("输入被控码并连接");
        hero_widget_->setContent(subtitle, cta, !busy, !busy);
        strip_btn_1_->setText(QStringLiteral("远程控制"));
        strip_btn_1_->setEnabled(!busy);
        strip_btn_2_->setText(QStringLiteral("修改连接参数"));
        strip_btn_2_->setEnabled(true);
        strip_btn_3_->setText(QStringLiteral("复制 IP"));
        strip_btn_3_->setEnabled(true);
    } else {
        const bool can_connect = online && !selected_.is_self;
        QString subtitle = selected_.subtitle;
        if (selected_.is_self) subtitle += QStringLiteral(" · 本机设备");
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
}

void AccountWindow::updateControls() {
    if (!account_) return;
    const auto* local = p2p_account_device_id(account_.get());
    const bool signed_in = p2p_account_signed_in(account_.get()) != 0;
    const bool busy = request_ != Request::none;

    if (account_btn_) {
        account_btn_->setText(signed_in && active_credentials_
            ? QStringLiteral("%1 ▾").arg(active_credentials_->user)
            : QStringLiteral("登录账号"));
    }
    if (cloud_refresh_btn_) {
        cloud_refresh_btn_->setVisible(signed_in);
        cloud_refresh_btn_->setEnabled(!busy);
    }
    if (cloud_list_) cloud_list_->setVisible(signed_in);
    if (cloud_login_sidebar_btn_) cloud_login_sidebar_btn_->setVisible(!signed_in);
    if (welcome_login_btn_) welcome_login_btn_->setVisible(!signed_in);
    if (login_page_) login_page_->setEnabled(!busy);

    const bool service_active = service_info_.running;
    if (cloud_host_card_) {
        cloud_host_card_->setVisible(signed_in && selected_.kind == SelectionKind::cloud);
    }
    if (add_device_) {
        add_device_->setVisible(!local);
        add_device_->setEnabled(signed_in && !busy);
    }
    if (receive_) {
        receive_->setVisible(!service_active);
        receive_->setEnabled(local && (screens_->count() || host_));
        receive_->setText(host_ ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收"));
    }
    if (cloud_host_status_) {
        if (service_active) {
            cloud_host_status_->setText(service_info_.status.isEmpty()
                ? QStringLiteral("[系统服务] 后台已启动") : service_info_.status);
        } else if (host_) {
            cloud_host_status_->setText(host_status_text_.isEmpty()
                ? QStringLiteral("本机云端接收运行中") : host_status_text_);
        } else {
            cloud_host_status_->setText(local ? QStringLiteral("云端接收已暂停")
                : QStringLiteral("添加本机后可开启云端接收"));
        }
    }
    if (service_button_) {
        const bool has_service_exe = serviceExecutableExists();
        service_button_->setVisible(has_service_exe || service_info_.installed);
        service_button_->setText(service_info_.installed ? QStringLiteral("卸载系统服务") : QStringLiteral("启用系统服务"));
        service_button_->setEnabled(local || service_info_.installed);
    }
    if (elevate_button_) elevate_button_->setVisible(!elevated_ && !service_active);
    if (privilege_status_) {
        if (service_active) {
            privilege_status_->setText(QStringLiteral("SYSTEM 系统服务已托管 · 支持 UAC / 锁屏 / 开机自启"));
        } else if (elevated_) {
            privilege_status_->setText(QStringLiteral("管理员权限 · 支持任务管理器"));
        } else {
            privilege_status_->setText(QStringLiteral("普通权限 · 如需 UAC / 锁屏支持请启用系统服务"));
        }
    }

    if (tray_unattended_) {
        tray_unattended_->setText(lan_host_ ? QStringLiteral("关闭无人值守") : QStringLiteral("开启无人值守"));
    }
    if (tray_receive_) {
        tray_receive_->setVisible(signed_in);
        tray_receive_->setText(service_active ? QStringLiteral("云端接收由系统服务托管")
            : (host_ ? QStringLiteral("停止云端接收") : QStringLiteral("开启云端接收")));
        tray_receive_->setEnabled(signed_in && !service_active && local);
    }

    updateUnattendedBanner();
    updateDetailView();
}

void AccountWindow::setUnattendedEnabled(bool enabled) {
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
    const QSignalBlocker b(unattended_switch_);
    if (unattended_switch_) unattended_switch_->setChecked(true);
    startLanReceiving(lan_host_code_, lan_host_port_);
}

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

void AccountWindow::startLanReceiving(const QString& override_code, std::uint16_t port) {
    if (lan_host_ || !screens_ || !screens_->count()) return;
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
                updateUnattendedBanner();
                if (state == P2P_STOPPED) {
                    const QSignalBlocker b(unattended_switch_);
                    if (unattended_switch_) unattended_switch_->setChecked(false);
                    stopLanReceiving();
                    setStatus(status);
                }
            }, Qt::QueuedConnection);
        }, &error);
    if (!session->valid()) {
        const QSignalBlocker b(unattended_switch_);
        if (unattended_switch_) unattended_switch_->setChecked(false);
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

void AccountWindow::editUnattendedCode() {
    FramelessDialog dialog(QStringLiteral("修改固定被控码"), false, this);
    dialog.resize(360, 180);
    auto* form = new QFormLayout;
    form->setSpacing(10);
    auto* edit = new QLineEdit(lan_host_code_, &dialog);
    edit->setMaxLength(32);
    edit->setPlaceholderText(QStringLiteral("6–32 位字母或数字"));
    form->addRow(QStringLiteral("固定被控码"), edit);
    dialog.contentLayout()->addLayout(form);

    auto* hint = new QLabel(QStringLiteral("修改后将立即生效，主控端需使用新被控码连接本机。"), &dialog);
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

    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dialog, [&] {
        const auto next = edit->text().trimmed();
        if (next.size() < 6 || next.size() > 32) {
            QMessageBox::warning(&dialog, QStringLiteral("固定被控码"), QStringLiteral("被控码长度需为 6 至 32 位。"));
            return;
        }
        lan_host_code_ = next;
        saveLanSecret(QStringLiteral("HostAccessCode"), lan_host_code_);
        dialog.accept();
    });
    connect(edit, &QLineEdit::returnPressed, ok, &QPushButton::click);
    edit->selectAll();
    edit->setFocus();

    if (dialog.exec() == QDialog::Accepted) {
        if (lan_host_) {
            stopLanReceiving();
            startLanReceiving(lan_host_code_, lan_host_port_);
        }
        updateUnattendedBanner();
        setStatus(QStringLiteral("固定被控码已更新。"));
    }
}

void AccountWindow::copyUnattendedInfo() {
    const auto ips = winText(p2p_lan_local_addresses());
    const auto first = ips.section(QStringLiteral(","), 0, 0).trimmed();
    QString text;
    if (!first.isEmpty()) text = QStringLiteral("IP: %1  被控码: %2").arg(first, lan_host_code_);
    else text = lan_host_code_;
    if (auto* cb = QApplication::clipboard()) cb->setText(text);
    setStatus(QStringLiteral("已复制本机局域网直连信息。"));
}

void AccountWindow::refreshLanPeers() {
    if (!lan_scanner_ || !lan_list_) return;
    const auto count = p2p_lan_scanner_read(lan_scanner_.get());
    const QSignalBlocker block(lan_list_);
    lan_list_->clear();

    if (count == 0) {
        auto* placeholder = new QListWidgetItem(QStringLiteral("暂未发现局域网设备"), lan_list_);
        placeholder->setFlags(Qt::NoItemFlags);
        return;
    }

    QListWidgetItem* to_select{};
    for (size_t i = 0; i < count; ++i) {
        p2p_lan_peer_v1 peer{};
        if (p2p_lan_scanner_peer(lan_scanner_.get(), i, &peer) != P2P_OK) continue;
        const auto name = winText(peer.name);
        const auto addr = winText(peer.address);
        const auto state = peer.busy ? QStringLiteral("busy") : QStringLiteral("online");

        auto* item = new QListWidgetItem(makeSidebarDeviceIcon(state, true), name, lan_list_);
        item->setToolTip(QStringLiteral("%1 (%2)").arg(name, addr));
        item->setData(Qt::UserRole, addr);
        item->setData(device_name_role, name);
        item->setData(device_state_role, state);
        item->setData(lan_port_role, static_cast<int>(peer.port));

        if (selected_.kind == SelectionKind::lan && selected_.id == addr) {
            to_select = item;
            selected_.name = name;
            selected_.state = state;
            selected_.port = peer.port;
        }
    }

    if (to_select) {
        lan_list_->setCurrentItem(to_select);
        updateDetailView();
    } else if (selected_.kind == SelectionKind::none && lan_list_->count() > 0
               && right_stack_->currentWidget() != login_page_) {
        auto* first = lan_list_->item(0);
        lan_list_->setCurrentItem(first);
        selected_ = {
            SelectionKind::lan,
            first->data(Qt::UserRole).toString(),
            first->data(device_name_role).toString(),
            first->data(device_state_role).toString(),
            first->data(Qt::UserRole).toString(),
            static_cast<std::uint16_t>(first->data(lan_port_role).toInt()),
            false
        };
        right_stack_->setCurrentWidget(detail_page_);
        updateDetailView();
    }
}

void AccountWindow::openLanConnectDialog(const QString& prefill_host, const QString& prefill_name) {
    FramelessDialog dialog(QStringLiteral("局域网直连"), false, this);
    dialog.resize(390, 232);

    const QString initial_host = !prefill_host.isEmpty()
        ? prefill_host : QSettings().value(QStringLiteral("lan/lastTarget")).toString();
    const QString saved_code = !initial_host.isEmpty()
        ? readLanSecret(QStringLiteral("Peer/") + initial_host) : QString{};

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(12);

    auto* host_edit = new QLineEdit(initial_host, &dialog);
    host_edit->setPlaceholderText(QStringLiteral("如 192.168.0.32"));
    host_edit->setMaxLength(128);
    form->addRow(QStringLiteral("目标 IP 地址"), host_edit);

    auto* code_row = new QHBoxLayout;
    code_row->setSpacing(6);
    auto* code_edit = new QLineEdit(saved_code, &dialog);
    code_edit->setPlaceholderText(QStringLiteral("输入远端固定被控码"));
    code_edit->setMaxLength(32);
    code_edit->setEchoMode(QLineEdit::Password);
    code_row->addWidget(code_edit, 1);
    auto* show_cb = new QCheckBox(QStringLiteral("显示"), &dialog);
    connect(show_cb, &QCheckBox::toggled, &dialog, [code_edit](bool checked) {
        code_edit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    });
    code_row->addWidget(show_cb);
    form->addRow(QStringLiteral("被控码"), code_row);

    auto* remember_cb = new QCheckBox(QStringLiteral("记住此设备的被控码"), &dialog);
    remember_cb->setChecked(!saved_code.isEmpty() || true);
    form->addRow(QString{}, remember_cb);

    dialog.contentLayout()->addLayout(form);
    dialog.contentLayout()->addStretch(1);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消"), &dialog);
    auto* go = new QPushButton(QStringLiteral("立即连接"), &dialog);
    go->setObjectName(QStringLiteral("primary"));
    btns->addWidget(cancel);
    btns->addWidget(go);
    dialog.contentLayout()->addLayout(btns);

    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(host_edit, &QLineEdit::returnPressed, code_edit, qOverload<>(&QLineEdit::setFocus));

    LanConnectOptions result;
    QString display_name = prefill_name;
    connect(go, &QPushButton::clicked, &dialog, [&] {
        const auto raw = host_edit->text().trimmed();
        const auto code = code_edit->text();
        if (raw.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("局域网直连"), QStringLiteral("请输入目标设备的 IP 地址。"));
            host_edit->setFocus();
            return;
        }
        if (code.size() < 6 || code.size() > 32) {
            QMessageBox::warning(&dialog, QStringLiteral("局域网直连"), QStringLiteral("请输入 6 至 32 位被控码。"));
            code_edit->setFocus();
            return;
        }
        QString host = raw;
        std::uint16_t port = P2P_LAN_DEFAULT_PORT;
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
        if (display_name.isEmpty()) display_name = host;
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
    if (selected_.kind == SelectionKind::lan) {
        const auto saved_code = readLanSecret(QStringLiteral("Peer/") + selected_.id);
        if (saved_code.size() >= 6 && saved_code.size() <= 32) {
            LanConnectOptions lan{selected_.id.toStdWString(),
                selected_.port ? selected_.port : static_cast<std::uint16_t>(P2P_LAN_DEFAULT_PORT),
                saved_code.toStdWString()};
            if (!viewer_) viewer_ = new RemoteWindow(this);
            viewer_->openLanSession(lan, selected_.name, options_);
        } else {
            openLanConnectDialog(selected_.id, selected_.name);
        }
    } else if (selected_.kind == SelectionKind::cloud) {
        if (selected_.state != QStringLiteral("online") || selected_.is_self) return;
        if (!viewer_) viewer_ = new RemoteWindow(this);
        viewer_->openSession(account_.get(), selected_.id, selected_.name, options_, diagnostics);
    }
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
        setStatus(winText(p2p_last_error()));
        pending_.reset();
        return;
    }
    password_->clear();
    request_ = Request::login;
    setStatus(create ? QStringLiteral("正在注册账号…") : QStringLiteral("正在登录…"));
    updateControls();
}

void AccountWindow::refresh() {
    refreshServiceStatus();
    if (request_ != Request::none || !p2p_account_signed_in(account_.get())) return;
    if (p2p_account_refresh(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::devices;
    updateControls();
}

void AccountWindow::addLocalDevice() {
    if (request_ != Request::none || !p2p_account_signed_in(account_.get()) || p2p_account_device_id(account_.get())) return;
    if (p2p_account_add_local_device(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
        return;
    }
    request_ = Request::add_device;
    setStatus(QStringLiteral("正在添加本机…"));
    updateControls();
}

void AccountWindow::removeSelectedDevice() {
    if (request_ != Request::none || !p2p_account_signed_in(account_.get()) || selected_.kind != SelectionKind::cloud) return;
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
    const bool was_registered = p2p_account_device_id(account_.get()) != nullptr;
    const auto result = p2p_account_complete(account_.get());
    if (result == P2P_PENDING) return;
    const auto request = std::exchange(request_, Request::none);
    if (result == P2P_OK) {
        if (request == Request::login) {
            QSettings().setValue(QStringLiteral("lastServer"), pending_->server);
            if (pending_->remember && !saveCredentials(*pending_)) {
                setStatus(QStringLiteral("已登录，无法保存 Windows 凭据。"));
            } else {
                setStatus(QString{});
            }
            active_credentials_.emplace(pending_->server, pending_->user, pending_->password, pending_->remember);
            pending_.reset();
            refreshServiceStatus();
            if (!service_info_.running && AppSettings::instance().data().auto_receive_on_login) startReceiving();
            right_stack_->setCurrentWidget(selected_.kind == SelectionKind::none ? welcome_page_ : detail_page_);
            refresh_.start();
            refresh();
        } else if (request == Request::devices || request == Request::add_device || request == Request::remove_device) {
            if (was_registered && !p2p_account_device_id(account_.get())) {
                if (host_) stopReceiving();
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
            stopReceiving();
            active_credentials_.reset();
            if (cloud_list_) cloud_list_->clear();
            if (selected_.kind == SelectionKind::cloud) selected_ = {};
        }
        pending_.reset();
        setStatus(error);
    }
    updateControls();
}

void AccountWindow::signOut() {
    if (!p2p_account_signed_in(account_.get())) return;
    refresh_.stop();
    p2p_account_cancel(account_.get());
    request_ = Request::none;
    stopReceiving();
    active_credentials_.reset();
    if (p2p_account_logout(account_.get()) != P2P_OK) {
        setStatus(winText(p2p_last_error()));
    } else {
        request_ = Request::logout;
        setStatus(QStringLiteral("正在退出账号…"));
    }
    if (cloud_list_) cloud_list_->clear();
    if (selected_.kind == SelectionKind::cloud) selected_ = {};
    loaded_server_.clear();
    loadCredentials();
    updateControls();
}

void AccountWindow::fillDevices() {
    if (!cloud_list_) return;
    const auto local = winText(p2p_account_device_id(account_.get()));
    const auto count = p2p_account_device_count(account_.get());
    const QSignalBlocker block(cloud_list_);
    cloud_list_->clear();

    QListWidgetItem* to_select{};
    for (size_t i = 0; i < count; ++i) {
        p2p_device device{};
        if (p2p_account_device(account_.get(), i, &device) != P2P_OK) continue;
        const auto id = winText(device.id);
        const auto state = winText(device.state);
        const auto name = winText(device.name);
        const auto platform = winText(device.platform);
        const bool is_self = id == local || (service_info_.running && id == service_info_.device_id);
        const auto label = is_self ? QStringLiteral("%1 (本机)").arg(name) : name;

        auto* item = new QListWidgetItem(makeSidebarDeviceIcon(state, false), label, cloud_list_);
        item->setData(Qt::UserRole, id);
        item->setData(device_state_role, state);
        item->setData(device_name_role, name);
        item->setData(device_platform_role, platform);
        if (selected_.kind == SelectionKind::cloud && selected_.id == id) {
            to_select = item;
            selected_.name = name;
            selected_.state = state;
            selected_.subtitle = platform;
            selected_.is_self = is_self;
        }
    }
    if (to_select) {
        cloud_list_->setCurrentItem(to_select);
        updateDetailView();
    } else if (selected_.kind == SelectionKind::cloud) {
        selected_ = {};
        updateDetailView();
    }
}

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
    if (!p2p_account_device_id(account_.get()) || host_ || service_info_.running || !screens_->count()) return;
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
        dialog->contentLayout()->addWidget(new QLabel(QStringLiteral("暂无本机诊断记录。"), dialog));
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
        service_info_ = status;
        updateControls();
    }
    if (service_info_.running && host_) {
        stopReceiving();
    }
    if (!status.running || service_status_pending_) return;
    service_status_pending_ = true;
    const auto generation = service_status_generation_;
    service_status_thread_ = std::jthread([this, generation, status = std::move(status)]() mutable {
        auto info = queryHostWorkerStatus(std::move(status));
        QMetaObject::invokeMethod(this, [this, generation, info = std::move(info)] {
            service_status_pending_ = false;
            if (generation != service_status_generation_) return;
            service_info_ = info;
            updateControls();
        }, Qt::QueuedConnection);
    });
}

void AccountWindow::toggleHostService() {
    QString error;
    if (service_info_.installed) {
        setStatus(QStringLiteral("正在停止并卸载系统服务…"));
        if (!uninstallHostService(&error)) {
            setStatus(error);
            return;
        }
        refreshServiceStatus();
        if (p2p_account_device_id(account_.get()) && !service_info_.running) {
            startReceiving();
        }
        setStatus(QStringLiteral("已卸载系统服务，已恢复绿色便携接收模式。"));
        updateControls();
        return;
    }

    const auto* device_id = p2p_account_device_id(account_.get());
    if (!active_credentials_ || !device_id) {
        setStatus(QStringLiteral("请先添加本机，再启用系统服务。"));
        return;
    }
    stopReceiving();
    setStatus(QStringLiteral("正在安装并启动系统服务（请在弹出的 UAC 窗口确认）…"));
    const int monitor_index = std::max(0, screens_->currentIndex());
    if (!installHostService(active_credentials_->server, active_credentials_->user, active_credentials_->password,
            winText(p2p_account_installation_id(account_.get())), winText(device_id), monitor_index,
            options_.compatible_http, options_.relay_only, &error)) {
        setStatus(error);
        refreshServiceStatus();
        if (!service_info_.running && p2p_account_device_id(account_.get())) startReceiving();
        updateControls();
        return;
    }
    refreshServiceStatus();
    setStatus(QStringLiteral("系统服务已启动并启用开机自启。"));
    updateControls();
    refresh();
}

void AccountWindow::relaunchAsAdmin() {
    QString error;
    if (!relaunchClientElevated(&error)) {
        setStatus(error);
        return;
    }
    quit();
}

void AccountWindow::closeSessions() {
    refresh_.stop();
    lan_scan_timer_.stop();
    lan_scanner_.reset();
    delete viewer_.data();
    stopReceiving();
    stopLanReceiving();
}
