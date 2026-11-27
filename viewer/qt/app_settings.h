#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <optional>

struct AppSettingsData {
    // Appearance
    QString theme{QStringLiteral("dark")};     // "dark" | "midnight" | "light"
    QString accent{QStringLiteral("blue")};    // "blue" | "emerald" | "violet" | "amber"
    int font_size{14};                         // 13 | 14 | 15

    // General
    bool close_to_tray{true};
    bool auto_receive_on_login{true};
    QString default_server;

    // Updates
    bool auto_check_updates{true};
    bool auto_download_updates{true};
    QString update_mirror_url;

    // Stream & Audio defaults
    int default_quality{1};        // 0=720p, 1=1080p, 2=1440p
    int default_bitrate{1};        // 0=2M, 1=8M, 2=16M
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
    enum class Placement { none, embed, page };
    QString id;
    QString name;
    QString version;
    QString description;
    bool enabled{true};
    QStringList roles;
    QStringList permissions;
    QVector<PluginSettingField> fields;
    QMap<QString, QStringList> agent_reports;
    Placement placement{Placement::none};
    QString page;
    QString page_title;
    QString section;
    int order{50};
    bool extension_page{};
    bool host_config{};
};

class AppSettings final : public QObject {
    Q_OBJECT

public:
    static AppSettings& instance();

    const AppSettingsData& data() const noexcept { return data_; }
    void reload();
    void save(const AppSettingsData& next);

    static QString pluginsDirectory();
    static QString pluginConfigDirectory();
    static QVector<PluginSettingSpec> discoverPlugins();
    static QJsonObject loadPluginConfig(const QString& plugin_id, bool host = false);
    static bool savePluginConfig(const QString& plugin_id, const QJsonObject& config, bool host);
    static std::optional<bool> overlayVisibility(const QString& plugin_id);
    static void setOverlayVisibility(const QString& plugin_id, bool visible);

signals:
    void changed(const AppSettingsData& settings);
    void pluginConfigChanged(const QString& plugin_id);

private:
    AppSettings();
    AppSettingsData data_;
};
