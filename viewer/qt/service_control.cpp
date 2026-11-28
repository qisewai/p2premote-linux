#include "service_control.h"

#include "../../host/service_protocol.h"
#include "../../host/service_storage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <windows.h>
#include <shellapi.h>

#include <filesystem>

namespace {

using namespace p2premote::service;

std::wstring serviceExePath() {
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("p2premote_service.exe"));
    return QDir::toNativeSeparators(path).toStdWString();
}

bool runElevatedAndWait(const std::wstring& exe, const std::wstring& params, QString* error) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (error) {
            *error = (err == ERROR_CANCELLED)
                ? QStringLiteral("已取消管理员授权。")
                : QStringLiteral("无法启动管理员权限进程（错误码 %1）。").arg(err);
        }
        return false;
    }
    DWORD exit_code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &exit_code);
        CloseHandle(sei.hProcess);
    }
    if (exit_code != 0) {
        if (error) *error = QStringLiteral("系统服务操作失败（退出码 %1）。").arg(exit_code);
        return false;
    }
    return true;
}

} // namespace

bool isProcessElevated() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size{};
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool HostServiceAdapter::available() {
    return QFileInfo::exists(QString::fromStdWString(serviceExePath()));
}

std::optional<HostServiceInfo> HostServiceAdapter::query() {
    HostServiceInfo info;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS)) {
            info.installed = true;
            SERVICE_STATUS_PROCESS ssp{};
            DWORD bytes{};
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes)) {
                info.running = (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING);
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    return info;
}

HostServiceInfo HostServiceAdapter::queryWorker(HostServiceInfo info) {
    bool trusted{};
    if (const auto reply = ReadServiceStatus(250, &trusted)) {
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(*reply));
        if (doc.isObject()) {
            const auto obj = doc.object();
            info.pipe_connected = true;
            static const QString gui_hash = [] {
                QFile image(QCoreApplication::applicationFilePath());
                QCryptographicHash hash(QCryptographicHash::Sha256);
                return image.open(QIODevice::ReadOnly) && hash.addData(&image)
                    ? QString::fromLatin1(hash.result().toHex()) : QString{};
            }();
            const auto registered = QDir::fromNativeSeparators(obj.value(QStringLiteral("client_path")).toString());
            info.needs_upgrade = !trusted || gui_hash.isEmpty() ||
                gui_hash != obj.value(QStringLiteral("client_hash")).toString() ||
                registered.compare(QCoreApplication::applicationFilePath(), Qt::CaseInsensitive) != 0;
            if (trusted) info.foreground_lease = obj.value(QStringLiteral("foreground_lease")).toString();
            info.user = obj.value(QStringLiteral("user")).toString();
            info.device_id = obj.value(QStringLiteral("device_id")).toString();
            info.cloud_enabled = obj.contains(QStringLiteral("cloud_enabled"))
                ? obj.value(QStringLiteral("cloud_enabled")).toBool()
                : !info.device_id.isEmpty();
            info.state = obj.value(QStringLiteral("state")).toInt();
            info.desktop_control = obj.value(QStringLiteral("desktop_control")).toBool();
            info.lan_enabled = obj.value(QStringLiteral("lan_enabled")).toBool();
            const int port = obj.value(QStringLiteral("lan_port")).toInt(P2P_LAN_DEFAULT_PORT);
            info.lan_port = (port >= 1 && port <= 65535)
                ? static_cast<std::uint16_t>(port) : static_cast<std::uint16_t>(P2P_LAN_DEFAULT_PORT);
            info.lan_state = obj.value(QStringLiteral("lan_state")).toInt();
            info.lan_desktop_control = obj.value(QStringLiteral("lan_desktop_control")).toBool();
            info.unattended = obj.value(QStringLiteral("unattended")).toBool();
            info.monitor_index = obj.value(QStringLiteral("monitor_index")).toInt();
            info.status = obj.value(QStringLiteral("status")).toString();
            info.lan_status = obj.value(QStringLiteral("lan_status")).toString();
        }
    }
    return info;
}

