#include "service_control.h"

#include "../../host/service_protocol.h"
#include "../../host/service_storage.h"

#include <QCoreApplication>
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
        WaitForSingleObject(sei.hProcess, 30000);
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

bool serviceExecutableExists() {
    return QFileInfo::exists(QString::fromStdWString(serviceExePath()));
}

HostServiceInfo queryHostServiceInfo() {
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

HostServiceInfo queryHostWorkerStatus(HostServiceInfo info) {
    if (const auto reply = ReadServiceStatus(250)) {
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(*reply));
        if (doc.isObject()) {
            const auto obj = doc.object();
            info.pipe_connected = true;
            info.state = obj.value(QStringLiteral("state")).toInt();
            info.desktop_control = obj.value(QStringLiteral("desktop_control")).toBool();
            info.monitor_index = obj.value(QStringLiteral("monitor_index")).toInt();
            info.user = obj.value(QStringLiteral("user")).toString();
            info.device_id = obj.value(QStringLiteral("device_id")).toString();
            info.status = obj.value(QStringLiteral("status")).toString();
        }
    }
    return info;
}

bool installHostService(const QString& server, const QString& user, const std::wstring& password,
    const QString& installation_id, const QString& device_id, int monitor_index,
    bool compatible_http, bool relay_only, QString* error) {
    const auto exe = serviceExePath();
    if (!QFileInfo::exists(QString::fromStdWString(exe))) {
        if (error) *error = QStringLiteral("未找到 p2premote_service.exe，请重新解压完整客户端。");
        return false;
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("server"), server);
    obj.insert(QStringLiteral("user"), user);
    obj.insert(QStringLiteral("password"), QString::fromStdWString(password));
    obj.insert(QStringLiteral("installation_id"), installation_id);
    obj.insert(QStringLiteral("device_id"), device_id);
    obj.insert(QStringLiteral("monitor_index"), monitor_index);
    obj.insert(QStringLiteral("compatible_http"), compatible_http);
    obj.insert(QStringLiteral("relay_only"), relay_only);
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

bool uninstallHostService(QString* error) {
    const auto exe = serviceExePath();
    if (!QFileInfo::exists(QString::fromStdWString(exe))) {
        if (error) *error = QStringLiteral("未找到 p2premote_service.exe。");
        return false;
    }
    return runElevatedAndWait(exe, L"--uninstall", error);
}

bool relaunchClientElevated(QString* error) {
    const auto exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
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
