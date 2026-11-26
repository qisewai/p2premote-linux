#include "../qt/service_control.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>

#include <unistd.h>

namespace {

const auto service_unit = QStringLiteral("p2premote-host.service");
const auto cloud_unit = QStringLiteral("p2premote-cloud-host.service");
const auto service_executable = QString::fromUtf8(P2PREMOTE_LINUX_HOST_EXECUTABLE);
const auto systemctl = QStringLiteral("/usr/bin/systemctl");
const auto pkexec = QStringLiteral("/usr/bin/pkexec");

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

bool serviceExecutableExists() {
    return QFileInfo(service_executable).isExecutable() &&
        (geteuid() == 0 || QFileInfo(pkexec).isExecutable());
}

std::optional<HostServiceInfo> queryLinuxHostServiceInfo() {
    const auto lan = queryUnit(service_unit);
    const auto cloud = queryUnit(cloud_unit);
    if (!lan || !cloud) return std::nullopt;

    HostServiceInfo info;
    info.installed = lan->installed;
    info.enabled = lan->enabled;
    info.lan_enabled = lan->state == QStringLiteral("active");
    info.cloud_installed = cloud->installed;
    const bool cloud_running = cloud->state == QStringLiteral("active");
    info.cloud_enabled = cloud->enabled || cloud_running;
    info.running = info.lan_enabled || cloud_running;

    if (lan->state == QStringLiteral("activating")) {
        info.lan_status = QStringLiteral("被控服务正在启动");
    } else if (lan->state == QStringLiteral("failed")) {
        info.lan_status = QStringLiteral("被控服务启动失败，请查看 systemd 日志");
    } else if (info.lan_enabled) {
        info.lan_state = P2P_WAITING;
        const auto fields = lan->status.split(u' ');
        if (fields.size() == 3 && fields[0] == QStringLiteral("p2premote")) {
            const auto port = fields[2].toUInt();
            if (port >= 1 && port <= 65535) info.lan_port = static_cast<std::uint16_t>(port);
            if (fields[1] == QStringLiteral("streaming")) {
                info.lan_state = P2P_STREAMING;
                info.lan_desktop_control = true;
                info.lan_status = QStringLiteral("远程主控已接入");
            } else if (fields[1] == QStringLiteral("display")) {
                info.lan_state = P2P_NEGOTIATING;
                info.lan_status = QStringLiteral("已连接，正在等待活动的 Xorg 桌面");
            } else if (fields[1] == QStringLiteral("negotiating")) {
                info.lan_state = P2P_NEGOTIATING;
                info.lan_status = QStringLiteral("正在建立远程会话");
            }
        }
        if (info.lan_status.isEmpty()) info.lan_status = QStringLiteral("系统服务正在等待连接，关闭客户端后仍继续运行");
    }
    const auto fields = cloud->status.split(u' ');
    if (fields.size() == 3 && fields[0] == QStringLiteral("p2premote-cloud")) {
        info.device_id = fields[2];
        const auto state = fields[1];
        if (cloud_running && state == QStringLiteral("waiting")) {
            info.status = QStringLiteral("已上线，等待同账号设备连接");
            info.state = P2P_WAITING;
            info.cloud_online = true;
        } else if (cloud_running && state == QStringLiteral("streaming")) {
            info.status = QStringLiteral("同账号设备正在远程控制本机");
            info.state = P2P_STREAMING;
            info.desktop_control = info.cloud_online = true;
        } else if (cloud_running && (state == QStringLiteral("negotiating") || state == QStringLiteral("display"))) {
            info.status = state == QStringLiteral("display") ? QStringLiteral("已连接，正在等待活动的 Xorg 桌面")
                                                              : QStringLiteral("正在建立云端远程会话");
            info.state = P2P_NEGOTIATING;
            info.cloud_online = true;
        } else if (state == QStringLiteral("removed")) {
            info.status = QStringLiteral("设备已从账号移除，请重新添加本机");
        } else if (state == QStringLiteral("credentials")) {
            info.status = QStringLiteral("后台登录失效，请关闭并重新启用云端接收");
        }
    }
    if (info.cloud_enabled && info.status.isEmpty()) {
        info.status = cloud->state == QStringLiteral("failed") ? QStringLiteral("云端被控服务启动失败")
                                                                 : QStringLiteral("正在连接云端服务器…");
    }
    return info;
}

namespace {

void manageService(QObject* owner, const QString& command, const QByteArray& payload,
    std::function<void(QString)> completed) {
    if (!serviceExecutableExists()) {
        completed(QStringLiteral("未找到安装好的被控服务或系统授权工具。"));
        return;
    }

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
    QObject::connect(child, &QProcess::finished, owner, [child, completed](int result, QProcess::ExitStatus status) {
        const auto detail = QString::fromUtf8(child->readAllStandardError()).trimmed();
        child->deleteLater();
        if (status == QProcess::NormalExit && result == 0) {
            completed({});
        } else if (result == 126) {
            completed(QStringLiteral("已取消管理员授权。"));
        } else if (result == 127) {
            completed(QStringLiteral("无法取得管理员授权。"));
        } else {
            completed(detail.isEmpty() ? QStringLiteral("被控服务操作失败，请查看 systemd 日志。") : detail);
        }
    });

    if (geteuid() == 0) child->start(service_executable, {command});
    else child->start(pkexec, {QStringLiteral("--disable-internal-agent"), service_executable, command});
}

} // namespace

void manageLinuxHostService(QObject* owner, bool enable, std::uint16_t port, const QString& code,
    std::function<void(QString)> completed) {
    const auto command = enable ? QStringLiteral("--enable-service") : QStringLiteral("--disable-service");
    manageService(owner, command, enable ? QByteArray::number(port) + '\n' + code.toUtf8() + '\n' : QByteArray{},
        std::move(completed));
}

void manageLinuxCloudService(QObject* owner, const std::optional<HostServiceConfig>& config,
    std::function<void(QString)> completed) {
    const auto command = config ? QStringLiteral("--enable-cloud-service") : QStringLiteral("--disable-cloud-service");
    QByteArray payload;
    if (config) {
        const QJsonObject data{{"server", config->server}, {"user", config->user},
            {"password", QString::fromStdWString(config->password)}, {"installationId", config->installation_id},
            {"deviceId", config->device_id}, {"relayOnly", config->relay_only}};
        payload = QJsonDocument(data).toJson(QJsonDocument::Compact);
    }
    manageService(owner, command, payload, std::move(completed));
}
