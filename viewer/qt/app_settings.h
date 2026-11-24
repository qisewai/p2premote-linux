#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct AppSettingsData {
    // Appearance
    QString theme{QStringLiteral("dark")};     // "dark" | "midnight" | "light"
    QString accent{QStringLiteral("blue")};    // "blue" | "emerald" | "violet" | "amber"
    int font_size{14};                         // 13 | 14 | 15

    // General
    bool close_to_tray{true};
    bool auto_receive_on_login{true};
    QString default_server;

    // Stream & Audio defaults
    int default_quality{1};        // 0=720p, 1=1080p, 2=1440p
    int default_bitrate{1};        // 0=4M, 1=8M, 2=16M
    bool show_diagnostics{true};
    bool speaker_enabled{true};
    int speaker_volume{100};       // 25 | 50 | 75 | 100
    bool mic_fallback{true};

    bool operator==(const AppSettingsData&) const = default;
};

struct PluginSettingField {
    QString key;
    QString type; // "bool" | "string" | "int" | "number" | "select"
    QString label;
    QString description;
    QString placeholder;
    QString suffix;
    bool secret{};
    QVariant default_value;
    double min_value{0.0};
    double max_value{100.0};
    double step_value{1.0};
    int decimals{1};
    QVector<QPair<QString, QString>> options; // (value, label)
};

struct PluginSettingSpec {
    QString id;
    QString name;
    QString version;
    QString description;
    bool enabled{true};
    QStringList roles;
    QStringList permissions;
    QVector<PluginSettingField> fields;
};

class AppSettings final : public QObject {
    Q_OBJECT

public:
    static AppSettings& instance();

    const AppSettingsData& data() const noexcept { return data_; }
    void reload();
    void save(const AppSettingsData& next);
    void resetDefaults();

    static QString pluginsDirectory();
    static QString pluginConfigDirectory();
    static QVector<PluginSettingSpec> discoverPlugins();
    static QJsonObject loadPluginConfig(const QString& plugin_id);
    static bool savePluginConfig(const QString& plugin_id, const QJsonObject& config);

signals:
    void changed(const AppSettingsData& settings);
    void pluginConfigChanged(const QString& plugin_id);

private:
    AppSettings();
    AppSettingsData data_;
};
