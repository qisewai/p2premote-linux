#pragma once

#include <filesystem>
#include <string>
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
    std::filesystem::path latency_trace_dir;
    std::vector<IceServerOptions> ice_servers;
    bool relay_only{};
    bool auto_control{};
    bool hardware_video{};
    bool desktop_duplication{};
    bool video_cursor{};
    bool test_playout_delay{};
    bool test_present_immediate{};
};

ConnectionOptions ReadInternetConfig(const std::filesystem::path& path);

} // namespace p2premote
