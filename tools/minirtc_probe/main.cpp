#include "minirtc.h"
#include "desktop_probe.h"
#include "minirtc_desktop_video.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char stream_id[] = "probe_echo";
constexpr int quality_width = 1920, quality_height = 1080;
constexpr auto quality_luma = static_cast<size_t>(quality_width) * quality_height;
using Datagram = std::array<char, 5>;

const char* PathName(TraversalMode path) {
    switch (path) {
    case P2P: return "direct";
    case Relay: return "relay";
    default: return "unknown";
    }
}

std::uint32_t Sequence(const Datagram& packet) {
    std::uint32_t value{};
    for (auto i = 1u; i < packet.size(); ++i) value = (value << 8) | static_cast<unsigned char>(packet[i]);
    return value;
}

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Datagram> received;
    bool signed_in{}, connected{}, failed{}, ended{};
    bool saw_direct{}, saw_relay{}, srtp{};
    TraversalMode path{UnknownMode};
    Clock::time_point start{Clock::now()};
    desktop_probe::DesktopProbe* desktop{};
    bool quality_check{};
    unsigned quality_frames{}, measured_frames{}, undersized_frames{}, weak_detail_frames{};
    Clock::time_point first_video{}, last_video{};
};

void Signal(SignalStatus status, const char*, size_t, void* user) {
    auto& state = *static_cast<State*>(user);
    std::lock_guard lock(state.mutex);
    if (status == SignalConnected) {
        state.signed_in = true;
        std::cout << "SIGNAL_READY" << std::endl;
    }
    if (status == SignalFailed || status == SignalTlsCertError || status == SignalServerClosed) {
        state.failed = true;
        std::cout << "SIGNAL_FAILURE=" << static_cast<int>(status) << std::endl;
    }
    state.changed.notify_all();
}

void Connection(ConnectionStatus status, const char*, size_t, void* user) {
    auto& state = *static_cast<State*>(user);
    std::lock_guard lock(state.mutex);
    std::cout << "CONNECTION_STATE=" << static_cast<int>(status) << std::endl;
    state.connected = state.connected || status == Connected;
    state.failed = state.failed || status == Failed || status == IncorrectPassword ||
        status == NoSuchTransmissionId || status == RemoteUnavailable;
    state.ended = state.ended || status == Closed;
    state.changed.notify_all();
}

void Network(const char*, size_t, TraversalMode path, const MiniRtcNetTrafficStats* stats,
    const char*, size_t, void* user) {
    if (path == UnknownMode) return;
    auto& state = *static_cast<State*>(user);
    std::lock_guard lock(state.mutex);
    if (state.path != path) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - state.start);
        std::cout << "ROUTE=" << PathName(path) << " elapsed_ms=" << elapsed.count() << " rtt_ms=" << stats->rtt_ms << std::endl;
    }
    state.path = path;
    state.saw_direct = state.saw_direct || path == P2P;
    state.saw_relay = state.saw_relay || path == Relay;
    state.srtp = stats->srtp_active;
}

void Receive(const char* data, size_t size, const char*, size_t,
    const char* source, size_t source_size, void* user) {
    auto& state = *static_cast<State*>(user);
    const std::string_view label(source, source_size);
    if (state.desktop && label == desktop_probe::input_stream) {
        state.desktop->ReceiveInput(data, size);
        return;
    }
    if (size != Datagram{}.size() || label != stream_id) return;
    std::lock_guard lock(state.mutex);
    if (state.received.size() == 64) return;
    auto& packet = state.received.emplace_back();
    std::copy_n(data, size, packet.begin());
    state.changed.notify_all();
}

void ReceiveVideo(const MiniRtcVideoFrame* frame, const char*, size_t,
    const char* source, size_t source_size, void* user) {
    auto& state = *static_cast<State*>(user);
    if (!frame || std::string_view(source, source_size) != desktop_probe::video_stream) return;
    if (state.desktop) {
        state.desktop->ReceiveVideo(*frame);
    } else if (state.quality_check) {
        const std::lock_guard lock(state.mutex);
        state.last_video = Clock::now();
        if (!state.quality_frames++) state.first_video = state.last_video;
        if (state.last_video - state.first_video < 4s) return;
        ++state.measured_frames;
        if (frame->width != quality_width || frame->height != quality_height) {
            ++state.undersized_frames;
            return;
        }
        if (!frame->data || frame->size < quality_luma) {
            ++state.weak_detail_frames;
            return;
        }
        // Compare alternating 4-pixel dark and bright strokes after decoding.
        const auto* row = reinterpret_cast<const std::uint8_t*>(frame->data) + 128 * quality_width;
        int contrast{};
        for (int x = 34; x < 290; x += 8) contrast += static_cast<int>(row[x + 4]) - row[x];
        if (contrast < 32 * 120) ++state.weak_detail_frames;
    }
}

