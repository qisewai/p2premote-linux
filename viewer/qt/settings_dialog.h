#pragma once

#include "app_settings.h"
#include "frameless_chrome.h"

#include <QHash>
#include <QJsonValue>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QStackedWidget;

class SettingsDialog final : public FramelessDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private:
    struct PluginFieldBinding {
        PluginSettingField field;
        QWidget* widget{};
        QJsonValue initial;
    };

    struct PluginCardBinding {
        QString id;
        QWidget* card{};
        int nav_row{};
        QVector<PluginFieldBinding> fields;
    };

    QScrollArea* buildAppearancePage();
    QScrollArea* buildGeneralPage();
    QScrollArea* buildStreamAudioPage();
    QScrollArea* buildPluginsOverviewPage(const QVector<PluginSettingSpec>& plugins);
    void addPluginCard(const PluginSettingSpec& spec, int nav_row);
    void showPluginConfig(const QString& id);
    static QJsonValue fieldValue(const PluginFieldBinding& binding);
    void loadAppControls(const AppSettingsData& data);
    AppSettingsData collectAppSettings() const;
    void previewAppearance();
    bool saveAll();
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

    QVector<PluginCardBinding> plugin_cards_;
    QHash<int, int> nav_to_page_;
    bool loading_{};
    bool plugin_reset_{};
};
