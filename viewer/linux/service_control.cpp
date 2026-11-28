#include "../qt/service_control.h"
#include "appimage.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>

#include <unistd.h>

namespace {

const auto service_unit = QStringLiteral("p2premote-host.service");
const auto service_executable = QString::fromUtf8(P2PREMOTE_LINUX_HOST_EXECUTABLE);
const auto systemctl = QStringLiteral("/usr/bin/systemctl");
const auto pkexec = QStringLiteral("/usr/bin/pkexec");
const auto appimage_installer = QStringLiteral(R"(
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
work=$(mktemp -d /tmp/p2premote-appimage.XXXXXXXX)
trap 'rm -rf -- "$work"' EXIT
TMPDIR="$work" "$1" --appimage-extract-and-run --install-and-enable-host
)");

QString describeDesktopStatus(const QString& state) {
    if (state == QStringLiteral("display")) return QStringLiteral("已连接，正在等待活动的桌面");
    if (state == QStringLiteral("permission")) return QStringLiteral("正在等待系统授予屏幕共享权限");
    if (state == QStringLiteral("revoked")) return QStringLiteral("屏幕共享已停止，请重新开启或授权");
    return {};
}

struct UnitInfo {
    bool installed{}, enabled{};
    QString state, status;
};

std::optional<UnitInfo> queryUnit(const QString& unit) {
    UnitInfo info;
    QProcess process;
    process.start(systemctl, {QStringLiteral("show"), QStringLiteral("--no-pager"),
        QStringLiteral("--property=LoadState,ActiveState,UnitFileState,StatusText"), unit});
    if (!process.waitForFinished(1500)) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(500);
        }
        return std::nullopt;
    }

    QString load_state;
    for (const auto& line : QString::fromUtf8(process.readAllStandardOutput()).split(u'\n')) {
        const auto sep = line.indexOf(u'=');
        if (sep < 0) continue;
        const auto name = line.left(sep);
        const auto value = line.mid(sep + 1);
        if (name == QStringLiteral("LoadState")) load_state = value;
        else if (name == QStringLiteral("ActiveState")) info.state = value;
        else if (name == QStringLiteral("StatusText")) info.status = value;
        else if (name == QStringLiteral("UnitFileState")) {
            info.enabled = value == QStringLiteral("enabled") || value == QStringLiteral("enabled-runtime");
        }
    }
    if (load_state.isEmpty()) return std::nullopt;
    info.installed = load_state == QStringLiteral("loaded");
    return info;
}

} // namespace

bool HostServiceAdapter::available() {
    return QFileInfo(systemctl).isExecutable() &&
        (QFileInfo(service_executable).isExecutable() || !appImageExecutable().isEmpty()) &&
        (geteuid() == 0 || QFileInfo(pkexec).isExecutable());
}

