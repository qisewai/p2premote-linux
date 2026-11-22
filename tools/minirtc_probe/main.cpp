#include "minirtc.h"

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

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char stream_id[] = "probe_echo";
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
    if (size != Datagram{}.size() || std::string_view(source, source_size) != stream_id) return;
    auto& state = *static_cast<State*>(user);
    std::lock_guard lock(state.mutex);
    if (state.received.size() == 64) return;
    auto& packet = state.received.emplace_back();
    std::copy_n(data, size, packet.begin());
    state.changed.notify_all();
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
        if (argc != 7) {
            throw std::runtime_error("usage: p2premote_minirtc_probe host|viewer wss://127.0.0.1:PORT/signal identity-file ca.pem direct|auto seconds");
        }
        const std::string_view role = argv[1], mode = argv[5], duration = argv[6];
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
        const auto destroy = [](PeerPtr* peer) { DestroyPeer(&peer); };
        const std::unique_ptr<PeerPtr, decltype(destroy)> peer(CreatePeer(&params), destroy);
        if (!peer || AddDataStream(peer.get(), stream_id, false) != 0 || Init(peer.get()) != 0) {
            throw std::runtime_error("initializing MiniRTC failed");
        }
        Wait(state, &State::signed_in, 12s, "signaling login timed out");
        const bool host = role == "host";
        if (!host && JoinConnection(peer.get(), "host") != 0) throw std::runtime_error("join failed");
        Wait(state, &State::connected, 35s, "ICE connection did not become usable");
        return Echo(peer.get(), state, host, std::chrono::seconds(seconds));
    } catch (const std::exception& error) {
        std::cerr << "RESULT=fail reason=" << error.what() << '\n';
        return 1;
    }
}
