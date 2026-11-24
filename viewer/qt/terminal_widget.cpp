#include "terminal_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStringList>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr qsizetype input_chunk_size = 8192;
constexpr qsizetype max_pending_input = 256 * 1024;

QColor toQColor(const GhosttyColorRgb& c) noexcept {
    return QColor(c.r, c.g, c.b);
}

QFont chooseTerminalFont(int point_size) {
    const QStringList candidates = {
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Cascadia Code"),
        QStringLiteral("Consolas"),
        QStringLiteral("Sarasa Mono SC"),
        QStringLiteral("Courier New")
    };
    const auto families = QFontDatabase::families();
    for (const auto& name : candidates) {
        if (families.contains(name)) {
            QFont f(name, point_size);
            f.setStyleHint(QFont::Monospace);
            f.setFixedPitch(true);
            return f;
        }
    }
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(point_size);
    return f;
}

int xtermModifierCode(Qt::KeyboardModifiers mods) noexcept {
    int code = 1;
    if (mods & Qt::ShiftModifier) code += 1;
    if (mods & Qt::AltModifier) code += 2;
    if (mods & Qt::ControlModifier) code += 4;
    return code;
}

QByteArray encodeCsiCursorKey(char final_char, Qt::KeyboardModifiers mods) {
    const int mod = xtermModifierCode(mods);
    if (mod == 1) {
        QByteArray out(3, '\0');
        out[0] = '\x1b';
        out[1] = '[';
        out[2] = final_char;
        return out;
    }
    return QByteArrayLiteral("\x1b[1;") + QByteArray::number(mod) + final_char;
}

QByteArray encodeTildeKey(int code, Qt::KeyboardModifiers mods) {
    const int mod = xtermModifierCode(mods);
    if (mod == 1) {
        return QByteArrayLiteral("\x1b[") + QByteArray::number(code) + '~';
    }
    return QByteArrayLiteral("\x1b[") + QByteArray::number(code) + ';' + QByteArray::number(mod) + '~';
}

} // namespace

GhosttyTerminalWidget::GhosttyTerminalWidget(QWidget* parent)
    : QWidget(parent), font_(chooseTerminalFont(base_font_pt_)) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setCursor(Qt::IBeamCursor);
    setMinimumSize(360, 200);

    updateMetrics();
    input_retry_.setInterval(500);
    connect(&input_retry_, &QTimer::timeout, this, &GhosttyTerminalWidget::sendPendingInput);

    if (ghostty_terminal_new(nullptr, &terminal_, static_cast<uint16_t>(cols_), static_cast<uint16_t>(rows_)) == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_USERDATA, this);
        GhosttyTerminalWritePtyFn write_fn = &GhosttyTerminalWidget::onWritePty;
        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(write_fn));

        const GhosttyColorRgb bg{16, 22, 31};
        const GhosttyColorRgb fg{230, 237, 243};
        const GhosttyColorRgb cursor{88, 166, 255};
        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
        ghostty_terminal_set(terminal_, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);

        ghostty_render_state_new(nullptr, &render_state_);
        ghostty_render_state_row_iterator_new(nullptr, &row_iter_);
        ghostty_render_state_row_cells_new(nullptr, &row_cells_);
    }

    blink_timer_.setInterval(530);
    connect(&blink_timer_, &QTimer::timeout, this, [this]() {
        cursor_blink_on_ = !cursor_blink_on_;
        update();
    });
    blink_timer_.start();
}

GhosttyTerminalWidget::~GhosttyTerminalWidget() {
    if (row_cells_) ghostty_render_state_row_cells_free(row_cells_);
    if (row_iter_) ghostty_render_state_row_iterator_free(row_iter_);
    if (render_state_) ghostty_render_state_free(render_state_);
    if (terminal_) ghostty_terminal_free(terminal_);
}

void GhosttyTerminalWidget::onWritePty(GhosttyTerminal, void* userdata, const uint8_t* data, size_t len) {
    if (!userdata || !data || len == 0) return;
    auto* self = static_cast<GhosttyTerminalWidget*>(userdata);
    self->queueInput(QByteArray(reinterpret_cast<const char*>(data), static_cast<qsizetype>(len)));
}