std::optional<HostServiceInfo> HostServiceAdapter::query() {
    const auto unit = queryUnit(service_unit);
    if (!unit) return std::nullopt;

    HostServiceInfo info;
    const bool marker = QFileInfo::exists(QStringLiteral("/etc/p2premote/host.enabled"));
    info.needs_cleanup = QFileInfo::exists(QStringLiteral("/etc/p2premote/host.inactive"));
    const bool active = unit->state == QStringLiteral("active");
    info.installed = marker || info.needs_cleanup || unit->enabled || active;
    info.unattended = QFileInfo::exists(QStringLiteral("/etc/p2premote/host.unattended"));
    info.enabled = unit->enabled;
    info.cloud_installed = unit->installed;
    info.running = active;

    QString lan_state;
    QString cloud_state;
    const auto fields = unit->status.split(u' ');
    if (fields.size() >= 3 && fields[0] == QStringLiteral("p2premote") &&
        fields[1].startsWith(QStringLiteral("lan=")) && fields[2].startsWith(QStringLiteral("cloud="))) {
        const auto lan_parts = fields[1].mid(4).split(u':');
        if (lan_parts.size() == 2) {
            lan_state = lan_parts[0];
            const auto port = lan_parts[1].toUInt();
            if (port >= 1 && port <= 65535) info.lan_port = static_cast<std::uint16_t>(port);
        }
        const auto cloud_value = fields[2].mid(6);
        const auto sep = cloud_value.indexOf(u':');
        if (sep >= 0) {
            cloud_state = cloud_value.left(sep);
            const auto device = cloud_value.mid(sep + 1);
            if (device != QStringLiteral("-")) info.device_id = device;
        }
    }

    info.lan_enabled = active && !lan_state.isEmpty() && lan_state != QStringLiteral("off");
    info.cloud_enabled = active && !cloud_state.isEmpty() && cloud_state != QStringLiteral("off");

    if (unit->state == QStringLiteral("activating")) {
        info.lan_status = QStringLiteral("被控服务正在启动");
    } else if (unit->state == QStringLiteral("failed")) {
        info.lan_status = QStringLiteral("被控服务启动失败，请查看 systemd 日志");
    } else if (info.lan_enabled) {
        info.lan_state = P2P_WAITING;
        if (lan_state == QStringLiteral("viewing")) {
            info.lan_state = P2P_STREAMING;
            info.lan_status = QStringLiteral("远程主控正在查看本机画面");
        } else if (lan_state == QStringLiteral("streaming")) {
            info.lan_state = P2P_STREAMING;
            info.lan_desktop_control = true;
            info.lan_status = QStringLiteral("远程主控已接入");
        } else if (const auto desktop_status = describeDesktopStatus(lan_state); !desktop_status.isEmpty()) {
            info.lan_state = P2P_NEGOTIATING;
            info.lan_status = desktop_status;
        } else if (lan_state == QStringLiteral("negotiating")) {
            info.lan_state = P2P_NEGOTIATING;
            info.lan_status = QStringLiteral("正在建立远程会话");
        } else if (lan_state == QStringLiteral("standby")) {
            info.lan_state = P2P_IDLE;
            info.lan_status = QStringLiteral("已休眠，等待前台软件启动");
        }
        if (info.lan_status.isEmpty()) info.lan_status = info.unattended
            ? QStringLiteral("无人值守服务正在等待连接，退出客户端后仍继续运行")
            : QStringLiteral("同网段设备已可发现此电脑，退出客户端后自动休眠断网");
    }

    if (info.cloud_enabled) {
        if (cloud_state == QStringLiteral("waiting")) {
            info.status = QStringLiteral("已上线，等待同账号设备连接");
            info.state = P2P_WAITING;
            info.cloud_online = true;
        } else if (cloud_state == QStringLiteral("viewing")) {
            info.status = QStringLiteral("同账号设备正在查看本机画面");
            info.state = P2P_STREAMING;
            info.cloud_online = true;
        } else if (cloud_state == QStringLiteral("streaming")) {
            info.status = QStringLiteral("同账号设备正在远程控制本机");
            info.state = P2P_STREAMING;
            info.desktop_control = info.cloud_online = true;
        } else if (const auto desktop_status = describeDesktopStatus(cloud_state); !desktop_status.isEmpty()) {
            info.status = desktop_status;
            info.state = P2P_NEGOTIATING;
            info.cloud_online = true;
        } else if (cloud_state == QStringLiteral("negotiating")) {
            info.status = QStringLiteral("正在建立云端远程会话");
            info.state = P2P_NEGOTIATING;
            info.cloud_online = true;
        } else if (cloud_state == QStringLiteral("removed")) {
            info.status = QStringLiteral("设备已从账号移除，请重新添加本机");
        } else if (cloud_state == QStringLiteral("credentials")) {
            info.status = QStringLiteral("后台登录失效，请关闭并重新启用云端接收");
        } else if (cloud_state == QStringLiteral("standby")) {
            info.status = QStringLiteral("已休眠，等待前台软件启动");
        } else {
            info.status = QStringLiteral("正在连接云端服务器…");
        }
    } else if (unit->state == QStringLiteral("failed")) {
        info.status = QStringLiteral("被控服务启动失败");
    }
    return info;
}

