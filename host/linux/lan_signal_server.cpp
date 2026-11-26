#include "lan_signal_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace p2premote::linux_host {
namespace {

constexpr std::size_t kMaxFrameBytes = 256 * 1024;
constexpr unsigned kMaxAuthFailures = 5;
constexpr std::uint64_t kLockoutMs = 60'000;
constexpr std::uint64_t kBeaconIntervalMs = 2'500;
constexpr std::uint64_t kAuthTimeoutMs = 3'000;
constexpr std::uint64_t kMediaConnectTimeoutMs = 25'000;
constexpr std::size_t kMaxPendingClients = 16;

std::uint64_t NowMs() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string HexEncode(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

std::string RandomHex(std::size_t bytes) {
    std::vector<std::uint8_t> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return HexEncode(buf.data(), buf.size());
}

std::array<std::uint8_t, 32> Sha256(std::string_view input) {
    std::array<std::uint8_t, 32> digest{};
    unsigned int len = 0;
    if (EVP_Digest(input.data(), input.size(), digest.data(), &len, EVP_sha256(), nullptr) != 1 || len != 32) {
        throw std::runtime_error("EVP_Digest SHA-256 failed");
    }
    return digest;
}

std::string HmacSha256Hex(const std::array<std::uint8_t, 32>& key, std::string_view msg) {
    std::array<std::uint8_t, 32> digest{};
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), digest.data(), &len) || len != 32) {
        throw std::runtime_error("HMAC-SHA256 failed");
    }
    return HexEncode(digest.data(), digest.size());
}