bool Quality(PeerPtr* peer, State& state, bool host, std::chrono::seconds duration) {
    auto next = Clock::now();
    const auto deadline = next + duration;
    if (host) {
        std::vector<std::uint8_t> pixels(quality_luma * 3 / 2, 128);
        for (int y = 96; y < 160; ++y) {
            auto* row = pixels.data() + y * quality_width;
            for (int x = 32; x < 296; ++x) row[x] = (x / 4 % 2) ? 225 : 24;
        }
        unsigned frames{};
        int old_x = 0;
        MiniRtcVideoFrame frame{};
        frame.data = reinterpret_cast<const char*>(pixels.data());
        frame.size = pixels.size();
        frame.width = quality_width;
        frame.height = quality_height;
        while (Clock::now() < deadline) {
            {
                const std::lock_guard lock(state.mutex);
                if (state.failed || state.ended) break;
            }
            const int x = frames * 13 % (quality_width - 192);
            for (int y = 260; y < 440; ++y) {
                auto* row = pixels.data() + y * quality_width;
                std::fill_n(row + old_x, 192, 128);
                std::fill_n(row + x, 192, 205);
            }
            old_x = x;
            if (SendVideoFrame(peer, &frame, desktop_probe::video_stream) != 0) return false;
            ++frames;
            next = std::max(next + 20ms, Clock::now());
            std::this_thread::sleep_until(next);
        }
        std::cout << "QUALITY_SENT frames=" << frames << " width=" << quality_width << " height=" << quality_height << std::endl;
        return frames > 0;
    }
    std::unique_lock lock(state.mutex);
    state.changed.wait_until(lock, deadline, [&] { return state.failed || state.ended; });
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(state.last_video - state.first_video).count();
    const bool pass = elapsed >= duration.count() - 8 && state.measured_frames > 0 &&
        state.undersized_frames == 0 && state.weak_detail_frames == 0;
    std::cout << "QUALITY=" << (pass ? "pass" : "fail") << " frames=" << state.quality_frames
        << " measured=" << state.measured_frames << " seconds=" << elapsed << " undersized=" << state.undersized_frames
        << " weak_detail=" << state.weak_detail_frames << std::endl;
    return pass;
}

void Wait(State& state, bool State::* member, std::chrono::seconds timeout, const char* error) {
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait_for(lock, timeout, [&] { return state.*member || state.failed || state.ended; }) || !(state.*member)) {
        throw std::runtime_error(error);
    }
}

int Echo(PeerPtr* peer, State& state, bool host, std::chrono::seconds duration) {
    auto next = Clock::now();
    const auto deadline = next + duration;
    std::uint32_t sequence{}, matched{};
    double minimum_rtt = -1;
    std::map<std::uint32_t, Clock::time_point> pending;
    while (Clock::now() < deadline) {
        if (!host && Clock::now() >= next) {
            ++sequence;
            const Datagram ping{'Q', static_cast<char>(sequence >> 24), static_cast<char>(sequence >> 16),
                static_cast<char>(sequence >> 8), static_cast<char>(sequence)};
            pending.emplace(sequence, Clock::now());
            if (SendDataFrame(peer, ping.data(), ping.size(), stream_id) != 0) throw std::runtime_error("send failed");
            next = Clock::now() + 250ms;
        }
        std::deque<Datagram> packets;
        {
            std::unique_lock lock(state.mutex);
            state.changed.wait_until(lock, host ? deadline : std::min(next, deadline), [&] {
                return !state.received.empty() || state.failed || state.ended;
            });
            if (state.failed) throw std::runtime_error("connection failed during echo");
            if (state.ended && state.received.empty()) break;
            packets.swap(state.received);
        }
        for (auto& packet : packets) {
            if (host && packet[0] == 'Q') {
                packet[0] = 'A';
                if (SendDataFrame(peer, packet.data(), packet.size(), stream_id) != 0) throw std::runtime_error("echo failed");
                ++matched;
            } else if (!host && packet[0] == 'A') {
                if (const auto at = pending.find(Sequence(packet)); at != pending.end()) {
                    const double rtt = std::chrono::duration<double, std::milli>(Clock::now() - at->second).count();
                    if (minimum_rtt < 0 || rtt < minimum_rtt) minimum_rtt = rtt;
                    pending.erase(at);
                    ++matched;
                }
            }
        }
    }
    const std::lock_guard lock(state.mutex);
    const bool success = matched > 0 && state.path != UnknownMode;
    std::cout << "RESULT=" << (success ? "pass" : "fail") << " role=" << (host ? "host" : "viewer")
        << " sent=" << (host ? matched : sequence) << " matched=" << matched << " minimum_echo_ms=" << minimum_rtt
        << " path=" << PathName(state.path) << " saw_direct=" << state.saw_direct << " saw_relay=" << state.saw_relay
        << " srtp=" << state.srtp << std::endl;
    return success ? 0 : 1;
}