namespace {

void manageService(QObject* owner, const QString& command, const QByteArray& payload,
    std::function<void(QString)> completed) {
    if (!HostServiceAdapter::available()) {
        completed(QStringLiteral("未找到安装好的被控服务或系统授权工具。"));
        return;
    }

    const auto image = command == QStringLiteral("--install-host-service") ? appImageExecutable() : QString{};
    const bool install_from_image = !image.isEmpty();
    const auto executable = install_from_image ? QStringLiteral("/usr/bin/bash") : service_executable;
    if (!QFileInfo(executable).isExecutable()) {
        completed(QStringLiteral("请先安装被控服务。"));
        return;
    }
    const auto args = install_from_image
        ? QStringList{QStringLiteral("-c"), appimage_installer, QStringLiteral("p2premote-appimage-install"), image}
        : QStringList{command};

    auto* child = new QProcess(owner);
    QObject::connect(child, &QProcess::started, child, [child, payload] {
        if (!payload.isEmpty()) child->write(payload);
        child->closeWriteChannel();
    });
    QObject::connect(child, &QProcess::errorOccurred, owner, [child, completed](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        child->deleteLater();
        completed(QStringLiteral("无法启动系统服务管理程序。"));
    });
    QObject::connect(child, &QProcess::finished, owner, [child, completed, install_from_image](int result, QProcess::ExitStatus status) {
        const auto detail = QString::fromUtf8(child->readAllStandardError()).trimmed();
        child->deleteLater();
        if (status == QProcess::NormalExit && result == 0) {
            if (install_from_image) {
                const auto launcher = QStringLiteral("/usr/local/bin/p2premote");
                if (!QProcess::startDetached(launcher, {QStringLiteral("--after-update")})) {
                    completed(QStringLiteral("被控服务已安装，请重新打开 P2P Remote。"));
                    return;
                }
            }
            completed({});
            if (install_from_image) QCoreApplication::exit(0);
        } else if (result == 126) {
            completed(QStringLiteral("已取消管理员授权。"));
        } else if (result == 127) {
            completed(QStringLiteral("无法取得管理员授权。"));
        } else {
            completed(detail.isEmpty() ? QStringLiteral("被控服务操作失败，请查看 systemd 日志。") : detail);
        }
    });

    if (geteuid() == 0) child->start(executable, args);
    else child->start(pkexec, QStringList{QStringLiteral("--disable-internal-agent"), executable} + args);
}

} // namespace

namespace {

QByteArray cloudCredentials(const HostServiceConfig& config) {
    const QJsonObject data{{"server", config.server}, {"user", config.user},
        {"password", QString::fromStdWString(config.password)}, {"installationId", config.installation_id},
        {"deviceId", config.device_id}, {"relayOnly", config.relay_only}};
    return QJsonDocument(data).toJson(QJsonDocument::Compact);
}

} // namespace

void HostServiceAdapter::configure(QObject* owner, HostConfigUpdate update, bool enabled,
    const HostServiceConfig& config, std::function<void(QString)> completed) {
    QString command;
    QByteArray payload;
    switch (update) {
    case HostConfigUpdate::install:
        command = enabled ? QStringLiteral("--install-host-service") : QStringLiteral("--uninstall-host-service");
        if (enabled && !config.server.isEmpty() && !config.user.isEmpty() && !config.password.empty() &&
            !config.installation_id.isEmpty() && !config.device_id.isEmpty()) {
            payload = cloudCredentials(config);
        }
        break;
    case HostConfigUpdate::lan:
        command = enabled ? QStringLiteral("--enable-service") : QStringLiteral("--disable-service");
        if (enabled) {
            payload = QByteArray::number(config.lan_port) + '\n' +
                QString::fromStdWString(config.lan_access_code).toUtf8() + '\n';
        }
        break;
    case HostConfigUpdate::cloud:
        command = enabled ? QStringLiteral("--enable-cloud-service") : QStringLiteral("--disable-cloud-service");
        if (enabled) payload = cloudCredentials(config);
        break;
    case HostConfigUpdate::unattended:
        command = enabled ? QStringLiteral("--enable-unattended") : QStringLiteral("--disable-unattended");
        break;
    case HostConfigUpdate::monitor:
    case HostConfigUpdate::upgrade:
        if (completed) completed(QStringLiteral("当前系统服务不支持此操作"));
        return;
    }
    manageService(owner, command, payload, std::move(completed));
}
