#include "frameless_chrome.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>

#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwcpRound = 2;

} // namespace

void applyWin32FramelessChrome(QWidget* window, bool allow_maximize) {
    if (window->isFullScreen()) return;
    const auto hwnd = reinterpret_cast<HWND>(window->winId());

    // Qt keeps its frameless geometry while Windows handles window transitions.
    const auto previous_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto style = (previous_style & ~static_cast<LONG_PTR>(WS_MAXIMIZEBOX)) |
        WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
        (allow_maximize ? WS_MAXIMIZEBOX : 0);
    if (style == previous_style) return;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    const MARGINS frame{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &frame);
    const DWORD pref = kDwmwcpRound;
    DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &pref, sizeof(pref));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool handleWin32FramelessChrome(QWidget* window, void* native_msg, qintptr* result) {
    const auto* msg = static_cast<const MSG*>(native_msg);
    if (msg->message == WM_NCCALCSIZE && msg->wParam) {
        // Use the whole window as client area. Maximized windows have an
        // invisible resize border that extends beyond the monitor work area.
        if (IsZoomed(msg->hwnd) && (GetWindowLongPtrW(msg->hwnd, GWL_STYLE) & WS_THICKFRAME)) {
            auto& client = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam)->rgrc[0];
            const auto monitor = MonitorFromRect(&client, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            RECT visible{};
            if (GetMonitorInfoW(monitor, &info) && IntersectRect(&visible, &client, &info.rcWork)) {
                client = visible;
            }
        }
        *result = 0;
        return true;
    }
    if (msg->message != WM_NCHITTEST || window->isMaximized() || window->isFullScreen()) return false;

    const auto dpr = window->devicePixelRatioF();
    const int margin = std::max(4, static_cast<int>(6 * dpr));
    RECT rect{};
    if (!GetWindowRect(msg->hwnd, &rect)) return false;

    const int x = GET_X_LPARAM(msg->lParam);
    const int y = GET_Y_LPARAM(msg->lParam);
    const bool left = x >= rect.left && x < rect.left + margin;
    const bool right = x <= rect.right && x > rect.right - margin;
    const bool top = y >= rect.top && y < rect.top + margin;
    const bool bottom = y <= rect.bottom && y > rect.bottom - margin;

    if (top && left) { *result = HTTOPLEFT; return true; }
    if (top && right) { *result = HTTOPRIGHT; return true; }
    if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
    if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
    if (left) { *result = HTLEFT; return true; }
    if (right) { *result = HTRIGHT; return true; }
    if (top) { *result = HTTOP; return true; }
    if (bottom) { *result = HTBOTTOM; return true; }
    return false;
}

void minimizeFramelessWindow(QWidget* window) {
    SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_SYSCOMMAND, SC_MINIMIZE, 0);
}

void toggleFramelessMaximize(QWidget* window) {
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    SendMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
}

TitleBar::TitleBar(QWidget* host_window, const QString& title, bool show_maximize,
    bool show_badge, QWidget* parent)
    : QFrame(parent), host_(host_window), allow_maximize_(show_maximize) {
    setObjectName(QStringLiteral("titleBar"));
    setFixedHeight(38);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(12, 0, 6, 0);
    layout_->setSpacing(8);

    if (show_badge) {
        auto* badge = new QLabel(QStringLiteral("P2P"), this);
        badge->setObjectName(QStringLiteral("titleBadge"));
        layout_->addWidget(badge);
    }

    title_label_ = new QLabel(title, this);
    title_label_->setObjectName(QStringLiteral("titleLabel"));
    title_label_->setTextFormat(Qt::PlainText);
    layout_->addWidget(title_label_);
    layout_->addStretch(1);

    tools_layout_ = new QHBoxLayout;
    tools_layout_->setContentsMargins(0, 0, 4, 0);
    tools_layout_->setSpacing(4);
    layout_->addLayout(tools_layout_);

    min_btn_ = new QPushButton(QStringLiteral("—"), this);
    min_btn_->setObjectName(QStringLiteral("titleBtn"));
    min_btn_->setToolTip(QStringLiteral("最小化"));
    min_btn_->setFocusPolicy(Qt::NoFocus);
    layout_->addWidget(min_btn_);
    connect(min_btn_, &QPushButton::clicked, this, [this] {
        if (host_) minimizeFramelessWindow(host_);
    });

    if (allow_maximize_) {
        max_btn_ = new QPushButton(QStringLiteral("□"), this);
        max_btn_->setObjectName(QStringLiteral("titleBtn"));
        max_btn_->setToolTip(QStringLiteral("最大化"));
        max_btn_->setFocusPolicy(Qt::NoFocus);
        layout_->addWidget(max_btn_);
        connect(max_btn_, &QPushButton::clicked, this, [this] {
            if (host_) toggleFramelessMaximize(host_);
        });
    }

    close_btn_ = new QPushButton(QStringLiteral("✕"), this);
    close_btn_->setObjectName(QStringLiteral("titleCloseBtn"));
    close_btn_->setToolTip(QStringLiteral("关闭"));
    close_btn_->setFocusPolicy(Qt::NoFocus);
    layout_->addWidget(close_btn_);
    connect(close_btn_, &QPushButton::clicked, this, [this] {
        if (host_) host_->close();
    });
}

void TitleBar::setTitle(const QString& title) {
    if (title_label_) title_label_->setText(title);
}

void TitleBar::addToolWidget(QWidget* widget) {
    if (tools_layout_ && widget) tools_layout_->addWidget(widget);
}

void TitleBar::syncMaximizeState() {
    if (!max_btn_ || !host_) return;
    const bool maxed = host_->isMaximized();
    max_btn_->setText(maxed ? QStringLiteral("❐") : QStringLiteral("□"));
    max_btn_->setToolTip(maxed ? QStringLiteral("还原") : QStringLiteral("最大化"));
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && host_ && host_->windowHandle()) {
        if (host_->windowHandle()->startSystemMove()) {
            event->accept();
            return;
        }
    }
    QFrame::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && allow_maximize_ && host_) {
        toggleFramelessMaximize(host_);
        event->accept();
        return;
    }
    QFrame::mouseDoubleClickEvent(event);
}

FramelessDialog::FramelessDialog(const QString& title, bool allow_maximize, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint), allow_maximize_(allow_maximize) {
    setWindowTitle(title);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    shell_ = new QFrame(this);
    shell_->setObjectName(QStringLiteral("framelessShell"));
    auto* shell_layout = new QVBoxLayout(shell_);
    shell_layout->setContentsMargins(1, 1, 1, 1);
    shell_layout->setSpacing(0);

    title_bar_ = new TitleBar(this, title, allow_maximize, false, shell_);
    shell_layout->addWidget(title_bar_);

    content_ = new QWidget(shell_);
    content_layout_ = new QVBoxLayout(content_);
    content_layout_->setContentsMargins(16, 14, 16, 14);
    content_layout_->setSpacing(10);
    shell_layout->addWidget(content_, 1);

    outer->addWidget(shell_);
}

void FramelessDialog::setDialogTitle(const QString& title) {
    setWindowTitle(title);
    if (title_bar_) title_bar_->setTitle(title);
}

void FramelessDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    applyWin32FramelessChrome(this, allow_maximize_);
    if (title_bar_) title_bar_->syncMaximizeState();
}

void FramelessDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && title_bar_) {
        title_bar_->syncMaximizeState();
    }
}

bool FramelessDialog::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
    if (handleWin32FramelessChrome(this, message, result)) return true;
    return QDialog::nativeEvent(type, message, result);
}
