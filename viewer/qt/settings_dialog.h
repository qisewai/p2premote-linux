#pragma once

#include "app_settings.h"
#include "frameless_chrome.h"

#include <QHash>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;

class SettingsDialog final : public FramelessDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private:
    struct PluginFieldBinding {
        PluginSettingField field;
        QWidget* widget{};
    };

    struct PluginPageBinding {
        PluginSettingSpec spec;
        QVector<PluginFieldBinding> fields;
    };

    QWidget* buildAppearancePage();
    QWidget* buildGeneralPage();
    QWidget* buildStreamAudioPage();
    QWidget* buildPluginsOverviewPage(const QVector<PluginSettingSpec>& plugins);
    QWidget* buildPluginSettingsPage(const PluginSettingSpec& spec);
    void loadFromSettings();
    AppSettingsData collectAppSettings() const;
    void previewAppearance();
    void saveAll();
    void resetAll();

    QListWidget* nav_{};
    QStackedWidget* pages_{};
    QLabel* save_hint_{};

    // Appearance controls
    QComboBox* theme_combo_{};
    QComboBox* accent_combo_{};
    QComboBox* font_combo_{};

    // General controls
    QCheckBox* close_to_tray_{};
    QCheckBox* auto_receive_{};
    QLineEdit* default_server_{};

    // Stream & Audio controls
    QComboBox* quality_combo_{};
    QComboBox* bitrate_combo_{};
    QCheckBox* show_diagnostics_{};
    QCheckBox* speaker_enabled_{};
    QComboBox* volume_combo_{};
    QCheckBox* mic_fallback_{};

    QVector<PluginPageBinding> plugin_pages_;
    QHash<int, int> nav_to_page_;
    bool loading_{};
};