bool ConstantTimeEqual(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

class FdGuard {
public:
    FdGuard() = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    ~FdGuard() { Reset(); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    FdGuard& operator=(FdGuard&& o) noexcept {
        if (this != &o) {
            Reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    int Get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void Reset(int next = -1) noexcept {
        if (fd_ >= 0) close(fd_);
        fd_ = next;
    }
private:
    int fd_{-1};
};

void SetNonBlocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool SendAll(int fd, const char* data, std::size_t size, std::uint64_t timeout_ms = 3000) noexcept {
    const auto deadline = NowMs() + timeout_ms;
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            const auto now = NowMs();
            if (now >= deadline) return false;
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, static_cast<int>(std::min<std::uint64_t>(deadline - now, 50))) < 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool SendFramedJson(int fd, std::string_view json, std::uint64_t timeout_ms = 3000) {
    if (json.size() > kMaxFrameBytes) return false;
    const std::uint32_t len_be = htonl(static_cast<std::uint32_t>(json.size()));
    std::string packet;
    packet.resize(4 + json.size());
    std::memcpy(packet.data(), &len_be, 4);
    std::memcpy(packet.data() + 4, json.data(), json.size());
    return SendAll(fd, packet.data(), packet.size(), timeout_ms);
}

bool SendTyped(int fd, std::string_view type, std::string_view payload, std::uint64_t timeout_ms = 3000) {
    return SendFramedJson(fd, "{\"type\":\"" + JsonEscape(type) + "\",\"payload\":\"" + JsonEscape(payload) + "\"}",
        timeout_ms);
}

struct FrameReader {
    std::vector<char> buf;
    void Clear() noexcept { buf.clear(); }

    bool Pump(int fd, std::vector<std::string>& frames) {
        char tmp[16384];
        const ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        if (n == 0) return false;
        buf.insert(buf.end(), tmp, tmp + n);
        std::size_t consumed = 0;
        while (buf.size() - consumed >= 4) {
            std::uint32_t len_be = 0;
            std::memcpy(&len_be, buf.data() + consumed, 4);
            const std::uint32_t len = ntohl(len_be);
            if (len == 0 || len > kMaxFrameBytes) return false;
            if (buf.size() - consumed < 4 + len) break;
            frames.emplace_back(buf.data() + consumed + 4, len);
            consumed += 4 + len;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
        return true;
    }
};

void SendUdpBroadcasts(int udp, std::uint16_t port, std::string_view payload) noexcept {
    if (udp < 0 || !port) return;
    auto send_to = [&](std::uint32_t addr_be) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        dst.sin_addr.s_addr = addr_be;
        [[maybe_unused]] const auto r = sendto(udp, payload.data(), payload.size(), 0,
            reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    };
    send_to(htonl(INADDR_BROADCAST));
    ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) == 0 && ifas) {
        for (const auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
            if (ifa->ifa_broadaddr && ifa->ifa_broadaddr->sa_family == AF_INET) {
                const auto bcast = reinterpret_cast<const sockaddr_in*>(ifa->ifa_broadaddr)->sin_addr.s_addr;
                if (bcast && bcast != htonl(INADDR_BROADCAST)) send_to(bcast);
            }
        }
        freeifaddrs(ifas);
    }
}

std::string FormatPeerIp(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    return inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) ? std::string(buf) : std::string("unknown");
}

} // namespace

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string ExtractJsonString(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = 0;
    while (pos < json.size()) {
        // Walk respecting string literals so we don't match keys inside escaped string values.
        const auto quote = json.find('"', pos);
        if (quote == std::string_view::npos) return {};
        // Parse the quoted string starting at `quote`
        std::string parsed;
        std::size_t i = quote + 1;
        bool escaped = false;
        bool closed = false;
        while (i < json.size()) {
            const char ch = json[i++];
            if (escaped) {
                switch (ch) {
                case '"': parsed.push_back('"'); break;
                case '\\': parsed.push_back('\\'); break;
                case '/': parsed.push_back('/'); break;
                case 'b': parsed.push_back('\b'); break;
                case 'f': parsed.push_back('\f'); break;
                case 'n': parsed.push_back('\n'); break;
                case 'r': parsed.push_back('\r'); break;
                case 't': parsed.push_back('\t'); break;
                case 'u':
                    if (i + 4 <= json.size()) {
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = json[i + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        }
                        i += 4;
                        if (code < 0x80) parsed.push_back(static_cast<char>(code));
                    }
                    break;
                default: parsed.push_back(ch); break;
                }
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                closed = true;
                break;
            } else {
                parsed.push_back(ch);
            }
        }
        if (!closed) return {};
        pos = i;
        // Skip whitespace; if followed by ':', this was a key!
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
                ++pos;
            }
            if (parsed == key) {
                if (pos >= json.size() || json[pos] != '"') return {};
                std::string val;
                std::size_t j = pos + 1;
                bool vesc = false;
                while (j < json.size()) {
                    const char ch = json[j++];
                    if (vesc) {
                        switch (ch) {
                        case '"': val.push_back('"'); break;
                        case '\\': val.push_back('\\'); break;
                        case '/': val.push_back('/'); break;
                        case 'b': val.push_back('\b'); break;
                        case 'f': val.push_back('\f'); break;
                        case 'n': val.push_back('\n'); break;
                        case 'r': val.push_back('\r'); break;
                        case 't': val.push_back('\t'); break;
                        case 'u':
                            if (j + 4 <= json.size()) {
                                unsigned code = 0;
                                for (int k = 0; k < 4; ++k) {
                                    const char h = json[j + k];
                                    code <<= 4;
                                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                                }
                                j += 4;
                                if (code < 0x80) val.push_back(static_cast<char>(code));
                            }
                            break;
                        default: val.push_back(ch); break;
                        }
                        vesc = false;
                    } else if (ch == '\\') {
                        vesc = true;
                    } else if (ch == '"') {
                        return val;
                    } else {
                        val.push_back(ch);
                    }
                }
                return {};
            }
        }
    }
    (void)needle;
    return {};
}

