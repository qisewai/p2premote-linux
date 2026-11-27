#include "update_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

namespace {

std::filesystem::path appDirectory() {
    return std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString());
}

std::filesystem::path appExecutablePath() {
    return std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString());
}

bool launchDetachedProcess(const std::filesystem::path& exe_path) {
    std::wstring cmdline = L"\"" + exe_path.wstring() + L"\" --after-update";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, exe_path.parent_path().c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool runElevatedServiceUpgrade(const std::filesystem::path& service_exe, QString* error) {
    if (!std::filesystem::exists(service_exe)) return true;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = service_exe.c_str();
    sei.lpParameters = L"--upgrade";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (error) {
            *error = (err == ERROR_CANCELLED)
                ? QStringLiteral("已取消管理员授权，后台服务将在下次手动同步时更新。")
                : QStringLiteral("无法启动管理员权限同步后台服务（错误码 %1）。").arg(err);
        }
        return false;
    }
    DWORD exit_code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        GetExitCodeProcess(sei.hProcess, &exit_code);
        CloseHandle(sei.hProcess);
    }
    if (exit_code != 0) {
        if (error) *error = QStringLiteral("后台服务同步更新失败（退出码 %1）。").arg(exit_code);
        return false;
    }
    return true;
}

} // namespace

void cleanPlatformUpdateLeftovers() noexcept {
    p2premote::update::CleanUpdateLeftovers(appDirectory());
}

bool isPlatformSelfUpdateSupported() {
    const auto dir = appDirectory();
    return p2premote::update::IsDirectoryWritable(dir) &&
           p2premote::update::IsDirectoryWritable(dir.parent_path());
}

UpdateApplyResult applyPlatformUpdate(const std::filesystem::path& archive_path,
                                      const std::filesystem::path& /*manifest_path*/,
                                      const p2premote::update::UpdateManifest& manifest,
                                      bool has_installed_service) {
    const auto target_dir = appDirectory();
    const auto exe_path = appExecutablePath();

    if (!p2premote::update::IsDirectoryWritable(target_dir) ||
        !p2premote::update::IsDirectoryWritable(target_dir.parent_path())) {
        return {false, false, QStringLiteral("当前安装目录不可写，无法执行原地自更新。")};
    }

    // Re-verify signature right before extracting.
    std::string err;
    if (!p2premote::update::VerifyArchiveFile(archive_path, manifest.size, manifest.signature,
                                              UpdateClient::embeddedPublicKey().toStdString(), &err)) {
        return {false, false, QStringLiteral("更新包复验失败：%1").arg(QString::fromUtf8(err))};
    }

    const auto work_dir = target_dir.parent_path() /
        (L".p2premote-update-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    ec.clear();

    const auto cleanup_work = [&] {
        std::error_code cec;
        std::filesystem::remove_all(work_dir, cec);
    };

    const auto unpacked = work_dir / L"app";
    if (!p2premote::update::ExtractTarGzArchive(archive_path, unpacked, &err)) {
        cleanup_work();
        return {false, false, QStringLiteral("解压更新包失败：%1").arg(QString::fromUtf8(err))};
    }

    const std::string running_name = exe_path.filename().string();
    if (!p2premote::update::SwapDirectoryFiles(unpacked, target_dir, running_name, &err)) {
        cleanup_work();
        return {false, false, QStringLiteral("替换程序文件失败（已自动回滚）：%1").arg(QString::fromUtf8(err))};
    }
    cleanup_work();

    // If the Windows host service is installed, upgrade its isolated %ProgramData% runtime in lockstep.
    if (has_installed_service) {
        QString svc_err;
        runElevatedServiceUpgrade(target_dir / L"p2premote_service.exe", &svc_err);
        // Even if the user cancels UAC for service sync, the GUI itself is already cleanly updated
        // and AccountWindow will show its built-in "同步后台服务" button on relaunch.
    }

    if (!launchDetachedProcess(exe_path)) {
        return {false, false, QStringLiteral("文件已更新，但自动重启失败，请手动重新打开 P2P Remote。")};
    }
    return {true, true, {}};
}
