#include "../qt/update_client.h"
#include "appimage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <unistd.h>

#include <filesystem>
#include <string>

namespace {

std::filesystem::path executablePath() {
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p;
    return std::filesystem::path(QCoreApplication::applicationFilePath().toUtf8().constData());
}

bool isSystemInstalledHostAvailable() {
    const QString host = QString::fromUtf8(P2PREMOTE_LINUX_HOST_EXECUTABLE);
    return QFileInfo(host).isExecutable() &&
           (geteuid() == 0 || QFileInfo(QStringLiteral("/usr/bin/pkexec")).isExecutable());
}

} // namespace

void cleanPlatformUpdateLeftovers() noexcept {
    if (!appImageExecutable().isEmpty()) return;
    p2premote::update::CleanUpdateLeftovers(executablePath().parent_path());
}

bool isPlatformSelfUpdateSupported() {
    if (!appImageExecutable().isEmpty()) return false;
    const auto dir = executablePath().parent_path();
    if (p2premote::update::IsDirectoryWritable(dir) &&
        p2premote::update::IsDirectoryWritable(dir.parent_path())) {
        return true;
    }
    return isSystemInstalledHostAvailable();
}

UpdateApplyResult applyPlatformUpdate(const std::filesystem::path& archive_path,
                                      const std::filesystem::path& manifest_path,
                                      const p2premote::update::UpdateManifest& manifest,
                                      bool /*has_installed_service*/) {
    if (!appImageExecutable().isEmpty()) {
        return {false, false, QStringLiteral("请下载并打开新版 AppImage。")};
    }
    const auto exe = executablePath();
    const auto target_dir = exe.parent_path();

    // Case 1: User-writable installation directory (portable / ~/.local).
    if (p2premote::update::IsDirectoryWritable(target_dir) &&
        p2premote::update::IsDirectoryWritable(target_dir.parent_path())) {
        std::string err;
        if (!p2premote::update::VerifyArchiveFile(archive_path, manifest.size, manifest.signature,
                                                  UpdateClient::embeddedPublicKey().toStdString(), &err)) {
            return {false, false, QStringLiteral("更新包复验失败：%1").arg(QString::fromUtf8(err))};
        }
        const auto work_dir = target_dir.parent_path() /
            (".p2premote-update-" + std::to_string(getpid()));
        std::error_code ec;
        std::filesystem::remove_all(work_dir, ec);
        const auto unpacked = work_dir / "app";
        if (!p2premote::update::ExtractTarGzArchive(archive_path, unpacked, &err)) {
            std::filesystem::remove_all(work_dir, ec);
            return {false, false, QStringLiteral("解压更新包失败：%1").arg(QString::fromUtf8(err))};
        }
        const auto packaged_gui = unpacked / "usr/local/libexec/p2premote/gui";
        const auto staged_gui = std::filesystem::is_regular_file(packaged_gui / exe.filename())
            ? packaged_gui : unpacked;
        if (!std::filesystem::is_regular_file(staged_gui / exe.filename()) ||
            !p2premote::update::SwapDirectoryFiles(staged_gui, target_dir, exe.filename().string(), &err)) {
            std::filesystem::remove_all(work_dir, ec);
            return {false, false, QStringLiteral("替换程序文件失败（已回滚）：%1").arg(QString::fromUtf8(err))};
        }
        std::filesystem::remove_all(work_dir, ec);
        if (QProcess::startDetached(QString::fromUtf8(exe.c_str()), {QStringLiteral("--after-update")})) {
            return {true, true, {}};
        }
        return {false, false, QStringLiteral("文件已更新，请手动重新打开 P2P Remote。")};
    }

    // Case 2: System-wide installation (/usr/local/libexec/p2premote) -> delegate to root host helper.
    if (!isSystemInstalledHostAvailable()) {
        return {false, false, QStringLiteral("当前目录不可写且缺少管理员提权工具，无法自更新。")};
    }

    const QString host = QString::fromUtf8(P2PREMOTE_LINUX_HOST_EXECUTABLE);
    const QStringList args{
        QStringLiteral("--apply-update"),
        QString::fromUtf8(archive_path.c_str()),
        QString::fromUtf8(manifest_path.c_str())
    };

    QProcess proc;
    if (geteuid() == 0) {
        proc.start(host, args);
    } else {
        QStringList pk_args{QStringLiteral("--disable-internal-agent"), host};
        pk_args.append(args);
        proc.start(QStringLiteral("/usr/bin/pkexec"), pk_args);
    }
    if (!proc.waitForFinished(180000)) {
        proc.kill();
        return {false, false, QStringLiteral("系统级更新超时。")};
    }
    const int rc = proc.exitCode();
    if (proc.exitStatus() != QProcess::NormalExit || rc != 0) {
        if (rc == 126) return {false, false, QStringLiteral("已取消管理员授权。")};
        const QString detail = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return {false, false, detail.isEmpty()
            ? QStringLiteral("系统级更新失败（退出码 %1）。").arg(rc)
            : detail};
    }

    // Restart user-session plugin agent and relaunch updated GUI launcher.
    const QString clipboard_agent = QFileInfo(host).dir().filePath(QStringLiteral("p2premote_clipboard"));
    if (QFileInfo(clipboard_agent).isExecutable()) {
        QProcess::startDetached(clipboard_agent, {});
    }
    const QString launcher = QFileInfo::exists(QStringLiteral("/usr/local/bin/p2premote"))
        ? QStringLiteral("/usr/local/bin/p2premote")
        : QString::fromUtf8(exe.c_str());
    if (QProcess::startDetached(launcher, {QStringLiteral("--after-update")})) return {true, true, {}};
    return {false, false, QStringLiteral("文件已更新，请手动重新打开 P2P Remote。")};
}
