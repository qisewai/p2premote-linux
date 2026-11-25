#include "app_settings.h"

#include <p2premote/core_api.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace {

bool isValidPluginId(const QString& id) {
    if (id.isEmpty() || id.size() > 32) return false;
    for (const QChar ch : id) {
        const bool ok = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('-') || ch == QLatin1Char('_');
        if (!ok) return false;
    }
    return true;
}

QVariant jsonToVariant(const QJsonValue& value, const QString& type) {
    if (type == QStringLiteral("bool")) return value.toBool(false);
    if (type == QStringLiteral("int")) return value.toInt(0);
    if (type == QStringLiteral("number")) return value.toDouble(0.0);
    return value.toString();
}

} // namespace

AppSettings& AppSettings::instance() {
    static AppSettings s;
    return s;
}

AppSettings::AppSettings() {
    reload();
}

void AppSettings::reload() {
    QSettings s;
    AppSettingsData next;
    next.theme = s.value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString();
    if (next.theme != QStringLiteral("dark") && next.theme != QStringLiteral("midnight")
        && next.theme != QStringLiteral("light")) {
        next.theme = QStringLiteral("dark");
    }
    next.accent = s.value(QStringLiteral("ui/accent"), QStringLiteral("blue")).toString();
    if (next.accent != QStringLiteral("blue") && next.accent != QStringLiteral("emerald")
        && next.accent != QStringLiteral("violet") && next.accent != QStringLiteral("amber")) {
        next.accent = QStringLiteral("blue");
    }
    next.font_size = std::clamp(s.value(QStringLiteral("ui/fontSize"), 14).toInt(), 12, 18);

    next.close_to_tray = s.value(QStringLiteral("general/closeToTray"), true).toBool();
    next.auto_receive_on_login = s.value(QStringLiteral("general/autoReceiveOnLogin"), true).toBool();
    next.default_server = s.value(QStringLiteral("lastServer"), QString{}).toString();

    next.default_quality = std::clamp(s.value(QStringLiteral("stream/defaultQuality"), 1).toInt(), 0, 2);
    next.default_bitrate = std::clamp(s.value(QStringLiteral("stream/defaultBitrate"), 1).toInt(), 0, 2);
    next.show_diagnostics = s.value(QStringLiteral("stream/showDiagnostics"), true).toBool();
    next.speaker_enabled = s.value(QStringLiteral("audio/speakerEnabled"), true).toBool();
    next.speaker_volume = std::clamp(s.value(QStringLiteral("audio/speakerVolume"), 100).toInt(), 25, 100);
    next.mic_fallback = s.value(QStringLiteral("audio/micFallback"), true).toBool();
    data_ = std::move(next);
}

void AppSettings::save(const AppSettingsData& next) {
    QSettings s;
    s.setValue(QStringLiteral("ui/theme"), next.theme);
    s.setValue(QStringLiteral("ui/accent"), next.accent);
    s.setValue(QStringLiteral("ui/fontSize"), next.font_size);

    s.setValue(QStringLiteral("general/closeToTray"), next.close_to_tray);
    s.setValue(QStringLiteral("general/autoReceiveOnLogin"), next.auto_receive_on_login);
    if (!next.default_server.trimmed().isEmpty()) {
        s.setValue(QStringLiteral("lastServer"), next.default_server.trimmed());
    }

    s.setValue(QStringLiteral("stream/defaultQuality"), std::clamp(next.default_quality, 0, 2));
    s.setValue(QStringLiteral("stream/defaultBitrate"), std::clamp(next.default_bitrate, 0, 2));
    s.setValue(QStringLiteral("stream/showDiagnostics"), next.show_diagnostics);
    s.setValue(QStringLiteral("audio/speakerEnabled"), next.speaker_enabled);
    s.setValue(QStringLiteral("audio/speakerVolume"), std::clamp(next.speaker_volume, 25, 100));
    s.setValue(QStringLiteral("audio/micFallback"), next.mic_fallback);
    s.sync();

    if (data_ != next) {
        data_ = next;
        emit changed(data_);
    }
}

