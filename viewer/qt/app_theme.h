#pragma once

#include "app_settings.h"

#include <QColor>
#include <QString>

struct ThemePalette {
    bool is_dark{true};
    QString bg;
    QString surface;
    QString surface_alt;
    QString surface_hover;
    QString surface_pressed;
    QString titlebar_bg;
    QString border;
    QString border_strong;
    QString text;
    QString text_secondary;
    QString text_muted;
    QString accent;
    QString accent_hover;
    QString accent_pressed;
    QString accent_soft;
    QString accent_text;
    QString danger;
    QString danger_hover;
    QString danger_surface;
    QString input_bg;
    QString scroll_thumb;
    QString scroll_hover;
    int font_size{14};
};

class AppTheme {
public:
    static ThemePalette paletteFor(const AppSettingsData& settings);
    static const ThemePalette& current();
    static QString buildStyleSheet(const ThemePalette& p);
    static void apply(const AppSettingsData& settings);
};
