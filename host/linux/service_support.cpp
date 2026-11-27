#include "service_support.h"

#include "host_session.h"
#include "update/update_archive.h"

#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-login.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace p2premote::linux_host {
namespace {

constexpr char config_directory[] = "/etc/p2premote";
constexpr char systemd_directory[] = "/etc/systemd/system";
constexpr char enabled_marker[] = "host.enabled";
constexpr char changing_marker[] = "host.inactive";
constexpr char unattended_marker[] = "host.unattended";
constexpr char env_name[] = "host.env";
constexpr char config_name[] = "host.conf";
constexpr char service_name[] = "p2premote-host.service";
constexpr std::size_t max_config_size = 40;
constexpr char cloud_config_name[] = "cloud-host.conf";
constexpr char cloud_service_name[] = "p2premote-cloud-host.service";
constexpr std::size_t max_cloud_config_size = 4096;
constexpr auto max_install_payload_size = max_cloud_config_size;

class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() { if (value_ >= 0) close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
    void reset(int value = -1) { const int old = std::exchange(value_, value); if (old >= 0) close(old); }
private:
    int value_;
};

bool TrustedDirectory(int fd) {
    struct stat attributes{};
    return fd >= 0 && fstat(fd, &attributes) == 0 && attributes.st_uid == 0 &&
        (attributes.st_mode & 0022) == 0;
}

bool ReadBounded(int fd, std::string& data, std::size_t limit) {
    std::array<char, 512> buffer{};
    while (data.size() <= limit) {
        const auto n = read(fd, buffer.data(), buffer.size());
        if (n == 0) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        data.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return false;
}

// A missing configuration keeps compatibility with installations using host.env.
bool ReadConfig(int directory, const char* name, std::size_t limit, std::optional<std::string>& value) {
    const Fd file(openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) return errno == ENOENT;
    struct stat attributes{};
    if (fstat(file.get(), &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
        attributes.st_uid != 0 || (attributes.st_mode & 0077) != 0) return false;
    value.emplace();
    return ReadBounded(file.get(), *value, limit);
}

bool ParseConfig(std::string_view data, HostConfig& config) {
    const auto newline = data.find('\n');
    if (newline == std::string_view::npos || newline + 1 >= data.size() || !data.ends_with('\n')) return false;
    unsigned port{};
    const auto parsed = std::from_chars(data.data(), data.data() + newline, port);
    const auto code = data.substr(newline + 1, data.size() - newline - 2);
    if (parsed.ec != std::errc{} || parsed.ptr != data.data() + newline || port == 0 || port > 65535 ||
        code.size() < 6 || code.size() > 32 || code.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos) {
        return false;
    }
    config.port = static_cast<std::uint16_t>(port);
    config.access_code = code;
    return true;
}

bool WriteFileAtomic(int directory, const char* dir_path, const char* target, std::string_view data, mode_t mode) {
    std::string path = std::string(dir_path) + '/' + target + ".XXXXXX";
    const Fd file(mkostemp(path.data(), O_CLOEXEC));
    if (file.get() < 0) return false;
    const char* name = std::strrchr(path.c_str(), '/') + 1;
    while (!data.empty()) {
        const auto n = write(file.get(), data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    const bool success = data.empty() && fchmod(file.get(), mode) == 0 && fsync(file.get()) == 0 &&
        renameat(directory, name, directory, target) == 0;
    if (!success) unlinkat(directory, name, 0);
    return success;
}

bool WriteConfig(int directory, const char* target, std::string_view data) {
    return WriteFileAtomic(directory, config_directory, target, data, 0600);
}

int SystemctlExit(const char* unit, const char* action, const char* option = "--quiet") {
    const pid_t pid = fork();
    if (pid == 0) {
        if (unit) execl("/usr/bin/systemctl", "systemctl", action, option, unit, nullptr);
        else execl("/usr/bin/systemctl", "systemctl", action, nullptr);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status{};
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool Systemctl(const char* unit, const char* action, const char* option = "--quiet") {
    return SystemctlExit(unit, action, option) == 0;
}

int LockServiceChanges() {
    Fd lock(open("/run/lock/p2premote-host.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
    struct stat st{};
    if (lock.get() < 0 || fstat(lock.get(), &st) || st.st_uid != 0 ||
        !S_ISREG(st.st_mode) || (st.st_mode & 0077) || flock(lock.get(), LOCK_EX)) return -1;
    return lock.release();
}

bool RemoveFile(int directory, const char* name) {
    return unlinkat(directory, name, 0) == 0 || errno == ENOENT;
}

std::string SelfExecutablePath() {
    std::array<char, 4096> buf{};
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0 || static_cast<std::size_t>(n) >= buf.size() - 1) return {};
    std::string path(buf.data(), static_cast<std::size_t>(n));
    if (path.empty() || path.front() != '/' || path.find_first_of("\r\n\"") != std::string::npos) return {};
    return path;
}

} // namespace

bool LoadServiceConfig(HostConfig& config) {
    const Fd directory(open(config_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) return errno == ENOENT;
    std::optional<std::string> data;
    return TrustedDirectory(directory.get()) && ReadConfig(directory.get(), config_name, max_config_size, data) &&
        (!data || ParseConfig(*data, config));
}

bool LoadCloudServiceConfig(std::optional<std::string>& config) {
    const Fd directory(open(config_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) return errno == ENOENT;
    return TrustedDirectory(directory.get()) && ReadConfig(directory.get(), cloud_config_name, max_cloud_config_size, config);
}

namespace {

int ManageChannel(const char* name, std::size_t limit, bool enable, bool remove_env,
    const std::function<bool(std::string_view)>& validate) {
    if (geteuid() != 0) {
        std::cerr << "Administrator authorization is required.\n";
        return 1;
    }

    // Read the configuration payload from stdin so secrets stay out of process arguments.
    std::string data;
    if (enable && (!ReadBounded(STDIN_FILENO, data, limit) || !validate(data))) {
        std::cerr << "Invalid P2P Remote service configuration.\n";
        return 2;
    }
    const Fd lock(LockServiceChanges());
    if (lock.get() < 0) return 1;
    const Fd directory(open(config_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!TrustedDirectory(directory.get()) || faccessat(directory.get(), changing_marker, F_OK, 0) == 0) return 1;
    if (faccessat(directory.get(), enabled_marker, F_OK, 0) != 0) {
        // Persist the old opt-in before changing its last enabled channel.
        if (!Systemctl(service_name, "is-enabled") && !Systemctl(cloud_service_name, "is-enabled") &&
            !Systemctl(service_name, "is-active") && !Systemctl(cloud_service_name, "is-active")) return 1;
        if (fchmod(directory.get(), 0755) ||
            !WriteFileAtomic(directory.get(), config_directory, enabled_marker, "1\n", 0644)) return 1;
    }

    std::optional<std::string> previous;
    if (!ReadConfig(directory.get(), name, limit, previous)) {
        std::cerr << "Could not read the previous service configuration.\n";
        return 1;
    }
    const bool was_enabled = Systemctl(service_name, "is-enabled");
    const bool was_running = Systemctl(service_name, "is-active");

    if (!enable) {
        if (!RemoveFile(directory.get(), name) || (remove_env && !RemoveFile(directory.get(), env_name))) {
            std::cerr << "Could not remove the saved service configuration.\n";
            return 1;
        }
        if ((was_running || was_enabled) && !Systemctl(service_name, "restart")) {
            if (previous) WriteConfig(directory.get(), name, *previous);
            std::cerr << "Could not reload the P2P Remote service.\n";
            return 1;
        }
        return 0;
    }

    if (!WriteConfig(directory.get(), name, data)) {
        std::cerr << "Could not save the system service configuration.\n";
        return 1;
    }
    if (Systemctl(service_name, "enable") && Systemctl(service_name, "restart")) return 0;

    const bool restored = previous ? WriteConfig(directory.get(), name, *previous)
                                   : RemoveFile(directory.get(), name);
    if (!restored) std::cerr << "Could not restore the previous service configuration.\n";
    if (!was_enabled) Systemctl(service_name, "disable");
    if (was_running && restored) Systemctl(service_name, "restart");
    else Systemctl(service_name, "stop");
    std::cerr << "Could not enable or start the P2P Remote service.\n";
    return 1;
}

struct CombinedNotifyState {
    std::mutex mutex;
    std::string lan_state = "off";
    std::uint16_t lan_port = 0;
    std::string cloud_state = "off";
    std::string cloud_device = "-";
    bool ready_sent = false;
};

CombinedNotifyState& NotifyStore() noexcept {
    static CombinedNotifyState store;
    return store;
}

void SendCombinedNotifyLocked(const CombinedNotifyState& state, bool send_ready) noexcept {
    const char* endpoint = std::getenv("NOTIFY_SOCKET");
    if (!endpoint || (*endpoint != '/' && *endpoint != '@')) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto size = std::strlen(endpoint);
    if (size >= sizeof(address.sun_path)) return;
    std::memcpy(address.sun_path, endpoint, size + 1);
    if (*endpoint == '@') address.sun_path[0] = '\0';

    const Fd fd(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (fd.get() < 0) return;
    char message[256];
    const auto length = std::snprintf(message, sizeof(message),
        "%sSTATUS=p2premote lan=%s:%u cloud=%s:%s",
        send_ready ? "READY=1\n" : "",
        state.lan_state.c_str(),
        static_cast<unsigned>(state.lan_port),
        state.cloud_state.c_str(),
        state.cloud_device.empty() ? "-" : state.cloud_device.c_str());
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(message)) return;
    const auto address_size = offsetof(sockaddr_un, sun_path) + size + (*endpoint == '/' ? 1 : 0);
    sendto(fd.get(), message, static_cast<std::size_t>(length), MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&address), static_cast<socklen_t>(address_size));
}

} // namespace

int ManageHostService(bool enable) {
    return ManageChannel(config_name, max_config_size, enable, true,
        [](std::string_view data) { HostConfig config; return ParseConfig(data, config); });
}

int ManageCloudHostService(bool enable, const std::function<bool(std::string_view)>& validate) {
    return ManageChannel(cloud_config_name, max_cloud_config_size, enable, false, validate);
}

int InstallHostServices(const std::function<bool(std::string_view)>& validate_cloud) {
    if (geteuid() != 0) {
        std::cerr << "Administrator authorization is required.\n";
        return 1;
    }

    std::string payload;
    if (!ReadBounded(STDIN_FILENO, payload, max_install_payload_size)) {
        std::cerr << "Invalid P2P Remote service configuration.\n";
        return 2;
    }

    const Fd lock(LockServiceChanges());
    if (lock.get() < 0 || (mkdir(config_directory, 0755) && errno != EEXIST)) return 1;
    const Fd config(open(config_directory, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    const Fd units(open(systemd_directory, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    const auto templates = std::filesystem::path(SelfExecutablePath()).parent_path() / "units";
    const Fd source(open(templates.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!TrustedDirectory(config.get()) || !TrustedDirectory(units.get()) || !TrustedDirectory(source.get()) ||
        fchmod(config.get(), 0755)) return 1;
    if (!payload.empty() && !validate_cloud(payload)) return 2;

    struct stat st{};
    if (faccessat(config.get(), changing_marker, F_OK, 0) == 0) {
        std::cerr << "Previous service change is incomplete. Uninstall before enabling again.\n";
        return 1;
    }
    if (fstatat(config.get(), enabled_marker, &st, AT_SYMLINK_NOFOLLOW) == 0) return 0;
    if (errno != ENOENT) return 1;
    // An active legacy installation already owns its channel settings.
    if (Systemctl(service_name, "is-enabled") || Systemctl(cloud_service_name, "is-enabled") ||
        Systemctl(service_name, "is-active") || Systemctl(cloud_service_name, "is-active")) {
        return WriteFileAtomic(config.get(), config_directory, enabled_marker, "1\n", 0644) ? 0 : 1;
    }
    std::optional<std::string> previous_cloud;
    if (!ReadConfig(config.get(), cloud_config_name, max_cloud_config_size, previous_cloud)) return 1;

    if (!WriteFileAtomic(config.get(), config_directory, changing_marker, "1\n", 0644)) return 1;
    bool unit_created = false;
    bool cloud_changed = false;
    bool service_started = false;
    const auto install = [&] {
        if (fstatat(units.get(), service_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022)) return false;
        } else {
            if (errno != ENOENT) return false;
            const Fd file(openat(source.get(), service_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            std::string contents;
            if (file.get() < 0 || fstat(file.get(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
                (st.st_mode & 0022) || !ReadBounded(file.get(), contents, 8192) ||
                !WriteFileAtomic(units.get(), systemd_directory, service_name, contents, 0644)) return false;
            unit_created = true;
        }
        // Clean up any legacy separate cloud unit if present.
        if (fstatat(units.get(), cloud_service_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            Systemctl(cloud_service_name, "disable", "--now");
            RemoveFile(units.get(), cloud_service_name);
        }
        if (!Systemctl(nullptr, "daemon-reload") || !RemoveFile(config.get(), unattended_marker)) return false;
        if (!payload.empty()) {
            if (!WriteConfig(config.get(), cloud_config_name, payload)) return false;
            cloud_changed = true;
        }
        if (!Systemctl(service_name, "enable") || !Systemctl(service_name, "restart")) return false;
        service_started = true;
        return WriteFileAtomic(config.get(), config_directory, enabled_marker, "1\n", 0644) &&
            RemoveFile(config.get(), changing_marker);
    };
    if (install()) return 0;

    bool restored = true;
    if (service_started || Systemctl(service_name, "is-enabled") || Systemctl(service_name, "is-active")) {
        if (!Systemctl(service_name, "disable", "--now")) {
            std::cerr << "Could not stop the host service during rollback. Retry uninstall.\n";
            return 1;
        }
    }
    if (cloud_changed) {
        restored = previous_cloud ? WriteConfig(config.get(), cloud_config_name, *previous_cloud)
                                  : RemoveFile(config.get(), cloud_config_name);
    }
    if (unit_created && !RemoveFile(units.get(), service_name)) restored = false;
    if (!Systemctl(nullptr, "daemon-reload") || !restored || !RemoveFile(config.get(), enabled_marker) ||
        !RemoveFile(config.get(), changing_marker)) std::cerr << "Could not fully roll back installation. Retry uninstall.\n";
    return 1;
}

int SetUnattended(bool enabled) {
    if (geteuid() != 0) return 1;
    const Fd lock(LockServiceChanges());
    const Fd config(open(config_directory, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (lock.get() < 0 || !TrustedDirectory(config.get()) || faccessat(config.get(), changing_marker, F_OK, 0) == 0) return 1;
    if (faccessat(config.get(), enabled_marker, F_OK, 0) != 0) {
        if (!Systemctl(service_name, "is-enabled") && !Systemctl(cloud_service_name, "is-enabled") &&
            !Systemctl(service_name, "is-active") && !Systemctl(cloud_service_name, "is-active")) return 1;
        if (fchmod(config.get(), 0755) || !WriteFileAtomic(config.get(), config_directory, enabled_marker, "1\n", 0644)) return 1;
    }
    const bool ok = enabled ? WriteFileAtomic(config.get(), config_directory, unattended_marker, "1\n", 0644)
                            : RemoveFile(config.get(), unattended_marker);
    return ok ? 0 : 1;
}

int UninstallHostServices() {
    if (geteuid() != 0) return 1;
    const Fd lock(LockServiceChanges());
    const Fd units(open(systemd_directory, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (lock.get() < 0 || !TrustedDirectory(units.get())) return 1;

    const Fd config(open(config_directory, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (config.get() < 0 && errno != ENOENT) return 1;
    if (config.get() >= 0 && (!TrustedDirectory(config.get()) ||
        !WriteFileAtomic(config.get(), config_directory, changing_marker, "1\n", 0644))) return 1;
    bool stopped = true;
    for (auto unit : {service_name, cloud_service_name}) {
        struct stat st{};
        const bool present = fstatat(units.get(), unit, &st, AT_SYMLINK_NOFOLLOW) == 0;
        if (!present && errno != ENOENT) return 1;
        if ((present || Systemctl(unit, "is-active") || Systemctl(unit, "is-enabled")) &&
            !Systemctl(unit, "disable", "--now")) stopped = false;
        const int state = SystemctlExit(unit, "is-active");
        if (state != 3 && state != 4) stopped = false;
    }
    if (!stopped) {
        std::cerr << "Could not stop and disable both host services. Configuration was retained.\n";
        return 1;
    }
    for (auto unit : {service_name, cloud_service_name}) if (!RemoveFile(units.get(), unit)) return 1;
    if (!Systemctl(nullptr, "daemon-reload")) return 1;

    if (config.get() < 0) return 0;
    for (auto name : {unattended_marker, config_name, cloud_config_name, env_name, enabled_marker, changing_marker}) {
        if (!RemoveFile(config.get(), name)) return 1;
    }
    // Preserve any unrelated administrator-owned files.
    return rmdir(config_directory) == 0 || errno == ENOTEMPTY ? 0 : 1;
}

bool IsUnattendedModeEnabled() noexcept {
    return access("/etc/p2premote/host.unattended", F_OK) == 0;
}

static bool IsUnattendedEnabled() noexcept {
    return IsUnattendedModeEnabled();
}

int ApplyLinuxSystemUpdate(std::string_view archive_path, std::string_view manifest_path) {
    if (geteuid() != 0) {
        std::cerr << "Administrator authorization is required to apply a system update.\n";
        return 1;
    }
#ifndef P2PREMOTE_UPDATE_PUBLIC_KEY
    constexpr const char* embedded_key = "";
#else
    constexpr const char* embedded_key = P2PREMOTE_UPDATE_PUBLIC_KEY;
#endif
#ifndef P2PREMOTE_VERSION
    constexpr const char* current_version = "0.2.0";
#else
    constexpr const char* current_version = P2PREMOTE_VERSION;
#endif
    if (!embedded_key || !*embedded_key) {
        std::cerr << "Update public key is not embedded in this build.\n";
        return 1;
    }

    const Fd lock(LockServiceChanges());
    if (lock.get() < 0) return 1;

    namespace fs = std::filesystem;
    using namespace p2premote::update;
    const fs::path libexec_root = "/usr/local/libexec";
    const fs::path runtime_dir = libexec_root / "p2premote";
    struct stat loaded{}, installed{};
    if (stat("/proc/self/exe", &loaded) != 0 ||
        stat((runtime_dir / "p2premote_linux_host").c_str(), &installed) != 0 ||
        loaded.st_dev != installed.st_dev || loaded.st_ino != installed.st_ino) {
        std::cerr << "The installed update helper has changed. Retry with the current version.\n";
        return 1;
    }
    const Fd mf(open(std::string(manifest_path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat info{};
    std::string manifest_text;
    if (mf.get() < 0 || fstat(mf.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        !ReadBounded(mf.get(), manifest_text, kMaxManifestBytes)) {
        std::cerr << "Could not open update manifest.\n";
        return 1;
    }
    std::string err;
    const auto manifest = ParseUpdateManifest(manifest_text, &err);
    if (!manifest || !VerifyUpdateManifest(*manifest, "linux-x64", embedded_key)) {
        std::cerr << "Invalid or unsigned Linux update manifest.\n";
        return 1;
    }
    if (p2premote::update::CompareVersions(manifest->version, current_version) <= 0) {
        std::cerr << "Refusing to downgrade or reinstall version " << manifest->version << ".\n";
        return 1;
    }
    const Fd input(open(std::string(archive_path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (input.get() < 0 || fstat(input.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != manifest->size) {
        std::cerr << "Could not open the expected update archive.\n";
        return 1;
    }

    const fs::path share_dir = "/usr/local/share/p2premote";
    std::error_code ec;
    std::string work_template = (libexec_root / ".p2premote-update-XXXXXX").string();
    if (!mkdtemp(work_template.data())) {
        std::cerr << "Could not create a private system update directory.\n";
        return 1;
    }
    const fs::path work_dir(work_template);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{work_dir};
    // Verify and unpack the same private copy of the package supplied by the GUI.
    const auto private_archive = work_dir / "update.tar.gz";
    {
        std::ofstream snapshot(private_archive, std::ios::binary);
        std::array<char, 64 * 1024> buffer{};
        auto remaining = manifest->size;
        while (snapshot && remaining) {
            const auto size = read(input.get(), buffer.data(), std::min<std::int64_t>(buffer.size(), remaining));
            if (size < 0 && errno == EINTR) continue;
            if (size <= 0) break;
            snapshot.write(buffer.data(), size);
            remaining -= size;
        }
        snapshot.close();
        if (!snapshot || remaining) {
            std::cerr << "Could not copy the update archive.\n";
            return 1;
        }
    }
    if (!VerifyArchiveFile(private_archive, manifest->size, manifest->signature, embedded_key, &err)) {
        std::cerr << "Archive verification failed: " << err << "\n";
        return 1;
    }
    const fs::path unpacked = work_dir / "stage";
    if (!ExtractTarGzArchive(private_archive, unpacked, &err)) {
        std::cerr << "Failed to extract update archive: " << err << "\n";
        return 1;
    }

    // Support both prefix-structured archives (usr/local/libexec/p2premote) and flat runtime archives.
    const fs::path staged_runtime = fs::exists(unpacked / "usr/local/libexec/p2premote/p2premote_linux_host")
        ? (unpacked / "usr/local/libexec/p2premote")
        : unpacked;
    const fs::path staged_share = unpacked / "usr/local/share/p2premote";
    if (!fs::is_regular_file(staged_runtime / "p2premote_linux_host")) {
        std::cerr << "Update archive does not contain p2premote_linux_host.\n";
        return 1;
    }

    const bool was_active = Systemctl(service_name, "is-active");
    if (was_active && !Systemctl(service_name, "stop")) return 1;

    const std::string suffix = ".old-" + std::to_string(getpid());
    const fs::path runtime_backup = libexec_root / (".p2premote" + suffix);
    const fs::path share_backup = fs::path("/usr/local/share") / (".p2premote" + suffix);
    bool moved_runtime = false;
    bool moved_share = false;
    bool installed_share = false;

    auto rollback = [&] {
        std::error_code rec;
        if (moved_runtime) {
            fs::remove_all(runtime_dir, rec);
            fs::rename(runtime_backup, runtime_dir, rec);
        }
        if (installed_share) fs::remove_all(share_dir, rec);
        if (moved_share) fs::rename(share_backup, share_dir, rec);
        Systemctl(nullptr, "daemon-reload");
        if (was_active) Systemctl(service_name, "start");
    };

    if (fs::exists(runtime_dir, ec)) {
        fs::remove_all(runtime_backup, ec);
        ec.clear();
        fs::rename(runtime_dir, runtime_backup, ec);
        if (ec) {
            rollback();
            std::cerr << "Could not back up existing runtime directory: " << ec.message() << "\n";
            return 1;
        }
        moved_runtime = true;
    }
    fs::rename(staged_runtime, runtime_dir, ec);
    if (ec) {
        rollback();
        std::cerr << "Could not install new runtime directory: " << ec.message() << "\n";
        return 1;
    }

    if (fs::is_directory(staged_share)) {
        if (fs::exists(share_dir, ec)) {
            fs::remove_all(share_backup, ec);
            ec.clear();
            fs::rename(share_dir, share_backup, ec);
            if (ec) {
                rollback();
                std::cerr << "Could not back up existing share directory: " << ec.message() << "\n";
                return 1;
            }
            moved_share = true;
        }
        fs::rename(staged_share, share_dir, ec);
        if (ec) {
            rollback();
            std::cerr << "Could not install new share directory: " << ec.message() << "\n";
            return 1;
        }
        installed_share = true;
    }

    if (was_active && (!Systemctl(service_name, "start") || !Systemctl(service_name, "is-active"))) {
        std::cerr << "Updated service failed to start; rolling back to previous version.\n";
        rollback();
        return 1;
    }

    // Keep the existing unit and launcher available until the new runtime starts.
    auto copy_if_present = [](const fs::path& src, const fs::path& dst, mode_t mode) {
        std::error_code cec;
        if (!fs::is_regular_file(src, cec) || cec) return;
        fs::create_directories(dst.parent_path(), cec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, cec);
        if (!cec) chmod(dst.c_str(), mode);
    };
    copy_if_present(runtime_dir / "units" / service_name, fs::path(systemd_directory) / service_name, 0644);
    copy_if_present(unpacked / "usr/local/bin/p2premote", "/usr/local/bin/p2premote", 0755);
    copy_if_present(unpacked / "etc/xdg/autostart/p2premote-clipboard.desktop",
                    "/etc/xdg/autostart/p2premote-clipboard.desktop", 0644);
    copy_if_present(unpacked / "usr/local/share/applications/p2premote.desktop",
                    "/usr/local/share/applications/p2premote.desktop", 0644);

    Systemctl(nullptr, "daemon-reload");
    if (moved_runtime) fs::remove_all(runtime_backup, ec);
    if (moved_share) fs::remove_all(share_backup, ec);
    return 0;
}

int ApplyDownloadedLinuxUpdate(std::string_view directory) {
    constexpr std::string_view prefix = "/tmp/p2premote-auto-";
    if (geteuid() != 0 || !directory.starts_with(prefix)) return 1;
    const auto name = directory.substr(prefix.size());
    if (name.size() != 6 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
        std::string_view::npos) return 1;
    const std::string path(directory);
    const Fd dir(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!TrustedDirectory(dir.get())) return 1;
    const int result = IsUnattendedModeEnabled()
        ? ApplyLinuxSystemUpdate(path + "/update.tar.gz", path + "/manifest.json") : 1;
    unlinkat(dir.get(), "update.tar.gz", 0);
    unlinkat(dir.get(), "manifest.json", 0);
    rmdir(path.c_str());
    return result;
}

static bool IsClientAlive() noexcept {
    // Each service keeps a connection to the verified GUI. EOF also invalidates
    // the old session when a replacement GUI has already started.
    static Fd peer(-1), process(-1);
    static uid_t owner{};
    uid_t active_uid{};
    const bool active = sd_seat_get_active("seat0", nullptr, &active_uid) >= 0;
    if (peer.get() >= 0) {
        pollfd status[]{{peer.get(), POLLIN | POLLRDHUP, 0}, {process.get(), POLLIN, 0}};
        if (active && active_uid == owner && poll(status, 2, 0) == 0) return true;
        peer.reset();
        process.reset();
        return false;
    }
    if (!active) return false;
    Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto count = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
        "p2premote-client-%u", active_uid);
    if (fd.get() < 0 || connect(fd.get(), reinterpret_cast<sockaddr*>(&address),
        offsetof(sockaddr_un, sun_path) + 1 + count)) return false;
    ucred cred{};
    socklen_t size = sizeof(cred);
    if (getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != active_uid) return false;
    Fd lifetime(static_cast<int>(syscall(SYS_pidfd_open, cred.pid, 0)));
    if (lifetime.get() < 0) return false;
    struct stat actual{}, expected{};
    const auto image = "/proc/" + std::to_string(cred.pid) + "/exe";
    if (stat(image.c_str(), &actual) || stat(P2PREMOTE_INSTALLED_GUI, &expected) ||
        expected.st_uid != 0 || (expected.st_mode & 0022) ||
        actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino) return false;
    owner = active_uid;
    process.reset(lifetime.release());
    peer.reset(fd.release());
    return true;
}

bool HostReceptionAllowed() noexcept {
    if (access("/etc/p2premote/host.inactive", F_OK) == 0 || errno != ENOENT) return false;
    return IsUnattendedEnabled() || IsClientAlive();
}

void NotifyHostState(std::string_view state, std::uint16_t port, bool ready) noexcept {
    auto& store = NotifyStore();
    std::lock_guard<std::mutex> guard(store.mutex);
    store.lan_state.assign(state);
    store.lan_port = port;
    const bool emit_ready = ready && !store.ready_sent;
    if (emit_ready) store.ready_sent = true;
    if (store.ready_sent) SendCombinedNotifyLocked(store, emit_ready);
}

void NotifyCloudState(std::string_view state, std::string_view device, bool ready) noexcept {
    auto& store = NotifyStore();
    std::lock_guard<std::mutex> guard(store.mutex);
    store.cloud_state.assign(state);
    store.cloud_device.assign(device.empty() ? std::string_view("-") : device);
    const bool emit_ready = ready && !store.ready_sent;
    if (emit_ready) store.ready_sent = true;
    if (store.ready_sent) SendCombinedNotifyLocked(store, emit_ready);
}

void NotifyServiceReady() noexcept {
    auto& store = NotifyStore();
    std::lock_guard<std::mutex> guard(store.mutex);
    const bool emit_ready = !store.ready_sent;
    store.ready_sent = true;
    SendCombinedNotifyLocked(store, emit_ready);
}

} // namespace p2premote::linux_host
