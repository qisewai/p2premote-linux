#include "video_surface.h"
#include "viewer_session.h"

#include <QCursor>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QWheelEvent>

#include <libyuv.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <linux/input-event-codes.h>

namespace {

using p2premote::InputButton;
using p2premote::InputKind;
using FrameClock = std::chrono::steady_clock;
using FrameMilliseconds = std::chrono::duration<double, std::milli>;

constexpr qint64 mouse_move_min_interval_ms = 2;
constexpr int drag_tail_delay_ms = 24;

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

class VideoSurface final : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    VideoSurface(std::function<void()> fullscreen, QWidget* parent)
        : QOpenGLWidget(parent), fullscreen_(std::move(fullscreen)),
          xkb_(QGuiApplication::platformName().startsWith("wayland") || QGuiApplication::platformName() == "xcb") {
        auto fmt = format();
        fmt.setSwapInterval(0);
        setFormat(fmt);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setObjectName(QStringLiteral("video"));
        connect(this, &QOpenGLWidget::frameSwapped, this, [this] {
            const auto timing = std::exchange(pending_swap_, std::nullopt);
            if (timing && session_ && isVisible()) {
                session_->recordFrameSwapped(FrameMilliseconds(timing->draw_at - timing->decoded_at).count(),
                    FrameMilliseconds(FrameClock::now() - timing->draw_at).count());
            }
        });
        heartbeat_.setInterval(p2premote::input_heartbeat_ms);
        connect(&heartbeat_, &QTimer::timeout, this, [this] {
            if (session_) session_->sendInput(InputKind::heartbeat);
        });
        move_flush_timer_.setSingleShot(true);
        connect(&move_flush_timer_, &QTimer::timeout, this, [this] { flushPendingMove(true); });
        drag_tail_timer_.setSingleShot(true);
        connect(&drag_tail_timer_, &QTimer::timeout, this, [this] {
            if (capturing_ && pressed_buttons_ != Qt::NoButton && session_) {
                session_->sendInput(InputKind::heartbeat);
            }
        });
    }

    ~VideoSurface() override {
        if (context()) {
            makeCurrent();
            if (tex_y_) glDeleteTextures(1, &tex_y_);
            if (tex_uv_) glDeleteTextures(1, &tex_uv_);
            vbo_.destroy();
            vao_.destroy();
            program_.removeAllShaders();
            doneCurrent();
        }
    }

    bool hasFrame() const { return frame_.width > 0 && frame_.height > 0 && !frame_.nv12.isEmpty(); }
    bool capturing() const { return capturing_; }

    void setRemoteCursor(DecodedCursor cursor) {
        cursor_separate_ = true;
        if (!cursor.visible) {
            remote_cursor_ = QCursor(Qt::BlankCursor);
        } else if (cursor.image.isNull()) {
            remote_cursor_ = QCursor(Qt::ArrowCursor);
        } else {
            remote_cursor_ = QCursor(QPixmap::fromImage(cursor.image), cursor.hot_x, cursor.hot_y);
        }
        updateSurfaceCursor();
    }

    void setCapture(bool enabled) {
        if (capturing_ == enabled) return;
        if (enabled && (!session_ || !session_->canControl() || !hasFrame())) return;
        capturing_ = enabled;
        if (enabled) {
            setFocus(Qt::OtherFocusReason);
            session_->sendInput(InputKind::heartbeat);
            heartbeat_.start();
            const auto pos = mapFromGlobal(QCursor::pos());
            pointer_in_video_ = hasFrame() && rect().contains(pos) && QRectF(videoRect()).contains(pos);
            updateSurfaceCursor();
        } else {
            heartbeat_.stop();
            move_flush_timer_.stop();
            drag_tail_timer_.stop();
            pending_move_.reset();
            last_move_sent_.reset();
            pressed_buttons_ = Qt::NoButton;
            if (session_) session_->sendInput(InputKind::reset);
            updateSurfaceCursor();
        }
    }

    void attach(ViewerSession* session) {
        cursor_separate_ = false;
        pointer_in_video_ = false;
        remote_cursor_ = QCursor(Qt::ArrowCursor);
        setCapture(false);
        session_ = session;
        frame_ = {};
        pending_swap_.reset();
        fallback_image_ = {};
        frame_dirty_ = true;
        updateSurfaceCursor();
        update();
    }