void GhosttyTerminalWidget::updateMetrics() {
    const QFontMetricsF fm(font_);
    cell_w_ = std::max(6, static_cast<int>(std::ceil(fm.horizontalAdvance(QChar('M')))));
    cell_h_ = std::max(12, static_cast<int>(std::ceil(fm.height())) + 2);
    ascent_ = static_cast<int>(std::ceil(fm.ascent())) + 1;
}

void GhosttyTerminalWidget::syncGridSize(bool notify) {
    const int avail_w = std::max(1, width() - pad_x_ * 2);
    const int avail_h = std::max(1, height() - pad_y_ * 2);
    const int next_cols = std::clamp(avail_w / cell_w_, 20, 400);
    const int next_rows = std::clamp(avail_h / cell_h_, 5, 200);
    if (next_cols == cols_ && next_rows == rows_) return;
    cols_ = next_cols;
    rows_ = next_rows;
    clearSelection();
    if (terminal_) {
        ghostty_terminal_resize(terminal_, static_cast<uint16_t>(cols_), static_cast<uint16_t>(rows_),
            static_cast<uint32_t>(cell_w_), static_cast<uint32_t>(cell_h_));
    }
    if (notify) emit resized(cols_, rows_);
    update();
}

void GhosttyTerminalWidget::adjustFontSize(int delta_pt) {
    const int next_pt = (delta_pt == 0) ? base_font_pt_ : std::clamp(font_.pointSize() + delta_pt, 8, 24);
    if (next_pt == font_.pointSize()) return;
    font_.setPointSize(next_pt);
    updateMetrics();
    syncGridSize(true);
    update();
}

void GhosttyTerminalWidget::feed(const QByteArray& bytes) {
    if (!terminal_ || bytes.isEmpty()) return;
    ghostty_terminal_vt_write(terminal_, reinterpret_cast<const uint8_t*>(bytes.constData()),
        static_cast<size_t>(bytes.size()));
    cursor_blink_on_ = true;
    update();
}

void GhosttyTerminalWidget::resetTerminal() {
    if (!terminal_) return;
    clearSelection();
    ghostty_terminal_reset(terminal_);
    cursor_blink_on_ = true;
    update();
}

void GhosttyTerminalWidget::clearDisplay() {
    clearSelection();
    // Erase the display and scrollback locally while preserving negotiated modes.
    feed(QByteArrayLiteral("\x1b[2J\x1b[3J"));
}

void GhosttyTerminalWidget::setInputSession(quint32 session) {
    if (input_session_ == session) return;
    input_session_ = session;
    input_retry_.stop();
    pending_input_.clear();
    in_flight_size_ = 0;
}

void GhosttyTerminalWidget::acknowledgeInput(quint32 sequence) {
    if (!in_flight_size_ || sequence != input_sequence_) return;
    pending_input_.remove(0, in_flight_size_);
    in_flight_size_ = 0;
    if (pending_input_.isEmpty()) input_retry_.stop();
    else sendPendingInput();
}

void GhosttyTerminalWidget::interrupt() {
    sendBytes(QByteArray(1, '\x03'));
}

void GhosttyTerminalWidget::queueInput(const QByteArray& bytes) {
    if (!input_session_ || bytes.isEmpty()) return;
    if (bytes.size() > max_pending_input - pending_input_.size()) {
        QToolTip::showText(mapToGlobal(rect().center()),
            QStringLiteral("终端输入队列空间不足，无法发送这段输入（上限 256 KiB）"), this);
        return;
    }
    pending_input_.append(bytes);
    if (!in_flight_size_) sendPendingInput();
}

void GhosttyTerminalWidget::sendPendingInput() {
    if (!input_session_ || pending_input_.isEmpty()) return;
    if (!in_flight_size_) {
        in_flight_size_ = std::min(input_chunk_size, pending_input_.size());
        ++input_sequence_;
        if (input_sequence_ == 0) ++input_sequence_;
    }
    input_retry_.start();
    emit inputChunk(input_session_, input_sequence_, pending_input_.first(in_flight_size_));
}