QString AppSettings::pluginsDirectory() {
    const auto app_dir = QCoreApplication::applicationDirPath();
    const auto beside_exe = QDir(app_dir).filePath(QStringLiteral("plugins"));
    if (QFileInfo::exists(beside_exe)) return beside_exe;
    return QDir::current().filePath(QStringLiteral("plugins"));
}

QString AppSettings::pluginConfigDirectory() {
    const auto local_app = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (local_app.isEmpty()) return {};
    return QDir(local_app).filePath(QStringLiteral("P2PRemote/plugin-config"));
}

QVector<PluginSettingSpec> AppSettings::discoverPlugins() {
    QVector<PluginSettingSpec> result;
    const QDir root(pluginsDirectory());
    if (!root.exists()) return result;

    const auto entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& dir_name : entries) {
        if (!isValidPluginId(dir_name)) continue;
        QFile file(root.filePath(dir_name + QStringLiteral("/plugin.json")));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto doc = QJsonDocument::fromJson(file.read(32 * 1024));
        if (!doc.isObject()) continue;
        const auto obj = doc.object();

        PluginSettingSpec spec;
        spec.id = obj.value(QStringLiteral("id")).toString();
        if (spec.id != dir_name) continue;
        spec.name = obj.value(QStringLiteral("name")).toString(spec.id);
        spec.version = obj.value(QStringLiteral("version")).toString(QStringLiteral("1.0.0"));
        spec.description = obj.value(QStringLiteral("description")).toString();
        spec.enabled = obj.value(QStringLiteral("enabled")).toBool(true);

        for (const auto& r : obj.value(QStringLiteral("roles")).toArray()) {
            if (r.isString()) spec.roles.append(r.toString());
        }
        for (const auto& p : obj.value(QStringLiteral("permissions")).toArray()) {
            if (p.isString()) spec.permissions.append(p.toString());
        }

        if (spec.enabled && spec.roles.contains(QStringLiteral("viewer"))) {
            const auto reports = obj.value(QStringLiteral("agent_reports")).toArray();
            for (const auto& value : reports) {
                if (spec.agent_reports.size() == 8) break;
                const auto report = value.toObject();
                const auto key = report.value(QStringLiteral("id")).toString();
                if (!isValidPluginId(key) || key.size() > 24 || key.contains(QLatin1Char('_'))) continue;
                const auto name = QStringLiteral("plugin_%1_%2").arg(spec.id, key);
                if (name.size() > 64 || spec.agent_reports.contains(name)) continue;
                QStringList fields;
                const auto requested = report.value(QStringLiteral("fields")).toArray();
                if (requested.isEmpty() || requested.size() > 8) continue;
                for (const auto& field : requested) fields.append(field.toString());
                spec.agent_reports.insert(name, fields);
            }
        }

        const auto ui = obj.value(QStringLiteral("settings_ui")).toObject();
        const auto mode = ui.value(QStringLiteral("mode")).toString();
        const auto page = ui.value(QStringLiteral("page")).toString();
        const bool builtin_page = page == QStringLiteral("appearance") || page == QStringLiteral("general") ||
            page == QStringLiteral("stream-audio");
        const auto group = ui.value(QStringLiteral("nav_group")).toString(QStringLiteral("软件设置"));
        const auto title = ui.value(QStringLiteral("page_title")).toString().trimmed();
        if (mode == QStringLiteral("embed") && builtin_page) {
            spec.placement = PluginSettingSpec::Placement::embed;
        } else if (mode == QStringLiteral("page") && isValidPluginId(page) && !builtin_page &&
            !title.isEmpty() && title.size() <= 64 &&
            (group == QStringLiteral("软件设置") || group == QStringLiteral("插件与扩展"))) {
            spec.placement = PluginSettingSpec::Placement::page;
        }
        spec.page = page;
        spec.page_title = title;
        spec.section = ui.value(QStringLiteral("section")).toString(spec.name).trimmed();
        if (spec.section.isEmpty()) spec.section = spec.name;
        spec.order = std::clamp(ui.value(QStringLiteral("order")).toInt(50), -10000, 10000);
        spec.extension_page = group == QStringLiteral("插件与扩展");
        const auto role = ui.value(QStringLiteral("role")).toString(QStringLiteral("viewer"));
        spec.host_config = role == QStringLiteral("host");
#if !defined(Q_OS_WIN)
        if (!spec.roles.contains(QStringLiteral("viewer"))) continue;
        if (spec.host_config) spec.placement = PluginSettingSpec::Placement::none;
#endif

        const auto settings = spec.placement == PluginSettingSpec::Placement::none ||
            (role != QStringLiteral("viewer") && !spec.host_config) || !spec.roles.contains(role)
            ? QJsonArray{} : obj.value(QStringLiteral("settings")).toArray();
        for (const auto& item : settings) {
            if (!item.isObject()) continue;
            const auto f = item.toObject();
            PluginSettingField field;
            field.key = f.value(QStringLiteral("key")).toString();
            field.type = f.value(QStringLiteral("type")).toString(QStringLiteral("string"));
            field.label = f.value(QStringLiteral("label")).toString(field.key);
            if (!isValidPluginId(field.key)) continue;
            field.description = f.value(QStringLiteral("description")).toString();
            field.placeholder = f.value(QStringLiteral("placeholder")).toString();
            field.suffix = f.value(QStringLiteral("suffix")).toString();
            field.secret = f.value(QStringLiteral("secret")).toBool(false);
            field.default_value = jsonToVariant(f.value(QStringLiteral("default")), field.type);
            field.min_value = f.value(QStringLiteral("min")).toDouble(0.0);
            field.max_value = f.value(QStringLiteral("max")).toDouble(100.0);
            field.step_value = f.value(QStringLiteral("step")).toDouble(1.0);
            field.decimals = std::clamp(f.value(QStringLiteral("decimals")).toInt(1), 0, 4);
            for (const auto& opt : f.value(QStringLiteral("options")).toArray()) {
                if (opt.isObject()) {
                    const auto oo = opt.toObject();
                    const auto val = oo.value(QStringLiteral("value")).toString();
                    const auto lbl = oo.value(QStringLiteral("label")).toString(val);
                    field.options.append({val, lbl});
                } else if (opt.isString()) {
                    field.options.append({opt.toString(), opt.toString()});
                }
            }
            spec.fields.append(std::move(field));
        }
        result.append(std::move(spec));
    }
    return result;
}

