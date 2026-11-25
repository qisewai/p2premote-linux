#include "video_surface.h"
#include "viewer_session.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QFontDatabase>
#include <QWidget>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <linux/input-event-codes.h>

namespace {

using p2premote::InputButton;
using p2premote::InputKind;

std::uint16_t windowsScanCode(const QKeyEvent& event, bool xkb) {
    // The Windows host uses SendInput with KEYEVENTF_SCANCODE. Bit 8 marks extended keys.
    if (xkb && event.nativeScanCode() > 8) {
        const auto code = event.nativeScanCode() - 8;
        if ((code >= KEY_ESC && code <= KEY_KPDOT) || code == KEY_102ND || code == KEY_F11 || code == KEY_F12) {
            return static_cast<std::uint16_t>(code);
        }
        switch (code) {
        case KEY_KPENTER: return 0x11c;
        case KEY_RIGHTCTRL: return 0x11d;
        case KEY_KPSLASH: return 0x135;
        case KEY_SYSRQ: return 0x137;
        case KEY_RIGHTALT: return 0x138;
        case KEY_HOME: return 0x147;
        case KEY_UP: return 0x148;
        case KEY_PAGEUP: return 0x149;
        case KEY_LEFT: return 0x14b;
        case KEY_RIGHT: return 0x14d;
        case KEY_END: return 0x14f;
        case KEY_DOWN: return 0x150;
        case KEY_PAGEDOWN: return 0x151;
        case KEY_INSERT: return 0x152;
        case KEY_DELETE: return 0x153;
        case KEY_LEFTMETA: return 0x15b;
        case KEY_RIGHTMETA: return 0x15c;
        case KEY_COMPOSE: return 0x15d;
        default: break;
        }
    }
    const auto key = event.key();
    constexpr std::array<std::uint16_t, 26> letters{
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return letters[key - Qt::Key_A];
    }
    if (key >= Qt::Key_1 && key <= Qt::Key_9) {
        return static_cast<std::uint16_t>(0x02 + key - Qt::Key_1);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F10) {
        return static_cast<std::uint16_t>(0x3b + key - Qt::Key_F1);
    }
    switch (key) {
    case Qt::Key_Escape: return 0x01;
    case Qt::Key_Exclam: return 0x02;
    case Qt::Key_At: return 0x03;
    case Qt::Key_NumberSign: return 0x04;
    case Qt::Key_Dollar: return 0x05;
    case Qt::Key_Percent: return 0x06;
    case Qt::Key_AsciiCircum: return 0x07;
    case Qt::Key_Ampersand: return 0x08;
    case Qt::Key_Asterisk: return 0x09;
    case Qt::Key_ParenLeft: return 0x0a;
    case Qt::Key_0:
    case Qt::Key_ParenRight: return 0x0b;
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return 0x0c;
    case Qt::Key_Equal:
    case Qt::Key_Plus: return 0x0d;
    case Qt::Key_Backspace: return 0x0e;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return 0x0f;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return 0x1a;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return 0x1b;
    case Qt::Key_Return: return 0x1c;
    case Qt::Key_Enter: return 0x11c;
    case Qt::Key_Control: return 0x1d;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return 0x27;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return 0x28;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return 0x29;
    case Qt::Key_Shift: return 0x2a;
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return 0x2b;
    case Qt::Key_Comma:
    case Qt::Key_Less: return 0x33;
    case Qt::Key_Period:
    case Qt::Key_Greater: return 0x34;
    case Qt::Key_Slash:
    case Qt::Key_Question: return 0x35;
    case Qt::Key_Alt: return 0x38;
    case Qt::Key_AltGr: return 0x138;
    case Qt::Key_Space: return 0x39;
    case Qt::Key_CapsLock: return 0x3a;
    case Qt::Key_NumLock: return 0x45;
    case Qt::Key_F11: return 0x57;
    case Qt::Key_Home: return 0x147;
    case Qt::Key_Up: return 0x148;
    case Qt::Key_PageUp: return 0x149;
    case Qt::Key_Left: return 0x14b;
    case Qt::Key_Right: return 0x14d;
    case Qt::Key_End: return 0x14f;
    case Qt::Key_Down: return 0x150;
    case Qt::Key_PageDown: return 0x151;
    case Qt::Key_Insert: return 0x152;
    case Qt::Key_Delete: return 0x153;
    case Qt::Key_Meta: return 0x15b;
    default: return 0;
    }
}

std::uint16_t pointerButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return static_cast<std::uint16_t>(InputButton::left);
    case Qt::RightButton: return static_cast<std::uint16_t>(InputButton::right);
    case Qt::MiddleButton: return static_cast<std::uint16_t>(InputButton::middle);
    default: return 0;
    }
}

