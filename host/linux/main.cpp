#include "host_session.h"
#include "cloud_host.h"
#include "desktop_audio.h"
#include "service_support.h"
#include "x11_shm_capturer.h"

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

std::atomic_bool g_stop{false};

void OnSignal(int) noexcept {
    g_stop.store(true, std::memory_order_release);
}

std::string DefaultHostname() {
    char buf[256]{};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        return std::string(buf);
    }
    return "Linux Host";
}

void PrintUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " --code <6-32 chars> [--port 27887] [--fps 30] [--max-height 1080] [--name <hostname>]\n"
              << "  Environment fallback: P2PREMOTE_ACCESS_CODE\n";
}

int ParseNumber(std::string_view text) {
    int number{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? number : -1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace p2premote::linux_host;

    if (argc == 2 && std::string_view(argv[1]) == "--capture-desktop-audio") return RunDesktopAudioCapture();
    if (argc == 2 && std::string_view(argv[1]) == "--enable-service") return ManageHostService(true);
    if (argc == 2 && std::string_view(argv[1]) == "--disable-service") return ManageHostService(false);
    if (argc == 2 && std::string_view(argv[1]) == "--enable-cloud-service") {
        return ManageCloudHostService(true, ValidCloudHostConfig);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--disable-cloud-service") {
        return ManageCloudHostService(false, ValidCloudHostConfig);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--enable-unattended") return SetUnattended(true);
    if (argc == 2 && std::string_view(argv[1]) == "--disable-unattended") return SetUnattended(false);
    if (argc == 2 && std::string_view(argv[1]) == "--install-host-service") {
        return InstallHostServices(ValidCloudHostConfig);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--uninstall-host-service") {
        return UninstallHostServices();
    }

    HostConfig cfg;
    bool system_service = false;
    cfg.display_name = DefaultHostname();
    if (const char* env_code = std::getenv("P2PREMOTE_ACCESS_CODE"); env_code && *env_code) {
        cfg.access_code = env_code;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_val = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (arg == "--system-service") {
            system_service = true;
        } else if (arg == "--code") {
            if (!next_val(cfg.access_code)) { PrintUsage(argv[0]); return 2; }
        } else if (arg == "--name") {
            if (!next_val(cfg.display_name)) { PrintUsage(argv[0]); return 2; }
        } else if (arg == "--port") {
            std::string v;
            if (!next_val(v)) { PrintUsage(argv[0]); return 2; }
            const int parsed = ParseNumber(v);
            if (parsed <= 0 || parsed > 65535) { PrintUsage(argv[0]); return 2; }
            cfg.port = static_cast<std::uint16_t>(parsed);
        } else if (arg == "--fps") {
            std::string v;
            if (!next_val(v)) { PrintUsage(argv[0]); return 2; }
            const int parsed = ParseNumber(v);
            if (parsed < 5 || parsed > 60) { PrintUsage(argv[0]); return 2; }
            cfg.fps = static_cast<unsigned>(parsed);
        } else if (arg == "--max-height") {
            std::string v;
            if (!next_val(v)) { PrintUsage(argv[0]); return 2; }
            const int parsed = ParseNumber(v);
            if (parsed < 360 || parsed > 2160) { PrintUsage(argv[0]); return 2; }
            cfg.max_height = parsed;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (system_service) {
        return RunSystemService(argc, argv, std::move(cfg));
    }
    if (cfg.access_code.size() < 6 || cfg.access_code.size() > 32) {
        std::cerr << "Error: --code (or P2PREMOTE_ACCESS_CODE) must be 6 to 32 characters.\n";
        PrintUsage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    char temp_logs[] = "/tmp/p2premote-minirtc.XXXXXX";
    if (mkdtemp(temp_logs)) cfg.log_dir = temp_logs;
    else {
        std::cerr << "Error: could not create a private MiniRTC log directory.\n";
        return 1;
    }

    auto session = std::make_unique<LinuxHostSession>(cfg, std::make_unique<X11ShmCapturer>());
    if (!session->Valid()) {
        std::cerr << "Error: failed to initialize Linux host session.\n";
        return 1;
    }

    std::cout << "[host] P2P Remote Linux Host running (backend=X11Shm+uinput, fps="
              << cfg.fps << ", max_height=" << cfg.max_height << ")\n" << std::flush;

    while (!g_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::cout << "[host] shutting down...\n" << std::flush;
    return 0;
}
