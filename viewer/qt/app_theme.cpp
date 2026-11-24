#include "app_theme.h"

#include <QApplication>
#include <QPalette>

namespace {

ThemePalette g_current_palette;

struct AccentTokens {
    const char* main;
    const char* hover;
    const char* pressed;
    const char* soft_dark;
    const char* soft_light;
};

AccentTokens accentFor(const QString& name) {
    if (name == QStringLiteral("emerald")) {
        return {"#10b981", "#059669", "#047857", "#11362c", "#d1fae5"};
    }
    if (name == QStringLiteral("violet")) {
        return {"#8b5cf6", "#7c3aed", "#6d28d9", "#281e45", "#ede9fe"};
    }
    if (name == QStringLiteral("amber")) {
        return {"#f59e0b", "#d97706", "#b45309", "#3b2a10", "#fef3c7"};
    }
    return {"#3b82f6", "#2563eb", "#1d4ed8", "#1c2e4a", "#dbeafe"};
}

} // namespace

ThemePalette AppTheme::paletteFor(const AppSettingsData& settings) {
    ThemePalette p;
    p.font_size = settings.font_size;
    const auto acc = accentFor(settings.accent);
    p.accent = QString::fromLatin1(acc.main);
    p.accent_hover = QString::fromLatin1(acc.hover);
    p.accent_pressed = QString::fromLatin1(acc.pressed);
    p.accent_text = QStringLiteral("#ffffff");

    if (settings.theme == QStringLiteral("light")) {
        p.is_dark = false;
        p.bg = QStringLiteral("#f3f6fa");
        p.surface = QStringLiteral("#ffffff");
        p.surface_alt = QStringLiteral("#f8fafc");
        p.surface_hover = QStringLiteral("#eef3f9");
        p.surface_pressed = QStringLiteral("#e2ebf5");
        p.titlebar_bg = QStringLiteral("#eaf0f6");
        p.border = QStringLiteral("#d6e0eb");
        p.border_strong = QStringLiteral("#b6c6d8");
        p.text = QStringLiteral("#132236");
        p.text_secondary = QStringLiteral("#41556d");
        p.text_muted = QStringLiteral("#64788f");
        p.accent_soft = QString::fromLatin1(acc.soft_light);
        p.danger = QStringLiteral("#dc2626");
        p.danger_hover = QStringLiteral("#b91c1c");
        p.danger_surface = QStringLiteral("#fee2e2");
        p.input_bg = QStringLiteral("#ffffff");
        p.scroll_thumb = QStringLiteral("#c5d2e0");
        p.scroll_hover = QStringLiteral("#9eb3c9");
    } else if (settings.theme == QStringLiteral("midnight")) {
        p.is_dark = true;
        p.bg = QStringLiteral("#0b1120");
        p.surface = QStringLiteral("#111a2e");
        p.surface_alt = QStringLiteral("#16223b");
        p.surface_hover = QStringLiteral("#1e2d4d");
        p.surface_pressed = QStringLiteral("#131d33");
        p.titlebar_bg = QStringLiteral("#0e1628");
        p.border = QStringLiteral("#1f3050");
        p.border_strong = QStringLiteral("#334c78");
        p.text = QStringLiteral("#e8eef8");
        p.text_secondary = QStringLiteral("#a8bad6");
        p.text_muted = QStringLiteral("#7389ab");
        p.accent_soft = QString::fromLatin1(acc.soft_dark);
        p.danger = QStringLiteral("#ef4444");
        p.danger_hover = QStringLiteral("#dc2626");
        p.danger_surface = QStringLiteral("#3b1a24");
        p.input_bg = QStringLiteral("#0d1526");
        p.scroll_thumb = QStringLiteral("#2a3f66");
        p.scroll_hover = QStringLiteral("#3e5b8f");
    } else {
        // Default: Deep Slate Dark
        p.is_dark = true;
        p.bg = QStringLiteral("#11151d");
        p.surface = QStringLiteral("#181e29");
        p.surface_alt = QStringLiteral("#1e2634");
        p.surface_hover = QStringLiteral("#263042");
        p.surface_pressed = QStringLiteral("#151a24");
        p.titlebar_bg = QStringLiteral("#141923");
        p.border = QStringLiteral("#273142");
        p.border_strong = QStringLiteral("#3b4a63");
        p.text = QStringLiteral("#e6edf6");
        p.text_secondary = QStringLiteral("#a6b4c8");
        p.text_muted = QStringLiteral("#7888a0");
        p.accent_soft = QString::fromLatin1(acc.soft_dark);
        p.danger = QStringLiteral("#ef4444");
        p.danger_hover = QStringLiteral("#dc2626");
        p.danger_surface = QStringLiteral("#3b1d26");
        p.input_bg = QStringLiteral("#121721");
        p.scroll_thumb = QStringLiteral("#313d52");
        p.scroll_hover = QStringLiteral("#465773");
    }
    return p;
}