void GhosttyTerminalWidget::sendBytes(const QByteArray& bytes) {
    if (bytes.isEmpty()) return;
    clearSelection();
    if (terminal_) {
        GhosttyTerminalScrollViewport sv{};
        sv.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM;
        ghostty_terminal_scroll_viewport(terminal_, sv);
    }
    cursor_blink_on_ = true;
    queueInput(bytes);
    update();
}

void GhosttyTerminalWidget::pasteClipboard() {
    const auto* clip = QApplication::clipboard();
    if (!clip) return;
    QString text = clip->text();
    if (text.isEmpty()) return;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    text.replace(QChar('\n'), QChar('\r'));
    sendBytes(text.toUtf8());
}

void GhosttyTerminalWidget::clearSelection() {
    selecting_ = false;
    has_selection_ = false;
}

QPoint GhosttyTerminalWidget::pixelToCell(const QPoint& pos) const noexcept {
    const int col = std::clamp((pos.x() - pad_x_) / std::max(1, cell_w_), 0, std::max(0, cols_ - 1));
    const int row = std::clamp((pos.y() - pad_y_) / std::max(1, cell_h_), 0, std::max(0, rows_ - 1));
    return {col, row};
}

void GhosttyTerminalWidget::normalizedSelection(QPoint& start, QPoint& end) const noexcept {
    start = sel_anchor_;
    end = sel_current_;
    if (start.y() > end.y() || (start.y() == end.y() && start.x() > end.x())) {
        std::swap(start, end);
    }
}

bool GhosttyTerminalWidget::isCellSelected(int col, int row) const noexcept {
    if (!has_selection_) return false;
    QPoint start, end;
    normalizedSelection(start, end);
    if (row < start.y() || row > end.y()) return false;
    if (row == start.y() && row == end.y()) return col >= start.x() && col <= end.x();
    if (row == start.y()) return col >= start.x();
    if (row == end.y()) return col <= end.x();
    return true;
}

QString GhosttyTerminalWidget::selectedText() const {
    if (!has_selection_ || viewport_cells_.isEmpty()) return {};
    QPoint start, end;
    normalizedSelection(start, end);
    QStringList pieces;
    for (int r = start.y(); r <= end.y() && r < viewport_cells_.size(); ++r) {
        const int c0 = (r == start.y()) ? start.x() : 0;
        const int c1 = (r == end.y()) ? end.x() : cols_ - 1;
        QString slice;
        for (const auto& cell : viewport_cells_[r]) {
            if (cell.column > c1) break;
            if (cell.column + cell.width > c0) slice += cell.text;
        }
        while (slice.endsWith(QChar(' '))) slice.chop(1);
        pieces.append(slice);
    }
    return pieces.join(QChar('\n'));
}

bool GhosttyTerminalWidget::copySelection() {
    const QString result = selectedText();
    if (result.isEmpty()) return false;
    if (auto* clip = QApplication::clipboard()) {
        clip->setText(result);
    }
    clearSelection();
    update();
    return true;
}

void GhosttyTerminalWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    if (!terminal_ || !render_state_ || !row_iter_ || !row_cells_) {
        p.fillRect(rect(), QColor(16, 22, 31));
        return;
    }

    if (ghostty_render_state_update(render_state_, terminal_) != GHOSTTY_SUCCESS) {
        p.fillRect(rect(), QColor(16, 22, 31));
        return;
    }

    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(render_state_, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    const QColor default_bg = toQColor(colors.background);
    const QColor default_fg = toQColor(colors.foreground);
    const QColor sel_bg(56, 139, 253, 120);
    p.fillRect(rect(), default_bg);

    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(render_state_, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor);

    uint16_t rs_rows = 0;
    ghostty_render_state_get(render_state_, GHOSTTY_RENDER_STATE_DATA_ROWS, &rs_rows);
    ghostty_render_state_get(render_state_, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter_);

    viewport_cells_.clear();
    viewport_cells_.resize(rs_rows);

    QFont current_font = font_;
    p.setFont(current_font);

    while (ghostty_render_state_row_iterator_next(row_iter_)) {
        int32_t row_y = 0;
        if (ghostty_render_state_row_get(row_iter_, GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y, &row_y) != GHOSTTY_SUCCESS) {
            continue;
        }
        if (row_y < 0 || row_y >= static_cast<int32_t>(rs_rows)) continue;
        if (ghostty_render_state_row_get(row_iter_, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &row_cells_) != GHOSTTY_SUCCESS) {
            continue;
        }

        const int py = pad_y_ + row_y * cell_h_;
        auto& row_cells = viewport_cells_[row_y];
        row_cells.reserve(cols_);
        int col_x = 0;
        while (ghostty_render_state_row_cells_next(row_cells_)) {
            const int px = pad_x_ + col_x * cell_w_;
            GhosttyCell raw{};
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            if (ghostty_render_state_row_cells_get(row_cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw) == GHOSTTY_SUCCESS) {
                ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
            }
            if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                ++col_x;
                continue;
            }
            const int span = (wide == GHOSTTY_CELL_WIDE_WIDE) ? 2 : 1;

            GhosttyColorRgb bg_rgb{};
            QColor bg = (ghostty_render_state_row_cells_get(row_cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg_rgb) == GHOSTTY_SUCCESS)
                ? toQColor(bg_rgb)
                : default_bg;
            GhosttyColorRgb fg_rgb{};
            QColor fg = (ghostty_render_state_row_cells_get(row_cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg_rgb) == GHOSTTY_SUCCESS)
                ? toQColor(fg_rgb)
                : default_fg;

            GhosttyStyle style{};
            bool has_style = false;
            if (ghostty_render_state_row_cells_get(row_cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING, &has_style) == GHOSTTY_SUCCESS && has_style) {
                ghostty_render_state_row_cells_get(row_cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
            }

            if (style.flags & GHOSTTY_STYLE_FLAG_INVERSE) {
                std::swap(bg, fg);
            }
            if (style.flags & GHOSTTY_STYLE_FLAG_FAINT) {
                fg.setAlpha(155);
            }

            if (bg != default_bg) {
                p.fillRect(px, py, cell_w_ * span, cell_h_, bg);
            }
            if (isCellSelected(col_x, row_y) || (span == 2 && isCellSelected(col_x + 1, row_y))) {
                p.fillRect(px, py, cell_w_ * span, cell_h_, sel_bg);
            }

            QString cell_text;
            if (!(style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE)) {
                uint8_t utf8_buf[64];
                GhosttyBuffer buf{utf8_buf, sizeof(utf8_buf), 0};
                if (ghostty_render_state_row_cells_get(row_cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf) == GHOSTTY_SUCCESS &&
                    buf.len > 0) {
                    const bool want_bold = (style.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
                    const bool want_italic = (style.flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;
                    if (current_font.bold() != want_bold || current_font.italic() != want_italic) {
                        current_font.setBold(want_bold);
                        current_font.setItalic(want_italic);
                        p.setFont(current_font);
                    }
                    p.setPen(fg);
                    cell_text = QString::fromUtf8(reinterpret_cast<const char*>(utf8_buf), static_cast<qsizetype>(buf.len));
                    p.drawText(px, py + ascent_, cell_text);
                }
            }
            row_cells.append(ViewportCell{col_x, span, cell_text.isEmpty() ? QStringLiteral(" ") : std::move(cell_text)});

            if (GHOSTTY_STYLE_UNDERLINE(style) != 0) {
                p.setPen(fg);
                p.drawLine(px, py + cell_h_ - 2, px + cell_w_ * span - 1, py + cell_h_ - 2);
            }
            if (style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) {
                p.setPen(fg);
                p.drawLine(px, py + cell_h_ / 2, px + cell_w_ * span - 1, py + cell_h_ / 2);
            }

            ++col_x;
        }
    }

    const bool draw_cursor = cursor.visible && cursor.viewport_has_value && (!cursor.blinking || cursor_blink_on_ || !hasFocus());
    if (draw_cursor) {
        const int cx = pad_x_ + static_cast<int>(cursor.viewport_x) * cell_w_;
        const int cy = pad_y_ + static_cast<int>(cursor.viewport_y) * cell_h_;
        QColor cursor_color = colors.cursor_has_value ? toQColor(colors.cursor) : QColor(88, 166, 255);
        if (!hasFocus()) {
            p.setPen(cursor_color);
            p.setBrush(Qt::NoBrush);
            p.drawRect(cx, cy, cell_w_ - 1, cell_h_ - 1);
        } else {
            switch (cursor.visual_style) {
            case GHOSTTY_CURSOR_STYLE_BAR:
                p.fillRect(cx, cy, 2, cell_h_, cursor_color);
                break;
            case GHOSTTY_CURSOR_STYLE_UNDERLINE:
                p.fillRect(cx, cy + cell_h_ - 2, cell_w_, 2, cursor_color);
                break;
            case GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW:
                p.setPen(cursor_color);
                p.setBrush(Qt::NoBrush);
                p.drawRect(cx, cy, cell_w_ - 1, cell_h_ - 1);
                break;
            case GHOSTTY_CURSOR_STYLE_BLOCK:
            default: {
                QColor translucent = cursor_color;
                translucent.setAlpha(150);
                p.fillRect(cx, cy, cell_w_, cell_h_, translucent);
                break;
            }
            }
        }
    }

    ghostty_render_state_clean(render_state_);
}

void GhosttyTerminalWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncGridSize(true);
}

void GhosttyTerminalWidget::keyPressEvent(QKeyEvent* event) {
    const auto key = event->key();
    const auto mods = event->modifiers();

    // Copy shortcut: Ctrl+Shift+C (or Ctrl+Insert)
    if (((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier) && key == Qt::Key_C) ||
        ((mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier) && key == Qt::Key_Insert)) {
        copySelection();
        event->accept();
        return;
    }

    // Paste shortcuts: Ctrl+Shift+V or Shift+Insert
    if (((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier) && key == Qt::Key_V) ||
        ((mods & Qt::ShiftModifier) && !(mods & Qt::ControlModifier) && key == Qt::Key_Insert)) {
        pasteClipboard();
        event->accept();
        return;
    }

    // Font zoom: Ctrl+= / Ctrl+- / Ctrl+0
    if ((mods & Qt::ControlModifier) && !(mods & Qt::AltModifier)) {
        if (key == Qt::Key_Equal || key == Qt::Key_Plus) {
            adjustFontSize(+1);
            event->accept();
            return;
        }
        if (key == Qt::Key_Minus) {
            adjustFontSize(-1);
            event->accept();
            return;
        }
        if (key == Qt::Key_0) {
            adjustFontSize(0);
            event->accept();
            return;
        }
    }

    // Ctrl+<letter> control codes (Ctrl+A .. Ctrl+Z)
    if ((mods & Qt::ControlModifier) && !(mods & Qt::AltModifier) && !(mods & Qt::ShiftModifier)) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            const char ch = static_cast<char>(key - Qt::Key_A + 1);
            sendBytes(QByteArray(1, ch));
            event->accept();
            return;
        }
        if (key == Qt::Key_BracketLeft) {
            sendBytes(QByteArray(1, '\x1b'));
            event->accept();
            return;
        }
        if (key == Qt::Key_Backslash) {
            sendBytes(QByteArray(1, '\x1c'));
            event->accept();
            return;
        }
        if (key == Qt::Key_BracketRight) {
            sendBytes(QByteArray(1, '\x1d'));
            event->accept();
            return;
        }
    }

    QByteArray seq;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        seq = QByteArrayLiteral("\r");
        break;
    case Qt::Key_Backspace:
        seq = (mods & Qt::AltModifier) ? QByteArrayLiteral("\x1b\x7f") : QByteArrayLiteral("\x7f");
        break;
    case Qt::Key_Tab:
        seq = QByteArrayLiteral("\t");
        break;
    case Qt::Key_Backtab:
        seq = QByteArrayLiteral("\x1b[Z");
        break;
    case Qt::Key_Escape:
        seq = QByteArrayLiteral("\x1b");
        break;
    case Qt::Key_Up:
        seq = encodeCsiCursorKey('A', mods);
        break;
    case Qt::Key_Down:
        seq = encodeCsiCursorKey('B', mods);
        break;
    case Qt::Key_Right:
        seq = encodeCsiCursorKey('C', mods);
        break;
    case Qt::Key_Left:
        seq = encodeCsiCursorKey('D', mods);
        break;
    case Qt::Key_Home:
        seq = encodeCsiCursorKey('H', mods);
        break;
    case Qt::Key_End:
        seq = encodeCsiCursorKey('F', mods);
        break;
    case Qt::Key_Insert:
        seq = encodeTildeKey(2, mods);
        break;
    case Qt::Key_Delete:
        seq = encodeTildeKey(3, mods);
        break;
    case Qt::Key_PageUp:
        seq = encodeTildeKey(5, mods);
        break;
    case Qt::Key_PageDown:
        seq = encodeTildeKey(6, mods);
        break;
    case Qt::Key_F1: seq = QByteArrayLiteral("\x1bOP"); break;
    case Qt::Key_F2: seq = QByteArrayLiteral("\x1bOQ"); break;
    case Qt::Key_F3: seq = QByteArrayLiteral("\x1bOR"); break;
    case Qt::Key_F4: seq = QByteArrayLiteral("\x1bOS"); break;
    case Qt::Key_F5: seq = encodeTildeKey(15, mods); break;
    case Qt::Key_F6: seq = encodeTildeKey(17, mods); break;
    case Qt::Key_F7: seq = encodeTildeKey(18, mods); break;
    case Qt::Key_F8: seq = encodeTildeKey(19, mods); break;
    case Qt::Key_F9: seq = encodeTildeKey(20, mods); break;
    case Qt::Key_F10: seq = encodeTildeKey(21, mods); break;
    case Qt::Key_F11: seq = encodeTildeKey(23, mods); break;
    case Qt::Key_F12: seq = encodeTildeKey(24, mods); break;
    default:
        break;
    }

    if (!seq.isEmpty()) {
        sendBytes(seq);
        event->accept();
        return;
    }

    const QString text = event->text();
    if (!text.isEmpty()) {
        QByteArray utf8 = text.toUtf8();
        if (mods & Qt::AltModifier) utf8.prepend('\x1b');
        sendBytes(utf8);
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void GhosttyTerminalWidget::inputMethodEvent(QInputMethodEvent* event) {
    const QString commit = event->commitString();
    if (!commit.isEmpty()) {
        sendBytes(commit.toUtf8());
    }
    event->accept();
}

void GhosttyTerminalWidget::wheelEvent(QWheelEvent* event) {
    if (!terminal_) {
        QWidget::wheelEvent(event);
        return;
    }
    const int delta_y = event->angleDelta().y();
    if (delta_y != 0) {
        if (event->modifiers() & Qt::ControlModifier) {
            adjustFontSize(delta_y > 0 ? +1 : -1);
            event->accept();
            return;
        }
        GhosttyTerminalScrollViewport sv{};
        sv.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA;
        sv.value.delta = delta_y > 0 ? -3 : 3;
        ghostty_terminal_scroll_viewport(terminal_, sv);
        update();
    }
    event->accept();
}

void GhosttyTerminalWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::RightButton) {
        if (!copySelection()) {
            pasteClipboard();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        selecting_ = true;
        has_selection_ = false;
        sel_anchor_ = pixelToCell(event->pos());
        sel_current_ = sel_anchor_;
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GhosttyTerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    if (selecting_) {
        sel_current_ = pixelToCell(event->pos());
        has_selection_ = (sel_current_ != sel_anchor_);
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void GhosttyTerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && selecting_) {
        selecting_ = false;
        sel_current_ = pixelToCell(event->pos());
        has_selection_ = (sel_current_ != sel_anchor_);
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GhosttyTerminalWidget::focusInEvent(QFocusEvent* event) {
    cursor_blink_on_ = true;
    update();
    QWidget::focusInEvent(event);
}

void GhosttyTerminalWidget::focusOutEvent(QFocusEvent* event) {
    update();
    QWidget::focusOutEvent(event);
}

bool GhosttyTerminalWidget::focusNextPrevChild(bool) {
    return false;
}