class VideoSurface final : public QWidget {
public:
    VideoSurface(std::function<void()> fullscreen, QWidget* parent)
        : QWidget(parent), fullscreen_(std::move(fullscreen)),
          xkb_(QGuiApplication::platformName().startsWith("wayland") || QGuiApplication::platformName() == "xcb") {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setObjectName(QStringLiteral("video"));
        heartbeat_.setInterval(p2premote::input_heartbeat_ms);
        connect(&heartbeat_, &QTimer::timeout, this, [this] { session_->sendInput(InputKind::heartbeat); });
    }

    bool hasFrame() const { return !frame_.isNull(); }
    bool capturing() const { return capturing_; }

    void setFrame(QImage frame) {
        frame_ = std::move(frame);
        update();
    }

    void setCapture(bool enabled) {
        if (capturing_ == enabled) return;
        if (enabled && (!session_ || !session_->canControl() || !hasFrame())) return;
        capturing_ = enabled;
        if (enabled) {
            setFocus(Qt::OtherFocusReason);
            session_->sendInput(InputKind::heartbeat);
            heartbeat_.start();
        } else {
            heartbeat_.stop();
            if (session_) session_->sendInput(InputKind::reset);
        }

    }

    void attach(ViewerSession* session) {
        setCapture(false);
        session_ = session;
        setFrame({});
    }