const ThemePalette& AppTheme::current() {
    return g_current_palette;
}

QString AppTheme::buildStyleSheet(const ThemePalette& p) {
    QString qss = QStringLiteral(R"(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: %FONT%px;
}
QWidget {
    color: %TEXT%;
    background: transparent;
}
QDialog, QWidget#accountWindow, QWidget#remoteWindow, QWidget#settingsDialog, QDialog#pluginDialog {
    background: %BG%;
}
QFrame#framelessShell {
    background: %BG%;
    border: 1px solid %BORDER%;
}
QFrame#titleBar, QFrame#remoteTopbar {
    background: %TITLEBAR%;
    border-bottom: 1px solid %BORDER%;
}
QLabel#titleLabel {
    color: %TEXT%;
    font-weight: 600;
    font-size: %FONT_SM%px;
}
QLabel#titleBadge {
    background: %ACCENT%;
    color: %ACCENT_TEXT%;
    border-radius: 4px;
    padding: 1px 6px;
    font-size: 11px;
    font-weight: 700;
}
QPushButton#titleBtn, QPushButton#titleCloseBtn, QPushButton#titleToolBtn {
    background: transparent;
    color: %TEXT_SEC%;
    border: 0;
    border-radius: 5px;
    padding: 4px 10px;
    min-height: 22px;
    font-size: %FONT_SM%px;
}
QPushButton#titleBtn {
    min-width: 34px;
    padding: 4px 0px;
    font-size: 13px;
}
QPushButton#titleCloseBtn {
    min-width: 36px;
    padding: 4px 0px;
    font-size: 13px;
}
QPushButton#titleBtn:hover, QPushButton#titleToolBtn:hover {
    background: %SURF_HOVER%;
    color: %TEXT%;
}
QPushButton#titleCloseBtn:hover {
    background: %DANGER%;
    color: #ffffff;
}
QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget {
    background: transparent;
    border: 0;
}
QLabel#heading {
    color: %TEXT%;
    font-size: %FONT_XL%px;
    font-weight: 600;
}
QLabel#sectionTitle {
    color: %TEXT%;
    font-size: %FONT_LG%px;
    font-weight: 600;
}
QLabel#muted, QLabel#status, QLabel#fieldDesc {
    color: %TEXT_MUTED%;
    font-size: %FONT_SM%px;
}
QFrame#accountCard, QFrame#card, QFrame#settingsCard {
    background: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 10px;
}
QFrame#divider {
    background: %BORDER%;
    max-height: 1px;
    min-height: 1px;
}
QLineEdit, QSpinBox, QDoubleSpinBox {
    background: %INPUT_BG%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    min-height: 28px;
    padding: 3px 10px;
    selection-background-color: %ACCENT%;
    selection-color: %ACCENT_TEXT%;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 1px solid %ACCENT%;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    color: %TEXT_MUTED%;
    background: %SURFACE_ALT%;
}
QComboBox {
    background: %INPUT_BG%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    min-height: 28px;
    padding: 3px 10px;
}
QComboBox:hover, QComboBox:focus {
    border-color: %ACCENT%;
}
QComboBox:disabled {
    color: %TEXT_MUTED%;
    background: %SURFACE_ALT%;
}
QComboBox::drop-down {
    border: 0;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: %SURFACE%;
    color: %TEXT%;
    border: 1px solid %BORDER_STRONG%;
    border-radius: 6px;
    padding: 4px;
    selection-background-color: %ACCENT_SOFT%;
    selection-color: %TEXT%;
    outline: 0;
}
QCheckBox {
    color: %TEXT%;
    spacing: 8px;
}
QCheckBox::indicator {
    width: 17px;
    height: 17px;
    border-radius: 4px;
    border: 1px solid %BORDER_STRONG%;
    background: %INPUT_BG%;
}
QCheckBox::indicator:hover {
    border-color: %ACCENT%;
}
QCheckBox::indicator:checked {
    background: %ACCENT%;
    border-color: %ACCENT%;
}
QPushButton {
    background: %SURFACE_ALT%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 5px 14px;
    min-height: 22px;
}
QPushButton:hover, QPushButton:focus {
    background: %SURF_HOVER%;
    border-color: %BORDER_STRONG%;
}
QPushButton:pressed {
    background: %SURF_PRESS%;
}
QPushButton:disabled {
    background: %SURFACE%;
    color: %TEXT_MUTED%;
    border-color: %BORDER%;
}
QPushButton#primary, QPushButton[variant="primary"] {
    background: %ACCENT%;
    color: %ACCENT_TEXT%;
    border-color: %ACCENT%;
    font-weight: 600;
}
QPushButton#primary:hover, QPushButton#primary:focus,
QPushButton[variant="primary"]:hover, QPushButton[variant="primary"]:focus {
    background: %ACCENT_HOVER%;
    border-color: %ACCENT_HOVER%;
}
QPushButton#primary:pressed, QPushButton[variant="primary"]:pressed {
    background: %ACCENT_PRESS%;
}
QPushButton#primary:disabled, QPushButton[variant="primary"]:disabled {
    background: %SURFACE_ALT%;
    color: %TEXT_MUTED%;
    border-color: %BORDER%;
}
QPushButton[variant="danger"] {
    background: %DANGER_SURF%;
    color: %TEXT%;
    border-color: %DANGER%;
}
QPushButton[variant="danger"]:hover {
    background: %DANGER%;
    color: #ffffff;
}
QPushButton[variant="subtle"] {
    background: transparent;
    border-color: transparent;
    color: %TEXT_SEC%;
}
QPushButton[variant="subtle"]:hover {
    background: %SURF_HOVER%;
    color: %TEXT%;
}
QListWidget#deviceList {
    background: transparent;
    border: 0;
    outline: 0;
}
QListWidget#deviceList::item {
    background: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
    padding: 9px 14px;
    min-height: 36px;
    color: %TEXT%;
}
QListWidget#deviceList::item:hover {
    background: %SURF_HOVER%;
    border-color: %BORDER_STRONG%;
}
QListWidget#deviceList::item:selected {
    background: %ACCENT_SOFT%;
    border-color: %ACCENT%;
    color: %TEXT%;
}
QListWidget#settingsNav {
    background: %TITLEBAR%;
    border: 0;
    border-right: 1px solid %BORDER%;
    padding: 8px 6px;
    outline: 0;
}
QListWidget#settingsNav::item {
    color: %TEXT_SEC%;
    border-radius: 6px;
    padding: 8px 12px;
    margin: 2px 2px;
}
QListWidget#settingsNav::item:hover {
    background: %SURF_HOVER%;
    color: %TEXT%;
}
QListWidget#settingsNav::item:selected {
    background: %ACCENT_SOFT%;
    color: %TEXT%;
    font-weight: 600;
}
QListWidget#settingsNav::item:disabled {
    color: %TEXT_MUTED%;
    font-size: 11px;
    font-weight: 700;
    padding: 10px 12px 4px 12px;
    background: transparent;
}
QTreeWidget {
    background: %SURFACE%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
    outline: 0;
    alternate-background-color: %SURFACE_ALT%;
}
QTreeWidget::item {
    padding: 4px 6px;
    min-height: 24px;
}
QTreeWidget::item:hover {
    background: %SURF_HOVER%;
}
QTreeWidget::item:selected {
    background: %ACCENT_SOFT%;
    color: %TEXT%;
}
QHeaderView::section {
    background: %SURFACE_ALT%;
    color: %TEXT_SEC%;
    border: 0;
    border-bottom: 1px solid %BORDER%;
    border-right: 1px solid %BORDER%;
    padding: 6px 8px;
    font-weight: 600;
}
QProgressBar {
    background: %INPUT_BG%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    text-align: center;
    min-height: 22px;
    font-weight: 500;
}
QProgressBar::chunk {
    background: %ACCENT%;
    border-radius: 5px;
}
QSlider::groove:horizontal {
    height: 6px;
    background: %BORDER%;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: %ACCENT%;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    width: 16px;
    height: 16px;
    margin: -5px 0;
    border-radius: 8px;
    background: %ACCENT%;
    border: 2px solid %SURFACE%;
}
QSplitter::handle {
    background: %BORDER%;
    width: 3px;
    height: 3px;
    border-radius: 1px;
}
QTabBar#remoteTabs {
    background: transparent;
    border: 0;
}
QTabBar#remoteTabs::tab {
    background: transparent;
    color: %TEXT_SEC%;
    border: 0;
    border-top: 2px solid transparent;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 7px 14px;
    margin-right: 3px;
    min-width: 110px;
    max-width: 230px;
    font-size: %FONT_SM%px;
}
QTabBar#remoteTabs::tab:hover {
    background: %SURF_HOVER%;
    color: %TEXT%;
}
QTabBar#remoteTabs::tab:selected {
    background: %SURFACE_ALT%;
    color: %TEXT%;
    border-top: 2px solid %ACCENT%;
    font-weight: 600;
}
QPushButton#barAddBtn {
    background: transparent;
    color: %TEXT_SEC%;
    border: 0;
    border-radius: 5px;
    padding: 3px 8px;
    min-height: 20px;
    font-size: 16px;
    font-weight: 600;
}
QPushButton#barAddBtn:hover {
    background: %SURF_HOVER%;
    color: %TEXT%;
}
QPushButton#barMenuBtn {
    background: %SURFACE_ALT%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 4px 10px;
    min-height: 20px;
    font-size: %FONT_SM%px;
}
QPushButton#barMenuBtn:hover, QPushButton#barMenuBtn:focus {
    background: %SURF_HOVER%;
    border-color: %BORDER_STRONG%;
}
QPushButton#barMenuBtn:disabled {
    background: %SURFACE%;
    color: %TEXT_MUTED%;
    border-color: %BORDER%;
}
QPushButton#barMenuBtn[micLive="true"] {
    background: %DANGER_SURF%;
    color: %TEXT%;
    border-color: %DANGER%;
}
QFrame#remoteStatusbar {
    background: %TITLEBAR%;
    border-top: 1px solid %BORDER%;
}
QLabel#remoteStatusText {
    color: %TEXT_MUTED%;
    font-size: 12px;
}
QWidget#video {
    background: #000000;
}
QMenu {
    background: %SURFACE%;
    color: %TEXT%;
    border: 1px solid %BORDER_STRONG%;
    border-radius: 8px;
    padding: 5px 0;
    font-size: %FONT_SM%px;
}
QMenu::item {
    padding: 6px 24px 6px 26px;
}
QMenu::item:selected {
    background: %ACCENT_SOFT%;
    color: %TEXT%;
}
QMenu::item:disabled {
    color: %TEXT_MUTED%;
}
QMenu::separator {
    height: 1px;
    background: %BORDER%;
    margin: 4px 10px;
}
QToolTip {
    background: %SURFACE%;
    color: %TEXT%;
    border: 1px solid %BORDER_STRONG%;
    border-radius: 6px;
    padding: 4px 8px;
}
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 2px 0;
}
QScrollBar::handle:vertical {
    background: %SCROLL_THUMB%;
    min-height: 28px;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background: %SCROLL_HOVER%;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    height: 0;
    background: transparent;
}
QScrollBar:horizontal {
    background: transparent;
    height: 8px;
    margin: 0 2px;
}
QScrollBar::handle:horizontal {
    background: %SCROLL_THUMB%;
    min-width: 28px;
    border-radius: 4px;
}
QScrollBar::handle:horizontal:hover {
    background: %SCROLL_HOVER%;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    width: 0;
    background: transparent;
}
QMessageBox {
    background: %BG%;
}
)");

    qss.replace(QStringLiteral("%FONT%"), QString::number(p.font_size));
    qss.replace(QStringLiteral("%FONT_SM%"), QString::number(std::max(12, p.font_size - 1)));
    qss.replace(QStringLiteral("%FONT_LG%"), QString::number(p.font_size + 2));
    qss.replace(QStringLiteral("%FONT_XL%"), QString::number(p.font_size + 8));
    qss.replace(QStringLiteral("%BG%"), p.bg);
    qss.replace(QStringLiteral("%SURFACE%"), p.surface);
    qss.replace(QStringLiteral("%SURFACE_ALT%"), p.surface_alt);
    qss.replace(QStringLiteral("%SURF_HOVER%"), p.surface_hover);
    qss.replace(QStringLiteral("%SURF_PRESS%"), p.surface_pressed);
    qss.replace(QStringLiteral("%TITLEBAR%"), p.titlebar_bg);
    qss.replace(QStringLiteral("%BORDER%"), p.border);
    qss.replace(QStringLiteral("%BORDER_STRONG%"), p.border_strong);
    qss.replace(QStringLiteral("%TEXT%"), p.text);
    qss.replace(QStringLiteral("%TEXT_SEC%"), p.text_secondary);
    qss.replace(QStringLiteral("%TEXT_MUTED%"), p.text_muted);
    qss.replace(QStringLiteral("%ACCENT%"), p.accent);
    qss.replace(QStringLiteral("%ACCENT_HOVER%"), p.accent_hover);
    qss.replace(QStringLiteral("%ACCENT_PRESS%"), p.accent_pressed);
    qss.replace(QStringLiteral("%ACCENT_SOFT%"), p.accent_soft);
    qss.replace(QStringLiteral("%ACCENT_TEXT%"), p.accent_text);
    qss.replace(QStringLiteral("%DANGER%"), p.danger);
    qss.replace(QStringLiteral("%DANGER_SURF%"), p.danger_surface);
    qss.replace(QStringLiteral("%INPUT_BG%"), p.input_bg);
    qss.replace(QStringLiteral("%SCROLL_THUMB%"), p.scroll_thumb);
    qss.replace(QStringLiteral("%SCROLL_HOVER%"), p.scroll_hover);
    return qss;
}

void AppTheme::apply(const AppSettingsData& settings) {
    g_current_palette = paletteFor(settings);
    if (!qApp) return;

    QPalette pal;
    pal.setColor(QPalette::Window, QColor(g_current_palette.bg));
    pal.setColor(QPalette::WindowText, QColor(g_current_palette.text));
    pal.setColor(QPalette::Base, QColor(g_current_palette.input_bg));
    pal.setColor(QPalette::AlternateBase, QColor(g_current_palette.surface_alt));
    pal.setColor(QPalette::Text, QColor(g_current_palette.text));
    pal.setColor(QPalette::Button, QColor(g_current_palette.surface_alt));
    pal.setColor(QPalette::ButtonText, QColor(g_current_palette.text));
    pal.setColor(QPalette::Highlight, QColor(g_current_palette.accent));
    pal.setColor(QPalette::HighlightedText, QColor(g_current_palette.accent_text));
    pal.setColor(QPalette::PlaceholderText, QColor(g_current_palette.text_muted));
    qApp->setPalette(pal);
    qApp->setStyleSheet(buildStyleSheet(g_current_palette));
}