    void setOverlays(std::vector<p2premote::PluginOverlayItem> overlays) {
        overlays_ = std::move(overlays);
        overlay_cache_dirty_ = true;
        update();
    }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        gl_ready_ = false;
        tex_width_ = tex_height_ = 0;

        const auto fmt = context() ? context()->format() : QSurfaceFormat{};
        const bool is_es = fmt.renderableType() == QSurfaceFormat::OpenGLES;
        const char* header = is_es ? "#version 300 es\nprecision mediump float;\n" : "#version 330 core\n";

        const QByteArray vert_src = QByteArray(header) + R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
out vec2 vTex;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTex = aTex;
}
)";
        const QByteArray frag_src = QByteArray(header) + R"(
in vec2 vTex;
out vec4 fragColor;
uniform sampler2D texY;
uniform sampler2D texUV;
void main() {
    float y = (texture(texY, vTex).r - 0.0627451) * 1.1643836;
    vec2 uv = texture(texUV, vTex).rg - vec2(0.5019608);
    float u = uv.x * 1.1383929;
    float v = uv.y * 1.1383929;
    vec3 rgb = vec3(
        y + 1.5748 * v,
        y - 0.187324 * u - 0.468124 * v,
        y + 1.8556 * u
    );
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";
        program_.removeAllShaders();
        if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vert_src)
            || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, frag_src)
            || !program_.link()) {
            return;
        }

        static constexpr float vertices[] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        if (vao_.isCreated()) vao_.destroy();
        if (vbo_.isCreated()) vbo_.destroy();
        vao_.create();
        QOpenGLVertexArrayObject::Binder vao_binder(&vao_);
        vbo_.create();
        vbo_.bind();
        vbo_.allocate(vertices, sizeof(vertices));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        vbo_.release();

        if (!tex_y_) glGenTextures(1, &tex_y_);
        if (!tex_uv_) glGenTextures(1, &tex_uv_);
        for (const GLuint tex : {tex_y_, tex_uv_}) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        program_.bind();
        program_.setUniformValue("texY", 0);
        program_.setUniformValue("texUV", 1);
        program_.release();

        gl_ready_ = true;
    }

    void paintGL() override {
        const auto previous_width = frame_.width;
        const auto previous_height = frame_.height;
        const bool new_frame = session_ && session_->takeLatestFrame(frame_);
        const auto draw_at = new_frame ? FrameClock::now() : FrameClock::time_point{};
        if (new_frame) {
            frame_dirty_ = true;
            if (capturing_ && (previous_width != frame_.width || previous_height != frame_.height)) {
                updatePointerInVideo(mapFromGlobal(QCursor::pos()));
            }
        }
        if (!hasFrame()) {
            QPainter painter(this);
            painter.fillRect(rect(), QColor("#111827"));
            painter.setPen(QColor("#cbd5e1"));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待被控端画面"));
            paintCachedOverlays(painter, rect());
            return;
        }
        const auto area = videoRect();
        const bool has_overlays = hasVisibleOverlays();
        if (gl_ready_ && !has_overlays) {
            drawGlFrame(area);
        } else {
            QPainter painter(this);
            if (gl_ready_) {
                painter.beginNativePainting();
                drawGlFrame(area);
                painter.endNativePainting();
            } else {
                painter.fillRect(rect(), QColor("#111827"));
                updateFallbackImage();
                if (!fallback_image_.isNull()) {
                    painter.setRenderHint(QPainter::SmoothPixmapTransform);
                    painter.drawImage(area, fallback_image_);
                }
            }
            if (has_overlays) paintCachedOverlays(painter, area);
        }
        if (new_frame && !area.isEmpty() && (gl_ready_ || !fallback_image_.isNull())) {
            pending_swap_ = FrameTiming{frame_.decoded_at, draw_at};
        }
    }

    void focusOutEvent(QFocusEvent* event) override {
        setCapture(false);
        QOpenGLWidget::focusOutEvent(event);
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
        return QOpenGLWidget::event(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        updatePointerInVideo(event->position());
        queueMove(event->position());
    }

    void leaveEvent(QEvent* event) override {
        if (pointer_in_video_) {
            pointer_in_video_ = false;
            updateSurfaceCursor();
        }
        QOpenGLWidget::leaveEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QOpenGLWidget::resizeEvent(event);
        if (capturing_) updatePointerInVideo(mapFromGlobal(QCursor::pos()));
    }

    void mousePressEvent(QMouseEvent* event) override {
        updatePointerInVideo(event->position());
        if (!capturing_ && event->button() == Qt::LeftButton && pointer_in_video_) {
            setCapture(true);
        }
        if (const auto button = pointerButton(event->button())) {
            pressed_buttons_ |= event->button();
            sendPointerAction(InputKind::button_down, button, event->position());
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (const auto button = pointerButton(event->button())) {
            pressed_buttons_ &= ~event->button();
            sendPointerAction(InputKind::button_up, button, event->position());
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        const auto amount = std::clamp(event->angleDelta().y(), -32768, 32767);
        if (amount) {
            sendPointerAction(InputKind::wheel, static_cast<std::uint16_t>(amount), event->position());
            event->accept();
        }
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
    void drawGlFrame(const QRect& area) {
        const qreal dpr = devicePixelRatioF();
        const int fb_w = std::max(1, qRound(width() * dpr));
        const int fb_h = std::max(1, qRound(height() * dpr));

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(17.0f / 255.0f, 24.0f / 255.0f, 39.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (frame_dirty_) {
            const auto luma = static_cast<std::size_t>(frame_.width) * frame_.height;
            const auto* y_ptr = frame_.nv12.constData();
            const auto* uv_ptr = y_ptr + luma;
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (tex_width_ != frame_.width || tex_height_ != frame_.height) {
                glBindTexture(GL_TEXTURE_2D, tex_y_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame_.width, frame_.height, 0, GL_RED, GL_UNSIGNED_BYTE, y_ptr);
                glBindTexture(GL_TEXTURE_2D, tex_uv_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, frame_.width / 2, frame_.height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, uv_ptr);
                tex_width_ = frame_.width;
                tex_height_ = frame_.height;
            } else {
                glBindTexture(GL_TEXTURE_2D, tex_y_);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_.width, frame_.height, GL_RED, GL_UNSIGNED_BYTE, y_ptr);
                glBindTexture(GL_TEXTURE_2D, tex_uv_);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_.width / 2, frame_.height / 2, GL_RG, GL_UNSIGNED_BYTE, uv_ptr);
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            frame_dirty_ = false;
        }

        const int vx = qRound(area.x() * dpr);
        const int vw = std::max(1, qRound(area.width() * dpr));
        const int vh = std::max(1, qRound(area.height() * dpr));
        const int vy = fb_h - qRound((area.y() + area.height()) * dpr);
        glViewport(vx, vy, vw, vh);

        program_.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_y_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_uv_);
        vao_.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        vao_.release();
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        program_.release();
    }

    void updateFallbackImage() {
        if (!frame_dirty_) return;
        frame_dirty_ = false;
        if (fallback_image_.width() != frame_.width || fallback_image_.height() != frame_.height) {
            fallback_image_ = QImage(frame_.width, frame_.height, QImage::Format_ARGB32);
        }
        if (fallback_image_.isNull()) return;
        const auto luma = static_cast<std::size_t>(frame_.width) * frame_.height;
        const auto* source = reinterpret_cast<const std::uint8_t*>(frame_.nv12.constData());
        libyuv::NV12ToARGBMatrix(source, frame_.width, source + luma, frame_.width,
            fallback_image_.bits(), fallback_image_.bytesPerLine(), &libyuv::kYuvH709Constants,
            frame_.width, frame_.height);
    }

    bool hasVisibleOverlays() const {
        return std::ranges::any_of(overlays_, [](const auto& item) {
            return item.visible && !item.text.empty();
        });
    }

    void paintCachedOverlays(QPainter& painter, const QRect& area) {
        if (area.isEmpty() || !hasVisibleOverlays()) return;
        const qreal dpr = devicePixelRatioF();
        if (overlay_cache_dirty_ || overlay_cache_area_ != area.size()
            || !qFuzzyCompare(overlay_cache_dpr_, dpr)) {
            rebuildOverlayCache(area.size(), dpr);
        }
        for (const auto& entry : cached_overlays_) {
            painter.drawImage(area.topLeft() + entry.pos, entry.image);
        }
    }

    void rebuildOverlayCache(const QSize& area_size, qreal dpr) {
        using p2premote::OverlayAnchor;
        cached_overlays_.clear();
        std::array<int, 6> stacks{};
        const QRect local_area(QPoint(0, 0), area_size);
        for (const auto& item : overlays_) {
            if (!item.visible || item.text.empty()) continue;
            auto font = item.monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : this->font();
            font.setPixelSize(item.font_size);
            const auto text = QString::fromStdWString(item.text);
            const QFontMetrics metrics(font);
            const auto bounds = metrics.boundingRect(QRect(0, 0, std::max(1, local_area.width() - 32), local_area.height()),
                Qt::AlignLeft | Qt::TextWordWrap, text);
            const QSize size(bounds.width() + 18, bounds.height() + 12);
            const bool right = item.anchor == OverlayAnchor::top_right || item.anchor == OverlayAnchor::bottom_right;
            const bool bottom = item.anchor == OverlayAnchor::bottom_left || item.anchor == OverlayAnchor::bottom_right;
            const bool center = item.anchor == OverlayAnchor::top_center || item.anchor == OverlayAnchor::bottom_center;
            auto& offset = stacks[static_cast<unsigned>(item.anchor)];
            const int x = center ? local_area.center().x() - size.width() / 2 :
                (right ? local_area.right() - size.width() - 8 : local_area.left() + 8);
            const int y = bottom ? local_area.bottom() - size.height() - 8 - offset : local_area.top() + 8 + offset;
            offset += size.height() + 6;

            QImage badge(std::max(1, qRound(size.width() * dpr)), std::max(1, qRound(size.height() * dpr)),
                QImage::Format_ARGB32_Premultiplied);
            badge.setDevicePixelRatio(dpr);
            badge.fill(Qt::transparent);
            QPainter cache_painter(&badge);
            cache_painter.setFont(font);
            auto background = item.bg_rgb == p2premote::overlay_default_color ? QColor("#162238") : QColor::fromRgb(item.bg_rgb);
            background.setAlpha(item.opacity);
            cache_painter.setPen(Qt::NoPen);
            cache_painter.setBrush(background);
            cache_painter.drawRoundedRect(QRect(QPoint(0, 0), size), 5, 5);
            cache_painter.setPen(item.text_rgb == p2premote::overlay_default_color ? QColor("#eef4ff") : QColor::fromRgb(item.text_rgb));
            cache_painter.drawText(QRect(QPoint(0, 0), size).adjusted(9, 6, -9, -6), Qt::AlignLeft | Qt::TextWordWrap, text);
            cached_overlays_.push_back({QPoint(x, y), std::move(badge)});
        }
        overlay_cache_area_ = area_size;
        overlay_cache_dpr_ = dpr;
        overlay_cache_dirty_ = false;
    }

    QRect videoRect() const {
        if (!hasFrame()) return {};
        const auto size = QSize(frame_.width, frame_.height).scaled(this->size(), Qt::KeepAspectRatio);
        return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    }

    std::optional<std::pair<std::uint16_t, std::uint16_t>> normalizePoint(const QPointF& pos, bool allow_outside) const {
        const auto area = videoRect();
        if (area.isEmpty() || (!allow_outside && !QRectF(area).contains(pos))) return std::nullopt;
        const auto normalize = [](qreal value, int pixels) {
            const auto fraction = std::clamp(value / std::max(1, pixels - 1), qreal{0}, qreal{1});
            return static_cast<std::uint16_t>(fraction * std::numeric_limits<std::uint16_t>::max());
        };
        return std::pair{normalize(pos.x() - area.x(), area.width()), normalize(pos.y() - area.y(), area.height())};
    }

    void queueMove(const QPointF& pos) {
        if (!capturing_) return;
        const auto point = normalizePoint(pos, false);
        if (!point) return;
        if (!pending_move_ && last_move_sent_ && *last_move_sent_ == *point) return;
        pending_move_ = *point;
        flushPendingMove(false);
    }

    void flushPendingMove(bool force) {
        if (!pending_move_ || !capturing_ || !session_) return;
        if (!force && move_clock_.isValid()) {
            const auto elapsed = move_clock_.elapsed();
            if (elapsed < mouse_move_min_interval_ms) {
                if (!move_flush_timer_.isActive()) {
                    move_flush_timer_.start(static_cast<int>(mouse_move_min_interval_ms - elapsed));
                }
                return;
            }
        }
        move_flush_timer_.stop();
        const auto move = *std::exchange(pending_move_, std::nullopt);
        if (last_move_sent_ && *last_move_sent_ == move) return;
        if (session_->sendInput(InputKind::move, 0, move.first, move.second)) {
            last_move_sent_ = move;
            move_clock_.restart();
            if (pressed_buttons_ != Qt::NoButton) drag_tail_timer_.start(drag_tail_delay_ms);
        }
    }

    void sendPointerAction(InputKind kind, std::uint16_t code, const QPointF& pos) {
        if (!capturing_ || !session_) return;
        const auto point = normalizePoint(pos, kind == InputKind::button_up);
        if (!point) return;
        pending_move_.reset();
        move_flush_timer_.stop();
        drag_tail_timer_.stop();
        if (session_->sendInput(kind, code, point->first, point->second)) {
            last_move_sent_ = *point;
        }
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

    void updatePointerInVideo(const QPointF& pos) {
        const bool inside = hasFrame() && rect().contains(pos.toPoint()) && QRectF(videoRect()).contains(pos);
        if (inside == pointer_in_video_) return;
        pointer_in_video_ = inside;
        updateSurfaceCursor();
    }

    void updateSurfaceCursor() {
        if (!capturing_ || !pointer_in_video_) {
            if (testAttribute(Qt::WA_SetCursor)) unsetCursor();
            return;
        }
        setCursor(cursor_separate_ ? remote_cursor_ : QCursor(Qt::BlankCursor));
    }

    ViewerSession* session_{};
    std::function<void()> fullscreen_;
    struct CachedOverlay {
        QPoint pos;
        QImage image;
    };
    std::vector<p2premote::PluginOverlayItem> overlays_;
    std::vector<CachedOverlay> cached_overlays_;
    QSize overlay_cache_area_;
    qreal overlay_cache_dpr_{1.0};
    bool overlay_cache_dirty_{true};
    QTimer heartbeat_;
    QTimer move_flush_timer_;
    QTimer drag_tail_timer_;
    QElapsedTimer move_clock_;
    std::optional<std::pair<std::uint16_t, std::uint16_t>> pending_move_;
    std::optional<std::pair<std::uint16_t, std::uint16_t>> last_move_sent_;
    Qt::MouseButtons pressed_buttons_{Qt::NoButton};
    VideoFrame frame_;
    struct FrameTiming { FrameClock::time_point decoded_at, draw_at; };
    std::optional<FrameTiming> pending_swap_;
    QImage fallback_image_;
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
    GLuint tex_y_{};
    GLuint tex_uv_{};
    int tex_width_{};
    int tex_height_{};
    bool gl_ready_{};
    bool frame_dirty_{};
    QCursor remote_cursor_{Qt::ArrowCursor};
    bool capturing_{};
    bool cursor_separate_{};
    bool pointer_in_video_{};
    const bool xkb_;
};

} // namespace

QWidget* createVideoSurface(std::function<void()> fullscreen, QWidget* parent) {
    return new VideoSurface(std::move(fullscreen), parent);
}

void attachVideoSession(QWidget* widget, ViewerSession* session) {
    static_cast<VideoSurface*>(widget)->attach(session);
}

void setVideoCursor(QWidget* widget, DecodedCursor cursor) {
    static_cast<VideoSurface*>(widget)->setRemoteCursor(std::move(cursor));
}

void setVideoOverlays(QWidget* widget, std::vector<p2premote::PluginOverlayItem> overlays) {
    static_cast<VideoSurface*>(widget)->setOverlays(std::move(overlays));
}