    void setOverlays(std::vector<p2premote::PluginOverlayItem> overlays) {
        overlays_ = std::move(overlays);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#111827"));
        if (frame_.isNull()) {
            painter.setPen(QColor("#cbd5e1"));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待被控端画面"));
            return;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto area = videoRect();
        painter.drawImage(area, frame_);
        paintOverlays(painter, area);
    }

    void focusOutEvent(QFocusEvent* event) override {
        setCapture(false);
        QWidget::focusOutEvent(event);
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) setCapture(false);
        if (capturing_ && event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (capturing_ && event->type() == QEvent::KeyPress) {
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        return QWidget::event(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override { pointer(InputKind::move, 0, event->position()); }

    void mousePressEvent(QMouseEvent* event) override {
        if (!capturing_ && event->button() == Qt::LeftButton && QRectF(videoRect()).contains(event->position())) {
            setCapture(true);
        }
        if (const auto button = pointerButton(event->button())) {
            pointer(InputKind::button_down, button, event->position());
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (const auto button = pointerButton(event->button())) {
            pointer(InputKind::button_up, button, event->position());
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        const auto amount = std::clamp(event->angleDelta().y(), -32768, 32767);
        if (amount) pointer(InputKind::wheel, static_cast<std::uint16_t>(amount), event->position());
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_F12 && capturing_) {
            setCapture(false);
            event->accept();
        } else if (event->key() == Qt::Key_F11 && capturing_) {
            if (!event->isAutoRepeat() && fullscreen_) fullscreen_();
            event->accept();
        } else {
            key(InputKind::key_down, event);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override { mousePressEvent(event); }

    void keyReleaseEvent(QKeyEvent* event) override { key(InputKind::key_up, event); }

private:
    void paintOverlays(QPainter& painter, const QRect& area) {
        using p2premote::OverlayAnchor;
        std::array<int, 6> stacks{};
        painter.save();
        painter.setClipRect(area);
        for (const auto& item : overlays_) {
            if (!item.visible || item.text.empty()) continue;
            auto font = item.monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : this->font();
            font.setPixelSize(item.font_size);
            painter.setFont(font);
            const auto text = QString::fromStdWString(item.text);
            const auto bounds = painter.fontMetrics().boundingRect(QRect(0, 0, std::max(1, area.width() - 32), area.height()),
                Qt::AlignLeft | Qt::TextWordWrap, text);
            const QSize size(bounds.width() + 18, bounds.height() + 12);
            const bool right = item.anchor == OverlayAnchor::top_right || item.anchor == OverlayAnchor::bottom_right;
            const bool bottom = item.anchor == OverlayAnchor::bottom_left || item.anchor == OverlayAnchor::bottom_right;
            const bool center = item.anchor == OverlayAnchor::top_center || item.anchor == OverlayAnchor::bottom_center;
            auto& offset = stacks[static_cast<unsigned>(item.anchor)];
            const int x = center ? area.center().x() - size.width() / 2 :
                (right ? area.right() - size.width() - 8 : area.left() + 8);
            const int y = bottom ? area.bottom() - size.height() - 8 - offset : area.top() + 8 + offset;
            offset += size.height() + 6;
            const QRect box(QPoint(x, y), size);
            auto background = item.bg_rgb == p2premote::overlay_default_color ? QColor("#162238") : QColor::fromRgb(item.bg_rgb);
            background.setAlpha(item.opacity);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(box, 5, 5);
            painter.setPen(item.text_rgb == p2premote::overlay_default_color ? QColor("#eef4ff") : QColor::fromRgb(item.text_rgb));
            painter.drawText(box.adjusted(9, 6, -9, -6), Qt::AlignLeft | Qt::TextWordWrap, text);
        }
        painter.restore();
    }

    QRect videoRect() const {
        const auto size = frame_.size().scaled(this->size(), Qt::KeepAspectRatio);
        return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    }

    void pointer(InputKind kind, std::uint16_t code, const QPointF& pos) {
        if (!capturing_) return;
        const auto area = videoRect();
        if (area.isEmpty() || (kind != InputKind::button_up && !QRectF(area).contains(pos))) return;
        const auto normalize = [](qreal value, int pixels) {
            const auto fraction = std::clamp(value / std::max(1, pixels - 1), qreal{0}, qreal{1});
            return static_cast<std::uint16_t>(fraction * std::numeric_limits<std::uint16_t>::max());
        };
        session_->sendInput(kind, code, normalize(pos.x() - area.x(), area.width()),
            normalize(pos.y() - area.y(), area.height()));
    }

    void key(InputKind kind, QKeyEvent* event) {
        if (!capturing_) return;
        if (event->key() == Qt::Key_F11 || event->key() == Qt::Key_F12) {
            event->accept();
            return;
        }
        const auto code = windowsScanCode(*event, xkb_);
        if (code && !event->isAutoRepeat()) {
            bool sync_lock{};
            if (xkb_ && event->nativeScanCode() > 8) {
                auto modifiers = event->nativeModifiers();
                const auto lock = code == 0x3a ? 0x02u : (code == 0x45 ? 0x10u : 0u);
                // Native XKB events report the modifier mask before applying this key press.
                if (kind == InputKind::key_down) modifiers ^= lock;
                sync_lock = session_->updateKeyboardLocks(modifiers & 0x02u, modifiers & 0x10u) && lock;
            }
            if (!sync_lock) session_->sendInput(kind, code);
        }
        event->accept();
    }

    ViewerSession* session_{};
    std::function<void()> fullscreen_;
    std::vector<p2premote::PluginOverlayItem> overlays_;
    QTimer heartbeat_;
    QImage frame_;
    bool capturing_{};
    const bool xkb_;
};

} // namespace

QWidget* createVideoSurface(std::function<void()> fullscreen, QWidget* parent) {
    return new VideoSurface(std::move(fullscreen), parent);
}

void attachVideoSession(QWidget* widget, ViewerSession* session) {
    static_cast<VideoSurface*>(widget)->attach(session);
}

void setVideoFrame(QWidget* widget, QImage frame) {
    static_cast<VideoSurface*>(widget)->setFrame(std::move(frame));
}

void setVideoOverlays(QWidget* widget, std::vector<p2premote::PluginOverlayItem> overlays) {
    static_cast<VideoSurface*>(widget)->setOverlays(std::move(overlays));
}