QJsonObject AppSettings::loadPluginConfig(const QString& plugin_id, bool host) {
    if (!isValidPluginId(plugin_id)) return {};
    const auto dir = pluginConfigDirectory();
    if (dir.isEmpty()) return {};
    const auto extension = host ? QStringLiteral(".host.json") : QStringLiteral(".viewer.json");
    QFile file(QDir(dir).filePath(plugin_id + extension));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(file.read(16 * 1024));
    return doc.isObject() ? doc.object() : QJsonObject{};
}

bool AppSettings::savePluginConfig(const QString& plugin_id, const QJsonObject& updates, bool host) {
    const auto data = QJsonDocument(updates).toJson(QJsonDocument::Compact);
    const auto role = host ? P2P_PLUGIN_CONFIG_HOST : P2P_PLUGIN_CONFIG_VIEWER;
    if (p2p_plugin_update_config(plugin_id.toUtf8().constData(), role, data.constData()) != P2P_OK) return false;
    if (!host) emit instance().pluginConfigChanged(plugin_id);
    return true;
}

std::optional<bool> AppSettings::overlayVisibility(const QString& plugin_id) {
    if (!isValidPluginId(plugin_id)) return std::nullopt;
    const auto value = QSettings().value(QStringLiteral("overlay/%1/visible").arg(plugin_id));
    return value.isValid() ? std::optional<bool>(value.toBool()) : std::nullopt;
}

void AppSettings::setOverlayVisibility(const QString& plugin_id, bool visible) {
    if (!isValidPluginId(plugin_id)) return;
    QSettings().setValue(QStringLiteral("overlay/%1/visible").arg(plugin_id), visible);
    emit instance().pluginConfigChanged(plugin_id);
}
