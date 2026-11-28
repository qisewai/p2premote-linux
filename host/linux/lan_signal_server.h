#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace p2premote::linux_host {

inline constexpr std::uint16_t kDefaultLanPort = 27887;
inline constexpr std::uint16_t kDefaultDiscoveryPort = 27888;

// Rewrites MiniRTC signaling payloads between host and viewer direction
// identically to Windows DirectSignalServer::RelayMiniRtc.
std::string RelayMiniRtcJson(std::string_view payload, bool from_host);

// Minimal flat JSON string helpers exposed for unit testing.
std::string JsonEscape(std::string_view text);
std::string ExtractJsonString(std::string_view json, std::string_view key);

class LanSignalServer final {
public:
    struct Callbacks {
        std::function<void(const std::string& peer_ip)> on_client_authed;
        std::function<void(const std::string& minirtc_payload)> on_minirtc;
        std::function<void(const std::string& reason)> on_ended;
    };

    LanSignalServer(std::string display_name, std::uint16_t port, std::string access_code,
        Callbacks callbacks, std::uint16_t discovery_port = kDefaultDiscoveryPort, bool allow_control = true);
    ~LanSignalServer();

    LanSignalServer(const LanSignalServer&) = delete;
    LanSignalServer& operator=(const LanSignalServer&) = delete;

    bool Start();
    void Stop();
    void SetMediaConnected(bool connected) noexcept;

    // Sends a MiniRTC signaling payload or control message ("accepted" / "ended" / "error")
    // to the currently authenticated viewer.
    void Send(std::string kind, std::string payload);
    void EndActiveClient(std::string reason);

private:
    void Run(std::stop_token stop, int listen_fd);

    Callbacks callbacks_;
    std::string display_name_;
    std::uint16_t port_{};
    std::string access_code_;
    std::uint16_t discovery_port_{};
    bool allow_control_{};
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> outbound_;
    std::atomic_bool end_client_{false};
    std::atomic_bool media_connected_{false};
    std::jthread worker_;
};

} // namespace p2premote::linux_host
