#pragma once

#include <QDialog>
#include <QFrame>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

void applyWin32FramelessChrome(QWidget* window, bool allow_maximize = true);
bool handleWin32FramelessChrome(QWidget* window, void* native_msg, qintptr* result);
void minimizeFramelessWindow(QWidget* window);
void toggleFramelessMaximize(QWidget* window);

class TitleBar final : public QFrame {
    Q_OBJECT

public:
    explicit TitleBar(QWidget* host_window, const QString& title, bool show_maximize = true,
        bool show_badge = false, QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void addToolWidget(QWidget* widget);
    void syncMaximizeState();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QWidget* host_{};
    QHBoxLayout* layout_{};
    QHBoxLayout* tools_layout_{};
    QLabel* title_label_{};
    QPushButton* min_btn_{};
    QPushButton* max_btn_{};
    QPushButton* close_btn_{};
    bool allow_maximize_{true};
};

class FramelessDialog : public QDialog {
    Q_OBJECT

public:
    explicit FramelessDialog(const QString& title, bool allow_maximize = true, QWidget* parent = nullptr);

    void setDialogTitle(const QString& title);
    TitleBar* titleBar() const noexcept { return title_bar_; }
    QWidget* contentWidget() const noexcept { return content_; }
    QVBoxLayout* contentLayout() const noexcept { return content_layout_; }

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

private:
    QFrame* shell_{};
    TitleBar* title_bar_{};
    QWidget* content_{};
    QVBoxLayout* content_layout_{};
    bool allow_maximize_{true};
};
