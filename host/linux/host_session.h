#pragma once

#include "capture_backend.h"
#include "desktop_audio.h"
#include "lan_signal_server.h"
#include "uinput_injector.h"

#include "managed_api.h"
#include <p2premote/input_protocol.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace p2premote::linux_host {

struct HostConfig {
    std::string access_code;
    std::string display_name;
    std::uint16_t port{kDefaultLanPort};
    std::uint16_t discovery_port{kDefaultDiscoveryPort};
    unsigned fps{30};
    int max_height{1080};
    std::string log_dir;
};

class LinuxHostSession final {
public:
    struct CloudCallbacks {
        std::function<void(std::string, std::uint64_t)> send_signal;
        std::function<void(std::uint64_t)> disconnected;
        std::function<void(std::string_view)> status;
    };

    LinuxHostSession(HostConfig config, std::unique_ptr<LinuxCaptureBackend> capturer,
        std::optional<CloudCallbacks> cloud = std::nullopt);
    ~LinuxHostSession();

    LinuxHostSession(const LinuxHostSession&) = delete;
    LinuxHostSession& operator=(const LinuxHostSession&) = delete;

    bool Valid() const noexcept { return capture_thread_.joinable(); }
    bool StartCloudPeer(TurnMode turn);
    bool ReceiveCloudSignal(const std::string& payload);
    void StopCloudPeer();
    std::uint64_t PeerGeneration() const noexcept { return peer_generation_.load(); }

private:
    void OnClientAuthed(const std::string& peer_ip);
    void OnMiniRtcSignal(const std::string& payload);
    void OnClientEnded(const std::string& reason);

    bool StartPeer(TurnMode turn);
    void StopPeer();
    void PublishState(std::string_view state);
    void CaptureLoop(std::stop_token stop);
    std::optional<DesktopAudioState> AudioState();
    void SendDesktopAudio(DesktopAudioState state, const char* data, std::size_t size);
    void HandleData(const char* data, std::size_t size, const char* remote_peer_id, std::size_t remote_peer_id_size,
        const char* source, std::size_t source_size);

    static void SignalOut(const char* data, std::size_t size, void* ctx) noexcept;
    static void ConnectionStatusCb(ConnectionStatus status, const char*, std::size_t, void* ctx);
    static void DataCb(const char* data, std::size_t size, const char* remote_peer_id, std::size_t remote_peer_id_size,
        const char* source, std::size_t source_size, void* ctx);

    HostConfig config_;
    std::optional<CloudCallbacks> cloud_;
    const p2premote::native_minirtc::Api* api_{};
    std::unique_ptr<LinuxCaptureBackend> capturer_;
    UinputInjector injector_;

    std::mutex peer_mutex_;
    PeerPtr* peer_{};
    int console_lock_{-1};
    std::atomic_bool peer_active_{false};
    std::atomic_bool connected_{false};
    std::atomic_bool cursor_ready_{false};
    std::atomic_bool force_cursor_{false};
    std::atomic<int> max_height_{1080};
    std::atomic<std::uint64_t> peer_generation_{};

    std::mutex input_mutex_;
    InputReceiveState input_state_;
    std::uint64_t last_input_ms_{};
    bool frame_sent_{};
    bool speaker_enabled_{};
    std::uint64_t audio_revision_{};

    std::unique_ptr<LanSignalServer> server_;
    std::vector<std::uint8_t> scaled_bgra_;
    std::vector<std::uint8_t> nv12_;
    std::jthread capture_thread_;
    std::jthread audio_thread_;
};

} // namespace p2premote::linux_host
