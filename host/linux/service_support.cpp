#include "service_support.h"

#include "host_session.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

namespace p2premote::linux_host {
namespace {

constexpr char config_directory[] = "/etc/p2premote";
constexpr char config_name[] = "host.conf";
constexpr char service_name[] = "p2premote-host.service";
constexpr std::size_t max_config_size = 40;
constexpr char cloud_config_name[] = "cloud-host.conf";
constexpr char cloud_service_name[] = "p2premote-cloud-host.service";
constexpr std::size_t max_cloud_config_size = 4096;

class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() { if (value_ >= 0) close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
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

bool WriteConfig(int directory, const char* target, std::string_view data) {
    std::string path = std::string(config_directory) + '/' + target + ".XXXXXX";
    const Fd file(mkostemp(path.data(), O_CLOEXEC));
    if (file.get() < 0) return false;
    const char* name = std::strrchr(path.c_str(), '/') + 1;
    while (!data.empty()) {
        const auto n = write(file.get(), data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    const bool success = data.empty() && fsync(file.get()) == 0 &&
        renameat(directory, name, directory, target) == 0;
    if (!success) unlinkat(directory, name, 0);
    return success;
}

bool Systemctl(const char* unit, const char* action, const char* option = "--quiet") {
    const pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/systemctl", "systemctl", action, option, unit, nullptr);
        _exit(127);
    }
    if (pid < 0) return false;
    int status{};
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
    return TrustedDirectory(directory.get()) && ReadConfig(directory.get(), cloud_config_name, max_cloud_config_size, config);
}

namespace {

int ManageService(const char* unit, const char* name, std::size_t limit, bool enable, bool erase_when_disabled,
    const std::function<bool(std::string_view)>& validate) {
    if (geteuid() != 0) {
        std::cerr << "Administrator authorization is required.\n";
        return 1;
    }

    // Read the verification code from stdin so it stays out of process arguments.
    std::string data;
    if (enable && (!ReadBounded(STDIN_FILENO, data, limit) || !validate(data))) {
        std::cerr << "Invalid P2P Remote service configuration.\n";
        return 2;
    }
    if (mkdir(config_directory, 0700) != 0 && errno != EEXIST) {
        std::cerr << "Could not create the service configuration directory.\n";
        return 1;
    }
    const Fd directory(open(config_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!TrustedDirectory(directory.get()) || flock(directory.get(), LOCK_EX) != 0) {
        std::cerr << "Could not lock the service configuration directory.\n";
        return 1;
    }
    if (!enable) {
        if (!Systemctl(unit, "disable", "--now")) return 1;
        if (erase_when_disabled && unlinkat(directory.get(), name, 0) != 0 && errno != ENOENT) {
            std::cerr << "Could not remove the saved cloud service configuration.\n";
            return 1;
        }
        return 0;
    }

    std::optional<std::string> previous;
    if (!ReadConfig(directory.get(), name, limit, previous) || !WriteConfig(directory.get(), name, data)) {
        std::cerr << "Could not save the system service configuration.\n";
        return 1;
    }
    const bool was_enabled = Systemctl(unit, "is-enabled");
    const bool was_running = Systemctl(unit, "is-active");
    if (Systemctl(unit, "enable") && Systemctl(unit, "restart")) return 0;

    const bool restored = previous ? WriteConfig(directory.get(), name, *previous)
                                   : unlinkat(directory.get(), name, 0) == 0;
    if (!restored) std::cerr << "Could not restore the previous service configuration.\n";
    if (!was_enabled) Systemctl(unit, "disable");
    if (was_running && restored) Systemctl(unit, "restart");
    else Systemctl(unit, "stop");
    std::cerr << "Could not enable or start the P2P Remote service.\n";
    return 1;
}

void NotifyState(const char* prefix, std::string_view state, std::string_view identity, bool ready) noexcept {
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
    char message[128];
    const auto length = std::snprintf(message, sizeof(message), "%sSTATUS=%s %.*s %.*s",
        ready ? "READY=1\n" : "", prefix, static_cast<int>(state.size()), state.data(),
        static_cast<int>(identity.size()), identity.data());
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(message)) return;
    const auto address_size = offsetof(sockaddr_un, sun_path) + size + (*endpoint == '/' ? 1 : 0);
    sendto(fd.get(), message, static_cast<std::size_t>(length), MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&address), static_cast<socklen_t>(address_size));
}

} // namespace

int ManageHostService(bool enable) {
    return ManageService(service_name, config_name, max_config_size, enable, false,
        [](std::string_view data) { HostConfig config; return ParseConfig(data, config); });
}

int ManageCloudHostService(bool enable, const std::function<bool(std::string_view)>& validate) {
    return ManageService(cloud_service_name, cloud_config_name, max_cloud_config_size, enable, true, validate);
}

void NotifyHostState(std::string_view state, std::uint16_t port, bool ready) noexcept {
    char value[6];
    const auto converted = std::to_chars(value, value + sizeof(value), port);
    NotifyState("p2premote", state, {value, static_cast<std::size_t>(converted.ptr - value)}, ready);
}

void NotifyCloudState(std::string_view state, std::string_view device, bool ready) noexcept {
    NotifyState("p2premote-cloud", state, device, ready);
}

} // namespace p2premote::linux_host
