#pragma once

#include "signal_socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace p2premote {

class StreamEvents;

inline constexpr std::uint16_t kLanDefaultPort = 27887;
inline constexpr std::uint16_t kLanDiscoveryPort = 27888;

struct LanPeer {
    std::wstring id;
    std::wstring name;
    std::wstring address;
    std::uint16_t port{};
    bool busy{};
    std::uint64_t last_seen_ms{};
};

class DirectSignalServer final : public ISignalTransport {
public:
    DirectSignalServer(std::shared_ptr<StreamEvents> events, std::wstring bind_address, std::uint16_t port,
        std::wstring access_code, std::uint16_t discovery_port = kLanDiscoveryPort);
    ~DirectSignalServer() override;

    void Send(std::string kind, std::string payload = {}) override;
    void EndSession() noexcept override { end_session_.store(true); }

private:
    void Run(std::stop_token stop, std::wstring bind_address, std::uint16_t port, std::wstring access_code,
        std::uint16_t discovery_port);

    std::shared_ptr<StreamEvents> events_;
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> pending_;
    std::atomic_bool end_session_{};
    std::jthread worker_;
};

class DirectSignalClient final : public ISignalTransport {
public:
    DirectSignalClient(std::shared_ptr<StreamEvents> events, std::wstring host_address, std::uint16_t port,
        std::wstring access_code);
    ~DirectSignalClient() override;

    void Send(std::string kind, std::string payload = {}) override;
    void EndSession() noexcept override { end_session_.store(true); }

private:
    void Run(std::stop_token stop, std::wstring host_address, std::uint16_t port, std::wstring access_code);

    std::shared_ptr<StreamEvents> events_;
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> pending_;
    std::atomic_bool end_session_{};
    std::jthread worker_;
};

class LanDiscoveryScanner final {
public:
    explicit LanDiscoveryScanner(std::uint16_t discovery_port = kLanDiscoveryPort);
    ~LanDiscoveryScanner();

    void Probe();
    std::vector<LanPeer> Snapshot() const;

private:
    void Run(std::stop_token stop, std::uint16_t discovery_port);

    mutable std::mutex mutex_;
    std::vector<LanPeer> peers_;
    std::atomic_bool probe_requested_{true};
    std::jthread worker_;
};

std::vector<std::wstring> LocalLanAddresses();
std::wstring FormatLocalLanAddresses();

} // namespace p2premote
