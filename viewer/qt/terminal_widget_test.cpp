#include "terminal_widget.h"

#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QResizeEvent>

#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    GhosttyTerminalWidget terminal;
    const QSize size(1400, 500);
    terminal.resize(size);
    QResizeEvent resize(size, QSize());
    QApplication::sendEvent(&terminal, &resize);

    terminal.feed(QStringLiteral("A中🙂Z").toUtf8());
    QImage frame(size, QImage::Format_ARGB32);
    terminal.render(&frame);
    const int cell_width = (size.width() - 16) / terminal.columns();
    const auto select = [&](int start, int end) {
        const QPointF from(9 + start * cell_width, 9);
        const QPointF to(9 + end * cell_width, 9);
        QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&terminal, &press);
        QApplication::sendEvent(&terminal, &release);
    };
    select(2, 5);
    if (terminal.selectedText() != QStringLiteral("中🙂Z")) {
        std::cerr << "Selected text did not follow terminal cells\n";
        return 1;
    }

    terminal.setInputSession(7);
    QList<quint32> sequences;
    QByteArray sent;
    QObject::connect(&terminal, &GhosttyTerminalWidget::inputChunk, &terminal,
        [&](quint32 session, quint32 seq, const QByteArray& bytes) {
            if (session != 7) return;
            sequences.append(seq);
            sent += bytes;
        });
    terminal.clearDisplay();
    if (!sequences.isEmpty() || !terminal.selectedText().isEmpty()) {
        std::cerr << "Clearing the display changed terminal input or left the old selection\n";
        return 1;
    }

    QInputMethodEvent paste;
    const QString input(70 * 1024, QChar('x'));
    paste.setCommitString(input);
    QApplication::sendEvent(&terminal, &paste);
    if (sent.size() != 8192 || sequences.size() != 1) {
        std::cerr << "The widget sent multiple chunks before acknowledgment\n";
        return 1;
    }
    terminal.acknowledgeInput(sequences.front() + 1);
    if (sequences.size() != 1) {
        std::cerr << "An unrelated acknowledgment released terminal input\n";
        return 1;
    }
    for (qsizetype i = 0; i < sequences.size() && i < 16; ++i) terminal.acknowledgeInput(sequences[i]);
    if (sent != input.toUtf8() || sequences.size() != 9) {
        std::cerr << "Input was lost across acknowledged chunks\n";
        return 1;
    }
    return 0;
}