std::string RelayMiniRtcJson(std::string_view payload, bool from_host) {
    if (payload.size() > 64 * 1024) throw std::runtime_error("MiniRTC signaling payload too large");
    const auto kind = ExtractJsonString(payload, "type");
    std::string out = "{\"transmission_id\":\"host\"";
    if (!from_host && (kind == "join_transmission" || kind == "user_join_transmission")) {
        out += ",\"type\":\"user_join_transmission\",\"status\":\"success\",\"user_id\":\"viewer\"}";
        return out;
    }
    if ((from_host && kind == "offer") || (!from_host && kind == "answer") ||
        kind == "new_candidate" || kind == "new_candidate_mid") {
        out += ",\"type\":\"" + JsonEscape(kind) + "\"";
        out += std::string(",\"remote_user_id\":\"") + (from_host ? "host" : "viewer") + "\"";
        if (kind == "new_candidate_mid") {
            out += ",\"mid\":\"" + JsonEscape(ExtractJsonString(payload, "mid")) + "\"";
            out += ",\"candidate\":\"" + JsonEscape(ExtractJsonString(payload, "candidate")) + "\"";
        } else {
            out += ",\"sdp\":\"" + JsonEscape(ExtractJsonString(payload, "sdp")) + "\"";
        }
        if (kind == "new_candidate" || kind == "new_candidate_mid") {
            const auto ufrag = ExtractJsonString(payload, "ufrag");
            if (!ufrag.empty()) {
                out += ",\"ufrag\":\"" + JsonEscape(ufrag) + "\"";
            }
        }
        out += "}";
        return out;
    }
    throw std::runtime_error("Unexpected MiniRTC signaling type: " + kind);
}

LanSignalServer::LanSignalServer(std::string display_name, std::uint16_t port, std::string access_code,
    Callbacks callbacks, std::uint16_t discovery_port)
    : callbacks_(std::move(callbacks)), display_name_(std::move(display_name)),
      port_(port ? port : kDefaultLanPort), access_code_(std::move(access_code)), discovery_port_(discovery_port) {}

LanSignalServer::~LanSignalServer() {
    Stop();
}

bool LanSignalServer::Start() {
    FdGuard listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP));
    const int yes = 1;
    if (listener) setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!listener || bind(listener.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener.Get(), 16) != 0) {
        std::cerr << "[lan] failed to bind/listen TCP port " << port_ << ": " << std::strerror(errno) << '\n';
        return false;
    }
    worker_ = std::jthread([this, listener = std::move(listener)](std::stop_token stop) {
        Run(stop, listener.Get());
    });
    return true;
}

void LanSignalServer::Stop() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

void LanSignalServer::SetMediaConnected(bool connected) noexcept {
    media_connected_.store(connected, std::memory_order_release);
}

void LanSignalServer::Send(std::string kind, std::string payload) {
    if (kind == "minirtc") payload = RelayMiniRtcJson(payload, true);
    const std::lock_guard lock(mutex_);
    if (outbound_.size() < 128) {
        outbound_.emplace_back(std::move(kind), std::move(payload));
    }
}

void LanSignalServer::EndActiveClient(std::string reason) {
    const std::lock_guard lock(mutex_);
    if (end_client_.load(std::memory_order_relaxed)) return;
    outbound_.emplace_front("ended", std::move(reason));
    end_client_.store(true, std::memory_order_release);
}

