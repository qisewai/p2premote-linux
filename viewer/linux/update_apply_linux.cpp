#include "../qt/update_client.h"
#include "appimage.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

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

std::error_code stagePortableLibraries(const std::filesystem::path& runtime) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto entry = fs::directory_iterator(runtime / "lib", ec);
    if (ec == std::errc::no_such_file_or_directory) return {};
    if (ec) return ec;

    // Older system packages keep some client dependencies beside the GUI directory.
    const auto gui_libs = runtime / "gui/lib";
    fs::create_directories(gui_libs, ec);
    if (ec) return ec;
    std::vector<fs::path> missing;
    while (entry != fs::directory_iterator()) {
        const auto destination = gui_libs / entry->path().filename();
        const auto status = fs::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) return ec;
        if (!fs::exists(status)) missing.push_back(entry->path());
        entry.increment(ec);
        if (ec) return ec;
    }
    for (const auto& path : missing) {
        fs::rename(path, gui_libs / path.filename(), ec);
        if (ec) return ec;
    }
    return {};
}

QString checkPortableLaunch(const std::filesystem::path& exe) {
    QProcess probe;
    probe.start(QString::fromUtf8(exe.c_str()), {QStringLiteral("--version")});
    if (!probe.waitForFinished(10000)) return probe.errorString();
    if (probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0) return {};

    const auto detail = QString::fromUtf8(probe.readAllStandardError()).trimmed();
    return detail.isEmpty() ? QStringLiteral("退出码 %1").arg(probe.exitCode()) : detail;
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
        const auto work_template = target_dir.parent_path() / ".p2premote-update-XXXXXX";
        const QTemporaryDir work_dir(QString::fromUtf8(work_template.c_str()));
        if (!work_dir.isValid()) {
            return {false, false, QStringLiteral("无法创建更新目录：%1").arg(work_dir.errorString())};
        }
        const auto work_path = std::filesystem::path(work_dir.path().toUtf8().constData());
        const auto unpacked = work_path / "app";
        if (!p2premote::update::ExtractTarGzArchive(archive_path, unpacked, &err)) {
            return {false, false, QStringLiteral("解压更新包失败：%1").arg(QString::fromUtf8(err))};
        }
        const auto runtime = unpacked / "usr/local/libexec/p2premote";
        auto staged_gui = unpacked;
        const auto packaged_gui = runtime / "gui";
        if (std::filesystem::is_regular_file(packaged_gui / exe.filename())) {
            if (const auto ec = stagePortableLibraries(runtime)) {
                return {false, false, QStringLiteral("准备更新运行库失败：%1").arg(QString::fromUtf8(ec.message()))};
            }
            // Probe the same layout that will be installed, without access to the package's sibling lib directory.
            staged_gui = work_path / "gui";
            std::error_code ec;
            std::filesystem::rename(packaged_gui, staged_gui, ec);
            if (ec) {
                return {false, false, QStringLiteral("准备新版程序文件失败：%1").arg(QString::fromUtf8(ec.message()))};
            }
        }
        const auto launch_error = checkPortableLaunch(staged_gui / exe.filename());
        if (!launch_error.isEmpty()) {
            return {false, false, QStringLiteral("新版启动检查失败，已保留原版本。\n%1").arg(launch_error)};
        }
        if (!p2premote::update::SwapDirectoryFiles(staged_gui, target_dir, exe.filename().string(), &err)) {
            return {false, false, QStringLiteral("替换程序文件失败（已回滚）：%1").arg(QString::fromUtf8(err))};
        }
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
