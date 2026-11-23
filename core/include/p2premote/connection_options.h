#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote {

enum class IcePath { unknown, host, nat, relay };

struct IceServerOptions {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

struct ConnectionOptions {
    std::wstring signal_url;
    std::wstring access_token;
    std::wstring remote_device;
    std::filesystem::path latency_trace_dir;
    std::filesystem::path ice_trace_dir;
    std::vector<IceServerOptions> ice_servers;
    bool relay_only{};
    bool auto_control{};
    bool fixed_cadence{};
    bool desktop_duplication{true};
    bool video_cursor{};
    bool test_present_immediate{};

    [[nodiscard]] bool DynamicCadenceEnabled() const noexcept {
        return !fixed_cadence;
    }
};

ConnectionOptions ParseInternetConfig(std::wstring_view text);

} // namespace p2premote