void LanSignalServer::Run(std::stop_token stop, int listen_fd) {
    const auto& display_name = display_name_;
    const auto port = port_;
    const auto discovery_port = discovery_port_;
    const auto key = Sha256("p2premote-lan-v1:" + access_code_);
    const auto instance_id = RandomHex(8);

    int yes = 1;

    FdGuard udp_fd;
    if (discovery_port != 0) {
        FdGuard candidate(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP));
        if (candidate) {
            setsockopt(candidate.Get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            setsockopt(candidate.Get(), SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            SetNonBlocking(candidate.Get());
            sockaddr_in uaddr{};
            uaddr.sin_family = AF_INET;
            uaddr.sin_port = htons(discovery_port);
            uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(candidate.Get(), reinterpret_cast<const sockaddr*>(&uaddr), sizeof(uaddr)) == 0) {
                udp_fd = std::move(candidate);
            }
        }
    }

    auto make_beacon = [&](bool busy) {
        return "{\"p2p_lan_host\":1,\"id\":\"" + JsonEscape(instance_id) +
            "\",\"name\":\"" + JsonEscape(display_name) +
            "\",\"port\":" + std::to_string(port) +
            ",\"busy\":" + (busy ? "true" : "false") + "}";
    };

    std::cout << "[lan] listening on TCP :" << port
              << " (discovery UDP :" << discovery_port << ", name=" << display_name << ")\n" << std::flush;

    struct FailureState {
        unsigned count{};
        std::uint64_t expires{};
    };
    struct PendingClient {
        FdGuard socket;
        std::string ip;
        std::string nonce;
        std::uint64_t deadline;
        FrameReader reader;
    };
    std::map<std::string, FailureState> failures;
    std::vector<PendingClient> pending;
    FdGuard client_fd;
    FrameReader reader;
    std::uint64_t media_deadline = 0;
    std::uint64_t next_beacon = 0;

    auto close_client = [&](std::string reason) {
        client_fd.Reset();
        reader.Clear();
        if (callbacks_.on_ended) callbacks_.on_ended(reason);
        media_connected_.store(false, std::memory_order_release);
        end_client_.store(false, std::memory_order_release);
        const std::lock_guard lock(mutex_);
        outbound_.clear();
    };

    while (!stop.stop_requested()) {
        const auto now = NowMs();
        const bool media_connected = media_connected_.load(std::memory_order_acquire);
        if (client_fd && !media_connected && now >= media_deadline &&
            !end_client_.load(std::memory_order_acquire)) {
            EndActiveClient("MiniRTC connection timed out");
        }
        if (now >= next_beacon) {
            next_beacon = now + kBeaconIntervalMs;
            std::erase_if(failures, [now](const auto& item) { return item.second.expires <= now; });
            if (udp_fd) SendUdpBroadcasts(udp_fd.Get(), discovery_port, make_beacon(static_cast<bool>(client_fd)));
        }

        if (client_fd) {
            std::deque<std::pair<std::string, std::string>> batch;
            {
                const std::lock_guard lock(mutex_);
                batch.swap(outbound_);
            }
            for (const auto& [kind, payload] : batch) {
                const bool sent = SendTyped(client_fd.Get(), kind, payload);
                if (!sent || kind == "ended") {
                    close_client(sent ? payload : "Viewer connection lost");
                    break;
                }
            }
        }

        pollfd pfds[3]{};
        nfds_t nfds = 0;
        pfds[nfds++] = {listen_fd, POLLIN, 0};
        const int udp_idx = udp_fd ? static_cast<int>(nfds) : -1;
        if (udp_fd) pfds[nfds++] = {udp_fd.Get(), POLLIN, 0};
        const int client_idx = client_fd ? static_cast<int>(nfds) : -1;
        if (client_fd) pfds[nfds++] = {client_fd.Get(), POLLIN, 0};
        const int ready = poll(pfds, nfds, 25);
        if (ready < 0 && errno != EINTR) break;

        if (udp_idx >= 0 && (pfds[udp_idx].revents & POLLIN)) {
            for (unsigned i = 0; i < 32; ++i) {
                char buf[512]{};
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const ssize_t n = recvfrom(udp_fd.Get(), buf, sizeof(buf), 0,
                    reinterpret_cast<sockaddr*>(&from), &from_len);
                if (n <= 0) break;
                const std::string_view view(buf, static_cast<std::size_t>(n));
                if (view.find("p2p_lan_discover") != std::string_view::npos) {
                    const auto reply = make_beacon(static_cast<bool>(client_fd));
                    sendto(udp_fd.Get(), reply.data(), reply.size(), 0,
                        reinterpret_cast<const sockaddr*>(&from), from_len);
                }
            }
        }

        if (client_idx >= 0 && (pfds[client_idx].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            std::vector<std::string> frames;
            if (!reader.Pump(client_fd.Get(), frames)) {
                close_client("Viewer disconnected");
            }
            for (const auto& frame : frames) {
                const auto type = ExtractJsonString(frame, "type");
                const auto payload = ExtractJsonString(frame, "payload");
                if (type == "ended") {
                    close_client(payload.empty() ? "Viewer ended session" : payload);
                    break;
                }
                if (type == "minirtc") {
                    try {
                        const auto relayed = RelayMiniRtcJson(payload, false);
                        if (callbacks_.on_minirtc) callbacks_.on_minirtc(relayed);
                    } catch (const std::exception& err) {
                        SendTyped(client_fd.Get(), "error", "局域网媒体信令格式无效");
                        close_client(err.what());
                        break;
                    }
                }
            }
        }

        if (pfds[0].revents & POLLIN) {
            sockaddr_in peer_addr{};
            socklen_t peer_len = sizeof(peer_addr);
            FdGuard incoming(accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len,
                SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (incoming) {
                const int nodelay = 1;
                setsockopt(incoming.Get(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                setsockopt(incoming.Get(), SOL_SOCKET, SO_KEEPALIVE, &nodelay, sizeof(nodelay));
                const int keepidle = 30;
                const int keepintvl = 5;
                const int keepcnt = 3;
                setsockopt(incoming.Get(), IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
                setsockopt(incoming.Get(), IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
                setsockopt(incoming.Get(), IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
                auto ip = FormatPeerIp(peer_addr);
                const auto failure = failures.find(ip);
                if (failure != failures.end() && failure->second.count >= kMaxAuthFailures &&
                    failure->second.expires > now) {
                    SendTyped(incoming.Get(), "error", "验证码错误次数过多，请稍后再试", 0);
                } else if (client_fd || pending.size() >= kMaxPendingClients) {
                    SendTyped(incoming.Get(), "error", "设备正在处理其他主控连接", 0);
                } else {
                    auto nonce = RandomHex(32);
                    const std::string challenge = "{\"type\":\"challenge\",\"nonce\":\"" + nonce +
                        "\",\"host_name\":\"" + JsonEscape(display_name) + "\",\"version\":1}";
                    if (SendFramedJson(incoming.Get(), challenge, 0)) {
                        pending.push_back({std::move(incoming), std::move(ip), std::move(nonce),
                            NowMs() + kAuthTimeoutMs, {}});
                    }
                }
            }
        }

        std::erase_if(pending, [&](PendingClient& candidate) {
            if (NowMs() >= candidate.deadline) return true;
            if (client_fd) {
                SendTyped(candidate.socket.Get(), "error", "设备正在被其他主控连接", 0);
                return true;
            }
            std::vector<std::string> frames;
            if (!candidate.reader.Pump(candidate.socket.Get(), frames)) return true;
            if (frames.empty()) return false;
            const auto& auth = frames.front();
            const auto viewer_nonce = ExtractJsonString(auth, "nonce");
            const auto viewer_hmac = ExtractJsonString(auth, "hmac");
            if (ExtractJsonString(auth, "type") != "auth" || viewer_nonce.empty() || viewer_nonce.size() > 128 ||
                !ConstantTimeEqual(viewer_hmac, HmacSha256Hex(key, candidate.nonce + ":" + viewer_nonce + ":viewer"))) {
                if (failures.size() < 4096 || failures.contains(candidate.ip)) {
                    auto& fail = failures[candidate.ip];
                    const auto failed_at = NowMs();
                    if (fail.expires <= failed_at) fail.count = 0;
                    if (fail.count < kMaxAuthFailures) ++fail.count;
                    fail.expires = failed_at + kLockoutMs;
                }
                SendTyped(candidate.socket.Get(), "error", "连接验证码错误", 0);
                return true;
            }

            const auto host_mac = HmacSha256Hex(key, candidate.nonce + ":" + viewer_nonce + ":host");
            const std::string accept = "{\"type\":\"accepted\",\"payload\":\"control\",\"hmac\":\"" + host_mac + "\"}";
            if (SendFramedJson(candidate.socket.Get(), accept, 0)) {
                failures.erase(candidate.ip);
                client_fd = std::move(candidate.socket);
                media_connected_.store(false, std::memory_order_release);
                media_deadline = NowMs() + kMediaConnectTimeoutMs;
                if (callbacks_.on_client_authed) callbacks_.on_client_authed(candidate.ip);
            }
            return true;
        });
    }

    if (client_fd) {
        SendTyped(client_fd.Get(), "ended", "Host shutting down", 0);
        close_client("Host shutting down");
    }
}

} // namespace p2premote::linux_host
