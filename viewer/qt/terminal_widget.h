#pragma once

#include <QByteArray>
#include <QFont>
#include <QList>
#include <QPoint>
#include <QTimer>
#include <QWidget>

#include <ghostty/vt.h>

class GhosttyTerminalWidget final : public QWidget {
    Q_OBJECT

public:
    explicit GhosttyTerminalWidget(QWidget* parent = nullptr);
    ~GhosttyTerminalWidget() override;

    void feed(const QByteArray& bytes);
    void clearDisplay();
    void resetTerminal();
    void setInputSession(quint32 session);
    void acknowledgeInput(quint32 sequence);
    void interrupt();
    QString selectedText() const;

    int columns() const noexcept { return cols_; }
    int rows() const noexcept { return rows_; }

signals:
    void inputChunk(quint32 session, quint32 sequence, const QByteArray& bytes);
    void resized(int cols, int rows);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    struct ViewportCell {
        int column;
        int width;
        QString text;
    };

    static void onWritePty(GhosttyTerminal terminal, void* userdata, const uint8_t* data, size_t len);
    void updateMetrics();
    void syncGridSize(bool notify);
    void sendBytes(const QByteArray& bytes);
    void queueInput(const QByteArray& bytes);
    void sendPendingInput();
    void pasteClipboard();
    bool copySelection();
    void clearSelection();
    QPoint pixelToCell(const QPoint& pos) const noexcept;
    bool isCellSelected(int col, int row) const noexcept;
    void normalizedSelection(QPoint& start, QPoint& end) const noexcept;
    void adjustFontSize(int delta_pt);

    GhosttyTerminal terminal_{nullptr};
    GhosttyRenderState render_state_{nullptr};
    GhosttyRenderStateRowIterator row_iter_{nullptr};
    GhosttyRenderStateRowCells row_cells_{nullptr};

    static constexpr int base_font_pt_ = 11;
    QFont font_;
    int cell_w_{9};
    int cell_h_{18};
    int ascent_{14};
    int pad_x_{8};
    int pad_y_{6};
    int cols_{100};
    int rows_{30};

    bool selecting_{false};
    bool has_selection_{false};
    QPoint sel_anchor_{};
    QPoint sel_current_{};
    QList<QList<ViewportCell>> viewport_cells_;

    QTimer input_retry_;
    QByteArray pending_input_;
    qsizetype in_flight_size_{};
    quint32 input_session_{};
    quint32 input_sequence_{};
    QTimer blink_timer_;
    bool cursor_blink_on_{true};
};
