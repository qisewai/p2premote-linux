#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote {

enum class ConnectionMode { session_code, lan_direct, internet };
enum class IcePath { unknown, host, nat, relay };

struct IceServerOptions {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

struct ConnectionOptions {
    ConnectionMode mode{};
    std::wstring signal_url;
    std::wstring access_token;
    std::wstring remote_device;
    std::filesystem::path latency_trace_dir;
    std::filesystem::path ice_trace_dir;
    std::vector<IceServerOptions> ice_servers;
    bool relay_only{};
    bool managed{};
    bool auto_control{};
    bool hardware_video{};
    bool fixed_cadence{};
    bool desktop_duplication{};
    bool video_cursor{};
    bool test_playout_delay{};
    bool test_present_immediate{};

    [[nodiscard]] bool DynamicCadenceEnabled() const noexcept {
        return mode != ConnectionMode::session_code && !fixed_cadence;
    }

    [[nodiscard]] bool SupportsQualitySelection() const noexcept {
        return mode == ConnectionMode::lan_direct || managed;
    }

    [[nodiscard]] bool SupportsLocalCursor() const noexcept {
        return mode == ConnectionMode::lan_direct || managed;
    }
};

ConnectionOptions ReadInternetConfig(const std::filesystem::path& path);
ConnectionOptions ParseInternetConfig(std::wstring_view text, bool managed);

} // namespace p2premote