template <size_t N>
void Copy(const std::string& value, char (&destination)[N]) {
    if (value.size() >= N) throw std::runtime_error("MiniRTC configuration field is too long");
    std::copy(value.begin(), value.end(), destination);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 7 && argc != 8) {
            throw std::runtime_error("usage: p2premote_minirtc_probe host|viewer wss://127.0.0.1:PORT/signal identity-file ca.pem direct|auto seconds [desktop|desktop-check|quality-check]");
        }
        const std::string_view role = argv[1], mode = argv[5], duration = argv[6];
        const std::string_view workload = argc == 8 ? argv[7] : "";
        if (argc == 8 && workload != "desktop" && workload != "desktop-check" && workload != "quality-check") {
            throw std::runtime_error("invalid probe workload");
        }
        int seconds{};
        const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
        if ((role != "host" && role != "viewer") || (mode != "direct" && mode != "auto") ||
            parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() || seconds < 5 || seconds > 120) {
            throw std::runtime_error("invalid role, mode or duration (5-120 seconds)");
        }
        const std::string server = argv[2];
        if (server.rfind("wss://127.0.0.1:", 0) != 0 || server.size() < 24 || server.substr(server.size() - 7) != "/signal") {
            throw std::runtime_error("use the loopback WSS address printed by minirtcsignal");
        }
        const auto identity_file = std::filesystem::absolute(argv[3]);
        std::ifstream input(identity_file);
        std::string identity;
        if (!std::getline(input, identity) || identity.size() != role.size() + 65 || identity.compare(0, role.size(), role) != 0 ||
            identity[role.size()] != '@' || identity.find_first_not_of("0123456789abcdef", role.size() + 1) != std::string::npos) {
            throw std::runtime_error("invalid probe identity file");
        }
        const auto ca_file = std::filesystem::absolute(argv[4]);
        if (!std::filesystem::is_regular_file(ca_file) || !SetEnvironmentVariableA("P2PREMOTE_MINIRTC_PROBE_CA", ca_file.string().c_str())) {
            throw std::runtime_error("invalid probe CA file");
        }
        const auto log_directory = identity_file.parent_path() / ("logs-" + std::string(role));
        std::filesystem::create_directories(log_directory);
        State state;
        const bool host = role == "host";
        state.quality_check = workload == "quality-check";
        std::unique_ptr<desktop_probe::DesktopProbe> desktop;
        if (workload == "desktop" || workload == "desktop-check") {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            desktop = std::make_unique<desktop_probe::DesktopProbe>(host, workload == "desktop-check");
            state.desktop = desktop.get();
        }
        Params params{};
        Copy(server, params.signal_server_ip);
        Copy(log_directory.string(), params.log_path);
        params.user_id = identity.c_str();
        params.user_data = &state;
        params.turn_mode = mode == "auto" ? TurnAutoUdpTcp : TurnDisabled;
        params.enable_srtp = true;
        params.on_signal_status = Signal;
        params.on_connection_status = Connection;
        params.on_net_status_report = Network;
        params.on_receive_data_buffer = Receive;
        const bool video = desktop || state.quality_check;
        if (video) {
            p2premote::ConfigureMiniRtcDesktopVideo(params, state.quality_check ? 60 : 30);
            params.on_receive_video_frame = ReceiveVideo;
        }
        const auto destroy = [](PeerPtr* peer) { DestroyPeer(&peer); };
        const std::unique_ptr<PeerPtr, decltype(destroy)> peer(CreatePeer(&params), destroy);
        if (!peer || (video && AddVideoStream(peer.get(), desktop_probe::video_stream) != 0) ||
            (desktop && AddDataStream(peer.get(), desktop_probe::input_stream, true) != 0) ||
            (!video && AddDataStream(peer.get(), stream_id, false) != 0) || Init(peer.get()) != 0) {
            throw std::runtime_error("initializing MiniRTC failed");
        }
        Wait(state, &State::signed_in, 12s, "signaling login timed out");
        if (!host && JoinConnection(peer.get(), "host") != 0) throw std::runtime_error("join failed");
        Wait(state, &State::connected, 35s, "ICE connection did not become usable");
        if (video) {
            const auto duration = std::chrono::seconds(seconds);
            const bool verified = desktop ?
                desktop->Run(peer.get(), duration, [&] {
                    const std::lock_guard lock(state.mutex);
                    return state.failed || state.ended;
                }) : Quality(peer.get(), state, host, duration);
            const std::lock_guard lock(state.mutex);
            const bool success = verified && !state.failed && state.path != UnknownMode && state.srtp;
            std::cout << "RESULT=" << (success ? "pass" : "fail") << " role=" << role << " workload=" << workload
                << " path=" << PathName(state.path) << " saw_direct=" << state.saw_direct << " saw_relay=" << state.saw_relay
                << " srtp=" << state.srtp << std::endl;
            return success ? 0 : 1;
        }
        return Echo(peer.get(), state, host, std::chrono::seconds(seconds));
    } catch (const std::exception& error) {
        std::cerr << "RESULT=fail reason=" << error.what() << '\n';
        return 1;
    }
}