static bool installHostService(const HostServiceConfig& config, HostConfigUpdate update, QString* error) {
    const auto exe = serviceExePath();
    if (!QFileInfo::exists(QString::fromStdWString(exe))) {
        if (error) *error = QStringLiteral("未找到 p2premote_service.exe，请重新解压完整客户端。");
        return false;
    }

    QJsonObject obj;
    switch (update) {
    case HostConfigUpdate::lan: obj.insert(QStringLiteral("update"), QStringLiteral("lan")); break;
    case HostConfigUpdate::cloud: obj.insert(QStringLiteral("update"), QStringLiteral("cloud")); break;
    case HostConfigUpdate::monitor: obj.insert(QStringLiteral("update"), QStringLiteral("monitor")); break;
    case HostConfigUpdate::install: break;
    case HostConfigUpdate::unattended:
    case HostConfigUpdate::upgrade:
        return false;
    }
    if (!config.server.isEmpty() && !config.user.isEmpty() && !config.password.empty()
            && !config.installation_id.isEmpty() && !config.device_id.isEmpty()) {
        obj.insert(QStringLiteral("server"), config.server);
        obj.insert(QStringLiteral("user"), config.user);
        obj.insert(QStringLiteral("password"), QString::fromStdWString(config.password));
        obj.insert(QStringLiteral("installation_id"), config.installation_id);
        obj.insert(QStringLiteral("device_id"), config.device_id);
        obj.insert(QStringLiteral("compatible_http"), config.compatible_http);
        obj.insert(QStringLiteral("relay_only"), config.relay_only);
    }
    obj.insert(QStringLiteral("lan_enabled"), config.lan_enabled);
    obj.insert(QStringLiteral("lan_port"), static_cast<int>(config.lan_port ? config.lan_port : P2P_LAN_DEFAULT_PORT));
    if (config.lan_enabled && !config.lan_access_code.empty()) {
        obj.insert(QStringLiteral("lan_access_code"), QString::fromStdWString(config.lan_access_code));
    }
    obj.insert(QStringLiteral("unattended"), config.unattended);
    obj.insert(QStringLiteral("monitor_index"), config.monitor_index);
    QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    const auto suffix = UniqueSuffix();
    const auto staging = std::filesystem::path(QDir::tempPath().toStdWString()) / (L"p2premote-" + suffix + L".tmp");

    std::string plain(json.constData(), static_cast<std::size_t>(json.size()));
    SecureZeroMemory(json.data(), static_cast<std::size_t>(json.size()));
    const bool written = !suffix.empty() && WriteEncryptedConfig(staging, plain, true);
    SecureZeroMemory(plain.data(), plain.size());
    if (!written) {
        if (error) *error = QStringLiteral("无法加密写入服务配置。");
        return false;
    }

    const std::wstring params = L"--install --config-file \"" + staging.wstring() + L"\"";
    const bool ok = runElevatedAndWait(exe, params, error);
    DeleteFileW(staging.c_str());
    return ok;
}

static bool uninstallHostService(QString* error) {
    const auto exe = serviceExePath();
    if (!QFileInfo::exists(QString::fromStdWString(exe))) {
        if (error) *error = QStringLiteral("未找到 p2premote_service.exe。");
        return false;
    }
    return runElevatedAndWait(exe, L"--uninstall", error);
}

bool HostServiceAdapter::configureSync(HostConfigUpdate update, bool enabled, const HostServiceConfig& config,
    QString* error) {
    switch (update) {
    case HostConfigUpdate::install:
        return enabled ? installHostService(config, update, error) : uninstallHostService(error);
    case HostConfigUpdate::cloud:
        return enabled ? installHostService(config, update, error)
                       : runElevatedAndWait(serviceExePath(), L"--disable-cloud", error);
    case HostConfigUpdate::unattended:
        return runElevatedAndWait(serviceExePath(), enabled ? L"--enable-unattended" : L"--disable-unattended", error);
    case HostConfigUpdate::upgrade:
        return runElevatedAndWait(serviceExePath(), L"--upgrade", error);
    case HostConfigUpdate::lan:
    case HostConfigUpdate::monitor:
        return installHostService(config, update, error);
    }
    return false;
}

void HostServiceAdapter::configure(QObject*, HostConfigUpdate update, bool enabled, const HostServiceConfig& config,
    std::function<void(QString)> completed) {
    QString error;
    if (!configureSync(update, enabled, config, &error) && error.isEmpty()) {
        error = QStringLiteral("无法更新被控服务");
    }
    if (completed) completed(std::move(error));
}

bool relaunchClientElevated(QString* error) {
    const auto exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = L"--elevated-restart";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (error) {
            *error = (err == ERROR_CANCELLED)
                ? QStringLiteral("已取消管理员授权。")
                : QStringLiteral("无法以管理员身份重启（错误码 %1）。").arg(err);
        }
        return false;
    }
    return true;
}
