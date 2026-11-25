#include "direct_signal.h"

#include "stream_events.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>

namespace p2premote {
namespace {

using namespace winrt::Windows::Data::Json;

constexpr std::size_t kMaxFrameBytes = 256 * 1024;
constexpr unsigned kMaxAuthFailures = 5;
constexpr ULONGLONG kLockoutDurationMs = 60'000;
constexpr ULONGLONG kPeerExpiryMs = 7'000;
constexpr ULONGLONG kBeaconIntervalMs = 2'500;

struct WinsockGuard {
    WinsockGuard() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("无法初始化 Winsock 网络子系统");
        }
    }
    ~WinsockGuard() { WSACleanup(); }
};

void EnsureWinsock() {
    static WinsockGuard guard;
}

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET s) noexcept : socket_(s) {}
    ~SocketHandle() { Reset(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        }
        return *this;
    }

    SOCKET Get() const noexcept { return socket_; }
    explicit operator bool() const noexcept { return socket_ != INVALID_SOCKET; }
    void Reset(SOCKET next = INVALID_SOCKET) noexcept {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = next;
    }

private:
    SOCKET socket_{INVALID_SOCKET};
};

std::string HexEncode(const std::uint8_t* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return out;
}

std::string RandomHex(std::size_t bytes) {
    std::vector<std::uint8_t> buf(bytes);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("无法生成安全随机数");
    }
    return HexEncode(buf.data(), buf.size());
}

std::array<std::uint8_t, 32> Sha256Bytes(std::string_view input) {
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        throw std::runtime_error("无法初始化 SHA-256");
    }
    struct Guard {
        BCRYPT_HASH_HANDLE h;
        ~Guard() { if (h) BCryptDestroyHash(h); }
    } guard{hash};
    std::array<std::uint8_t, 32> digest{};
    if ((!input.empty() && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
            static_cast<ULONG>(input.size()), 0) < 0) ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        throw std::runtime_error("SHA-256 计算失败");
    }
    return digest;
}

std::string HmacSha256Hex(const std::array<std::uint8_t, 32>& key, std::string_view message) {
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, &hash, nullptr, 0,
            const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) < 0) {
        throw std::runtime_error("无法初始化 HMAC-SHA256");
    }
    struct Guard {
        BCRYPT_HASH_HANDLE h;
        ~Guard() { if (h) BCryptDestroyHash(h); }
    } guard{hash};
    std::array<std::uint8_t, 32> digest{};
    if ((!message.empty() && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
            static_cast<ULONG>(message.size()), 0) < 0) ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        throw std::runtime_error("HMAC-SHA256 计算失败");
    }
    return HexEncode(digest.data(), digest.size());
}

std::array<std::uint8_t, 32> DeriveAccessKey(const std::wstring& access_code) {
    const auto utf8 = winrt::to_string(access_code);
    return Sha256Bytes("p2premote-lan-v1:" + utf8);
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

std::wstring ComputerDisplayName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = static_cast<DWORD>(std::size(name));
    if (GetComputerNameW(name, &length) && length > 0) return std::wstring(name, length);
    return L"Windows PC";
}

bool SetNonBlocking(SOCKET s, bool enabled) noexcept {
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool SendAll(SOCKET s, const char* data, std::size_t size, ULONGLONG timeout_ms = 3000) noexcept {
    const auto deadline = GetTickCount64() + timeout_ms;
    std::size_t sent_total = 0;
    while (sent_total < size) {
        const int sent = send(s, data + sent_total, static_cast<int>(size - sent_total), 0);
        if (sent > 0) {
            sent_total += static_cast<std::size_t>(sent);
            continue;
        }
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) return false;
        const auto now = GetTickCount64();
        if (now >= deadline) return false;
        WSAPOLLFD fd{s, POLLOUT, 0};
        const int wait = static_cast<int>(std::min<ULONGLONG>(deadline - now, 50));
        if (WSAPoll(&fd, 1, wait) < 0) return false;
    }
    return true;
}

bool SendJsonFrame(SOCKET s, const JsonObject& obj) {
    const auto utf8 = winrt::to_string(obj.Stringify());
    if (utf8.size() > kMaxFrameBytes) return false;
    std::uint32_t len_be = htonl(static_cast<std::uint32_t>(utf8.size()));
    std::string packet;
    packet.resize(4 + utf8.size());
    std::memcpy(packet.data(), &len_be, 4);
    std::memcpy(packet.data() + 4, utf8.data(), utf8.size());
    return SendAll(s, packet.data(), packet.size());
}

bool SendTypedFrame(SOCKET s, std::string_view type, std::string_view payload = {}) {
    JsonObject obj;
    obj.SetNamedValue(L"type", JsonValue::CreateStringValue(winrt::to_hstring(type)));
    obj.SetNamedValue(L"payload", JsonValue::CreateStringValue(winrt::to_hstring(payload)));
    return SendJsonFrame(s, obj);
}

// MiniRTC emits signaling requests addressed to a peer. The receiving peer
// expects the corresponding notification from the sender.
std::string RelayMiniRtc(std::string_view payload, bool from_host) {
    if (payload.size() > 64 * 1024) throw std::runtime_error("局域网媒体信令过长");
    const auto request = JsonObject::Parse(winrt::to_hstring(payload));
    const auto kind = request.GetNamedString(L"type", L"");
    JsonObject notice;
    notice.SetNamedValue(L"transmission_id", JsonValue::CreateStringValue(L"host"));
    if (!from_host && (kind == L"join_transmission" || kind == L"user_join_transmission")) {
        notice.SetNamedValue(L"type", JsonValue::CreateStringValue(L"user_join_transmission"));
        notice.SetNamedValue(L"status", JsonValue::CreateStringValue(L"success"));
        notice.SetNamedValue(L"user_id", JsonValue::CreateStringValue(L"viewer"));
    } else if ((from_host && kind == L"offer") || (!from_host && kind == L"answer") ||
            kind == L"new_candidate" || kind == L"new_candidate_mid") {
        notice.SetNamedValue(L"type", JsonValue::CreateStringValue(kind));
        notice.SetNamedValue(L"remote_user_id", JsonValue::CreateStringValue(from_host ? L"host" : L"viewer"));
        const auto copy_string = [&](const wchar_t* name) {
            notice.SetNamedValue(name, JsonValue::CreateStringValue(request.GetNamedString(name)));
        };
        if (kind == L"new_candidate_mid") {
            copy_string(L"mid");
            copy_string(L"candidate");
        } else {
            copy_string(L"sdp");
        }
        if ((kind == L"new_candidate" || kind == L"new_candidate_mid") && request.HasKey(L"ufrag")) {
            copy_string(L"ufrag");
        }
    } else {
        throw std::runtime_error("收到与局域网会话不符的媒体信令");
    }
    return winrt::to_string(notice.Stringify());
}

enum class RecvStatus { ok, timeout, closed, error };

RecvStatus RecvExact(SOCKET s, char* out, std::size_t size, std::stop_token stop, ULONGLONG deadline) noexcept {
    std::size_t received = 0;
    while (received < size) {
        if (stop.stop_requested()) return RecvStatus::closed;
        const int n = recv(s, out + received, static_cast<int>(size - received), 0);
        if (n > 0) {
            received += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) return RecvStatus::closed;
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) return RecvStatus::error;
        const auto now = GetTickCount64();
        if (now >= deadline) return RecvStatus::timeout;
        WSAPOLLFD fd{s, POLLIN, 0};
        const int wait = static_cast<int>(std::min<ULONGLONG>(deadline - now, 25));
        const int ready = WSAPoll(&fd, 1, wait);
        if (ready < 0) return RecvStatus::error;
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) return RecvStatus::closed;
    }
    return RecvStatus::ok;
}

RecvStatus RecvJsonFrame(SOCKET s, JsonObject& out, std::stop_token stop, ULONGLONG timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    std::uint32_t len_be{};
    const auto header_status = RecvExact(s, reinterpret_cast<char*>(&len_be), 4, stop, deadline);
    if (header_status != RecvStatus::ok) return header_status;
    const std::uint32_t length = ntohl(len_be);
    if (length == 0 || length > kMaxFrameBytes) return RecvStatus::error;
    std::string body(length, '\0');
    const auto body_status = RecvExact(s, body.data(), body.size(), stop, deadline);
    if (body_status != RecvStatus::ok) return body_status;
    try {
        out = JsonObject::Parse(winrt::to_hstring(body));
        return RecvStatus::ok;
    } catch (...) {
        return RecvStatus::error;
    }
}

struct FrameReader {
    std::vector<char> buf;

    void Clear() noexcept { buf.clear(); }

    // Non-blocking pump. Returns false on disconnect or protocol error.
    bool Pump(SOCKET s, std::vector<JsonObject>& out) {
        char temp[16384];
        for (;;) {
            const int n = recv(s, temp, sizeof(temp), 0);
            if (n > 0) {
                if (buf.size() + static_cast<std::size_t>(n) > kMaxFrameBytes + 4) return false;
                buf.insert(buf.end(), temp, temp + n);
            } else if (n == 0) {
                return false;
            } else {
                const int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) break;
                return false;
            }
        }
        while (buf.size() >= 4) {
            std::uint32_t len_be{};
            std::memcpy(&len_be, buf.data(), 4);
            const std::uint32_t length = ntohl(len_be);
            if (length == 0 || length > kMaxFrameBytes) return false;
            if (buf.size() < 4 + length) break;
            std::string body(buf.data() + 4, length);
            buf.erase(buf.begin(), buf.begin() + 4 + static_cast<std::ptrdiff_t>(length));
            try {
                out.push_back(JsonObject::Parse(winrt::to_hstring(body)));
            } catch (...) {
                return false;
            }
        }
        return true;
    }
};

struct LocalIpv4Info {
    std::wstring address;
    std::uint32_t ip_be{};
    std::uint32_t broadcast_be{};
};

std::vector<LocalIpv4Info> EnumerateLocalIpv4() {
    std::vector<LocalIpv4Info> result;
    ULONG size = 16 * 1024;
    std::vector<BYTE> buffer(size);
    ULONG err = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (err == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        err = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (err != NO_ERROR) return result;
    for (const auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (const auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const std::uint32_t ip_host = ntohl(sin->sin_addr.s_addr);
            const std::uint8_t first = static_cast<std::uint8_t>((ip_host >> 24) & 0xff);
            const std::uint8_t second = static_cast<std::uint8_t>((ip_host >> 16) & 0xff);
            if (first == 127 || (first == 169 && second == 254)) continue;
            wchar_t text[INET_ADDRSTRLEN]{};
            if (!InetNtopW(AF_INET, &sin->sin_addr, text, static_cast<std::size_t>(std::size(text)))) continue;
            const std::uint32_t mask_host = unicast->OnLinkPrefixLength == 0 ? 0u
                : (unicast->OnLinkPrefixLength >= 32 ? 0xffffffffu : (~0u << (32 - unicast->OnLinkPrefixLength)));
            const std::uint32_t bcast_host = ip_host | ~mask_host;
            result.push_back({text, sin->sin_addr.s_addr, htonl(bcast_host)});
        }
    }
    return result;
}

void SendUdpBroadcasts(SOCKET udp, std::uint16_t port, std::string_view payload, bool include_loopback) noexcept {
    if (udp == INVALID_SOCKET || !port) return;
    auto send_to_addr = [&](std::uint32_t addr_be) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        dst.sin_addr.s_addr = addr_be;
        sendto(udp, payload.data(), static_cast<int>(payload.size()), 0,
            reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    };
    send_to_addr(INADDR_BROADCAST);
    for (const auto& info : EnumerateLocalIpv4()) {
        if (info.broadcast_be && info.broadcast_be != INADDR_BROADCAST) {
            send_to_addr(info.broadcast_be);
        }
    }
    if (include_loopback) send_to_addr(htonl(INADDR_LOOPBACK));
}

std::string FormatPeerIp(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &addr.sin_addr, buf, sizeof(buf)) ? std::string(buf) : std::string("unknown");
}

std::wstring FormatPeerIpWide(const sockaddr_in& addr) {
    wchar_t buf[INET_ADDRSTRLEN]{};
    return InetNtopW(AF_INET, &addr.sin_addr, buf, std::size(buf)) ? std::wstring(buf) : std::wstring{};
}

} // namespace

DirectSignalServer::DirectSignalServer(std::shared_ptr<StreamEvents> events, std::wstring bind_address,
    std::uint16_t port, std::wstring access_code, std::uint16_t discovery_port)
    : events_(std::move(events)),
      worker_([this, bind_address = std::move(bind_address), port, access_code = std::move(access_code),
               discovery_port](std::stop_token stop) {
          try {
              winrt::init_apartment();
              try {
                  Run(stop, bind_address, port, access_code, discovery_port);
              } catch (...) {
                  winrt::uninit_apartment();
                  throw;
              }
              winrt::uninit_apartment();
          } catch (const std::exception& error) {
              if (!stop.stop_requested()) {
                  events_->Push({StreamEventType::error, {}, error.what()});
              }
          } catch (...) {
              if (!stop.stop_requested()) {
                  events_->Push({StreamEventType::error, {}, "局域网直连监听异常终止"});
              }
          }
      }) {}

DirectSignalServer::~DirectSignalServer() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

void DirectSignalServer::Send(std::string kind, std::string payload) {
    if (kind == "minirtc") payload = RelayMiniRtc(payload, true);
    const std::lock_guard lock(mutex_);
    if (pending_.size() >= 128) throw std::runtime_error("局域网信令发送队列已满");
    pending_.emplace_back(std::move(kind), std::move(payload));
}

void DirectSignalServer::Run(std::stop_token stop, std::wstring bind_address, std::uint16_t port,
    std::wstring access_code, std::uint16_t discovery_port) {
    EnsureWinsock();
    if (!port) port = kLanDefaultPort;
    const auto key = DeriveAccessKey(access_code);
    const auto instance_id = RandomHex(8);
    const auto host_name = ComputerDisplayName();

    SocketHandle listen_sock(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listen_sock) throw std::runtime_error("无法创建局域网监听套接字");
    BOOL reuse = TRUE;
    setsockopt(listen_sock.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    SetNonBlocking(listen_sock.Get(), true);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_address.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (InetPtonW(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("无效的局域网监听地址");
    }
    if (bind(listen_sock.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock.Get(), 8) != 0) {
        throw std::runtime_error("无法监听局域网端口 " + std::to_string(port) + "（端口可能已被占用）");
    }

    SocketHandle udp_sock;
    if (discovery_port != 0) {
        SocketHandle candidate(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (candidate) {
            BOOL yes = TRUE;
            setsockopt(candidate.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
            setsockopt(candidate.Get(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
            SetNonBlocking(candidate.Get(), true);
            sockaddr_in udp_addr{};
            udp_addr.sin_family = AF_INET;
            udp_addr.sin_port = htons(discovery_port);
            udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(candidate.Get(), reinterpret_cast<const sockaddr*>(&udp_addr), sizeof(udp_addr)) == 0) {
                udp_sock = std::move(candidate);
            }
        }
    }

    auto make_beacon = [&](bool busy) {
        JsonObject beacon;
        beacon.SetNamedValue(L"p2p_lan_host", JsonValue::CreateNumberValue(1));
        beacon.SetNamedValue(L"id", JsonValue::CreateStringValue(winrt::to_hstring(instance_id)));
        beacon.SetNamedValue(L"name", JsonValue::CreateStringValue(host_name));
        beacon.SetNamedValue(L"port", JsonValue::CreateNumberValue(port));
        beacon.SetNamedValue(L"busy", JsonValue::CreateBooleanValue(busy));
        return winrt::to_string(beacon.Stringify());
    };

    events_->Push({StreamEventType::signal, "ready", {}});

    struct FailureState {
        unsigned count{};
        ULONGLONG lockout_until{};
    };
    std::map<std::string, FailureState> failures;

    SocketHandle client_sock;
    FrameReader client_reader;
    std::string active_host_mac;
    ULONGLONG next_beacon = 0;

    while (!stop.stop_requested()) {
        const auto now = GetTickCount64();
        if (udp_sock && now >= next_beacon) {
            next_beacon = now + kBeaconIntervalMs;
            const auto payload = make_beacon(static_cast<bool>(client_sock));
            SendUdpBroadcasts(udp_sock.Get(), discovery_port, payload, false);
        }

        if (client_sock) {
            std::deque<std::pair<std::string, std::string>> outbound;
            {
                const std::lock_guard lock(mutex_);
                outbound.swap(pending_);
            }
            for (const auto& [kind, payload] : outbound) {
                bool ok = false;
                if (kind == "accept") {
                    JsonObject obj;
                    obj.SetNamedValue(L"type", JsonValue::CreateStringValue(L"accepted"));
                    obj.SetNamedValue(L"payload", JsonValue::CreateStringValue(winrt::to_hstring(payload)));
                    obj.SetNamedValue(L"hmac", JsonValue::CreateStringValue(winrt::to_hstring(active_host_mac)));
                    ok = SendJsonFrame(client_sock.Get(), obj);
                } else if (kind == "reject") {
                    SendTypedFrame(client_sock.Get(), "error", "被控端已拒绝连接");
                    client_sock.Reset();
                    client_reader.Clear();
                    active_host_mac.clear();
                    break;
                } else {
                    ok = SendTypedFrame(client_sock.Get(), kind, payload);
                }
                if (!ok) {
                    client_sock.Reset();
                    client_reader.Clear();
                    active_host_mac.clear();
                    events_->Push({StreamEventType::signal, "ended", "Viewer 连接已断开"});
                    break;
                }
            }
        }

        WSAPOLLFD fds[3]{};
        ULONG nfds = 0;
        fds[nfds++] = {listen_sock.Get(), POLLIN, 0};
        const int udp_idx = udp_sock ? static_cast<int>(nfds) : -1;
        if (udp_sock) fds[nfds++] = {udp_sock.Get(), POLLIN, 0};
        const int client_idx = client_sock ? static_cast<int>(nfds) : -1;
        if (client_sock) fds[nfds++] = {client_sock.Get(), POLLIN, 0};

        const int ready = WSAPoll(fds, nfds, 25);
        if (ready <= 0) continue;

        if (udp_idx >= 0 && (fds[udp_idx].revents & POLLIN)) {
            char buf[512]{};
            sockaddr_in from{};
            int from_len = sizeof(from);
            const int n = recvfrom(udp_sock.Get(), buf, sizeof(buf) - 1, 0,
                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0) {
                try {
                    const auto json = JsonObject::Parse(winrt::to_hstring(std::string_view(buf, static_cast<std::size_t>(n))));
                    if (json.HasKey(L"p2p_lan_discover")) {
                        const auto reply = make_beacon(static_cast<bool>(client_sock));
                        sendto(udp_sock.Get(), reply.data(), static_cast<int>(reply.size()), 0,
                            reinterpret_cast<const sockaddr*>(&from), from_len);
                    }
                } catch (...) {}
            }
        }

        if (fds[0].revents & POLLIN) {
            sockaddr_in peer_addr{};
            int peer_len = sizeof(peer_addr);
            SocketHandle incoming(accept(listen_sock.Get(), reinterpret_cast<sockaddr*>(&peer_addr), &peer_len));
            if (incoming) {
                BOOL nodelay = TRUE;
                setsockopt(incoming.Get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                SetNonBlocking(incoming.Get(), true);
                const auto peer_ip = FormatPeerIp(peer_addr);
                auto& fail_state = failures[peer_ip];
                if (fail_state.lockout_until > now) {
                    SendTypedFrame(incoming.Get(), "error", "被控码错误次数过多，请稍后再试");
                } else if (client_sock) {
                    SendTypedFrame(incoming.Get(), "error", "设备正在被其他主控连接");
                } else {
                    const auto host_nonce = RandomHex(32);
                    JsonObject challenge;
                    challenge.SetNamedValue(L"type", JsonValue::CreateStringValue(L"challenge"));
                    challenge.SetNamedValue(L"nonce", JsonValue::CreateStringValue(winrt::to_hstring(host_nonce)));
                    challenge.SetNamedValue(L"host_name", JsonValue::CreateStringValue(host_name));
                    challenge.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
                    JsonObject auth_msg{nullptr};
                    if (SendJsonFrame(incoming.Get(), challenge) &&
                        RecvJsonFrame(incoming.Get(), auth_msg, stop, 3000) == RecvStatus::ok &&
                        winrt::to_string(auth_msg.GetNamedString(L"type", L"")) == "auth") {
                        const auto viewer_nonce = winrt::to_string(auth_msg.GetNamedString(L"nonce", L""));
                        const auto viewer_hmac = winrt::to_string(auth_msg.GetNamedString(L"hmac", L""));
                        const auto expected = HmacSha256Hex(key, host_nonce + ":" + viewer_nonce + ":viewer");
                        if (!viewer_nonce.empty() && ConstantTimeEquals(viewer_hmac, expected)) {
                            fail_state = {};
                            active_host_mac = HmacSha256Hex(key, host_nonce + ":" + viewer_nonce + ":host");
                            client_sock = std::move(incoming);
                            client_reader.Clear();
                            {
                                const std::lock_guard lock(mutex_);
                                pending_.clear();
                            }
                            events_->Push({StreamEventType::signal, "request", peer_ip});
                        } else {
                            if (++fail_state.count >= kMaxAuthFailures) {
                                fail_state.count = 0;
                                fail_state.lockout_until = now + kLockoutDurationMs;
                            }
                            SendTypedFrame(incoming.Get(), "error", "被控码错误");
                        }
                    }
                }
            }
        }

        if (client_idx >= 0 && client_sock && (fds[client_idx].revents & (POLLIN | POLLERR | POLLHUP))) {
            std::vector<JsonObject> frames;
            if (!client_reader.Pump(client_sock.Get(), frames)) {
                client_sock.Reset();
                client_reader.Clear();
                active_host_mac.clear();
                events_->Push({StreamEventType::signal, "ended", "Viewer 已结束会话"});
                continue;
            }
            for (const auto& json : frames) {
                const auto type = winrt::to_string(json.GetNamedString(L"type", L""));
                const auto payload = winrt::to_string(json.GetNamedString(L"payload", L""));
                if (type == "ended") {
                    client_sock.Reset();
                    client_reader.Clear();
                    active_host_mac.clear();
                    events_->Push({StreamEventType::signal, "ended", payload.empty() ? "Viewer 已结束会话" : payload});
                    break;
                }
                if (type == "minirtc") {
                    try {
                        events_->Push({StreamEventType::signal, type, RelayMiniRtc(payload, false)});
                    } catch (...) {
                        SendTypedFrame(client_sock.Get(), "error", "局域网媒体信令格式无效");
                        client_sock.Reset();
                        client_reader.Clear();
                        active_host_mac.clear();
                        events_->Push({StreamEventType::signal, "ended", "局域网媒体信令格式无效"});
                        break;
                    }
                }
            }
        }
    }

    if (client_sock && end_session_.load()) {
        SendTypedFrame(client_sock.Get(), "ended", "Host 已结束会话");
    }
}

DirectSignalClient::DirectSignalClient(std::shared_ptr<StreamEvents> events, std::wstring host_address,
    std::uint16_t port, std::wstring access_code)
    : events_(std::move(events)),
      worker_([this, host_address = std::move(host_address), port, access_code = std::move(access_code)](
                  std::stop_token stop) {
          try {
              winrt::init_apartment();
              try {
                  Run(stop, host_address, port, access_code);
              } catch (...) {
                  winrt::uninit_apartment();
                  throw;
              }
              winrt::uninit_apartment();
          } catch (const std::exception& error) {
              if (!stop.stop_requested()) {
                  events_->Push({StreamEventType::signal_lost, {}, error.what()});
              }
          } catch (...) {
              if (!stop.stop_requested()) {
                  events_->Push({StreamEventType::signal_lost, {}, "局域网直连失败"});
              }
          }
      }) {}

DirectSignalClient::~DirectSignalClient() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

void DirectSignalClient::Send(std::string kind, std::string payload) {
    if (kind == "minirtc") payload = RelayMiniRtc(payload, false);
    const std::lock_guard lock(mutex_);
    if (pending_.size() >= 128) throw std::runtime_error("局域网信令发送队列已满");
    pending_.emplace_back(std::move(kind), std::move(payload));
}

void DirectSignalClient::Run(std::stop_token stop, std::wstring host_address, std::uint16_t port,
    std::wstring access_code) {
    EnsureWinsock();
    if (!port) port = kLanDefaultPort;
    const auto key = DeriveAccessKey(access_code);

    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const auto port_str = std::to_wstring(port);
    PADDRINFOW resolved{};
    if (GetAddrInfoW(host_address.c_str(), port_str.c_str(), &hints, &resolved) != 0 || !resolved) {
        throw std::runtime_error("无法解析目标地址：" + winrt::to_string(host_address));
    }
    struct AddrGuard {
        PADDRINFOW p;
        ~AddrGuard() { if (p) FreeAddrInfoW(p); }
    } addr_guard{resolved};

    SocketHandle sock(socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol));
    if (!sock) throw std::runtime_error("无法创建局域网连接套接字");
    BOOL nodelay = TRUE;
    setsockopt(sock.Get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    SetNonBlocking(sock.Get(), true);

    if (connect(sock.Get(), resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) != 0) {
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) throw std::runtime_error("无法连接到目标主机，请确认 IP、端口与防火墙设置");
        const auto deadline = GetTickCount64() + 5'000;
        bool connected = false;
        while (!stop.stop_requested()) {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            WSAPOLLFD fd{sock.Get(), POLLOUT, 0};
            const int r = WSAPoll(&fd, 1, static_cast<int>(std::min<ULONGLONG>(deadline - now, 50)));
            if (r > 0) {
                int so_err = 0;
                int so_len = sizeof(so_err);
                if (getsockopt(sock.Get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len) == 0 && so_err == 0) {
                    connected = true;
                }
                break;
            }
            if (r < 0) break;
        }
        if (stop.stop_requested()) return;
        if (!connected) throw std::runtime_error("连接目标主机超时，请确认被控端已开启局域网监听");
    }

    JsonObject first{nullptr};
    if (RecvJsonFrame(sock.Get(), first, stop, 5'000) != RecvStatus::ok) {
        throw std::runtime_error("等待被控端握手超时");
    }
    const auto first_type = winrt::to_string(first.GetNamedString(L"type", L""));
    if (first_type == "error") {
        events_->Push({StreamEventType::error, {}, winrt::to_string(first.GetNamedString(L"payload", L"连接被拒绝"))});
        return;
    }
    const auto host_nonce = winrt::to_string(first.GetNamedString(L"nonce", L""));
    if (first_type != "challenge" || host_nonce.empty()) {
        events_->Push({StreamEventType::error, {}, "目标主机返回了无效的握手消息"});
        return;
    }

    const auto viewer_nonce = RandomHex(32);
    const auto viewer_mac = HmacSha256Hex(key, host_nonce + ":" + viewer_nonce + ":viewer");
    const auto expected_host_mac = HmacSha256Hex(key, host_nonce + ":" + viewer_nonce + ":host");

    JsonObject auth;
    auth.SetNamedValue(L"type", JsonValue::CreateStringValue(L"auth"));
    auth.SetNamedValue(L"nonce", JsonValue::CreateStringValue(winrt::to_hstring(viewer_nonce)));
    auth.SetNamedValue(L"hmac", JsonValue::CreateStringValue(winrt::to_hstring(viewer_mac)));
    if (!SendJsonFrame(sock.Get(), auth)) throw std::runtime_error("发送被控码认证失败");

    JsonObject reply{nullptr};
    if (RecvJsonFrame(sock.Get(), reply, stop, 5'000) != RecvStatus::ok) {
        throw std::runtime_error("等待被控端认证结果超时");
    }
    const auto reply_type = winrt::to_string(reply.GetNamedString(L"type", L""));
    if (reply_type == "error") {
        events_->Push({StreamEventType::error, {}, winrt::to_string(reply.GetNamedString(L"payload", L"被控码错误"))});
        return;
    }
    const auto host_mac = winrt::to_string(reply.GetNamedString(L"hmac", L""));
    if (reply_type != "accepted" || !ConstantTimeEquals(host_mac, expected_host_mac)) {
        events_->Push({StreamEventType::error, {}, "被控端身份校验失败"});
        return;
    }
    events_->Push({StreamEventType::signal, "accepted", winrt::to_string(reply.GetNamedString(L"payload", L"control"))});

    FrameReader reader;
    while (!stop.stop_requested()) {
        std::deque<std::pair<std::string, std::string>> outbound;
        {
            const std::lock_guard lock(mutex_);
            outbound.swap(pending_);
        }
        for (const auto& [kind, payload] : outbound) {
            if (!SendTypedFrame(sock.Get(), kind, payload)) {
                events_->Push({StreamEventType::signal_lost, {}, "局域网信令发送失败"});
                return;
            }
        }

        WSAPOLLFD fd{sock.Get(), POLLIN, 0};
        const int ready = WSAPoll(&fd, 1, 25);
        if (ready <= 0) continue;

        std::vector<JsonObject> frames;
        if (!reader.Pump(sock.Get(), frames)) {
            if (!stop.stop_requested()) events_->Push({StreamEventType::signal_lost, {}, "局域网信令连接已断开"});
            return;
        }
        for (const auto& json : frames) {
            const auto type = winrt::to_string(json.GetNamedString(L"type", L""));
            const auto payload = winrt::to_string(json.GetNamedString(L"payload", L""));
            if (type == "minirtc") {
                events_->Push({StreamEventType::signal, type, RelayMiniRtc(payload, true)});
            } else if (type == "ended" || type == "error") {
                events_->Push({StreamEventType::signal, type, payload});
                return;
            }
        }
    }

    if (end_session_.load()) {
        SendTypedFrame(sock.Get(), "ended", "Viewer 已结束会话");
    }
}

LanDiscoveryScanner::LanDiscoveryScanner(std::uint16_t discovery_port)
    : worker_([this, discovery_port](std::stop_token stop) {
          try {
              winrt::init_apartment();
              try {
                  Run(stop, discovery_port);
              } catch (...) {
                  winrt::uninit_apartment();
                  throw;
              }
              winrt::uninit_apartment();
          } catch (...) {}
      }) {}

LanDiscoveryScanner::~LanDiscoveryScanner() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

void LanDiscoveryScanner::Probe() {
    probe_requested_.store(true);
}

std::vector<LanPeer> LanDiscoveryScanner::Snapshot() const {
    const auto now = GetTickCount64();
    const std::lock_guard lock(mutex_);
    std::vector<LanPeer> active;
    for (const auto& peer : peers_) {
        if (now - peer.last_seen_ms <= kPeerExpiryMs) active.push_back(peer);
    }
    std::ranges::sort(active, [](const LanPeer& a, const LanPeer& b) {
        if (a.name != b.name) return a.name < b.name;
        if (a.address != b.address) return a.address < b.address;
        return a.port < b.port;
    });
    return active;
}

void LanDiscoveryScanner::Run(std::stop_token stop, std::uint16_t discovery_port) {
    EnsureWinsock();
    if (!discovery_port) discovery_port = kLanDiscoveryPort;

    SocketHandle sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!sock) return;
    BOOL yes = TRUE;
    setsockopt(sock.Get(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(sock.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    SetNonBlocking(sock.Get(), true);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(discovery_port);
    if (bind(sock.Get(), reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        bind_addr.sin_port = 0;
        if (bind(sock.Get(), reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) return;
    }

    constexpr std::string_view probe_msg = "{\"p2p_lan_discover\":1}";
    ULONGLONG next_auto_probe = 0;

    while (!stop.stop_requested()) {
        const auto now = GetTickCount64();
        if (probe_requested_.exchange(false) || now >= next_auto_probe) {
            next_auto_probe = now + 3'000;
            SendUdpBroadcasts(sock.Get(), discovery_port, probe_msg, true);
        }

        WSAPOLLFD fd{sock.Get(), POLLIN, 0};
        if (WSAPoll(&fd, 1, 50) > 0 && (fd.revents & POLLIN)) {
            char buf[512]{};
            sockaddr_in from{};
            int from_len = sizeof(from);
            const int n = recvfrom(sock.Get(), buf, sizeof(buf) - 1, 0,
                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0) {
                try {
                    const auto json = JsonObject::Parse(winrt::to_hstring(std::string_view(buf, static_cast<std::size_t>(n))));
                    if (json.HasKey(L"p2p_lan_host")) {
                        const auto port_num = static_cast<int>(json.GetNamedNumber(L"port", kLanDefaultPort));
                        if (port_num > 0 && port_num <= 65535) {
                            LanPeer peer;
                            peer.id = json.GetNamedString(L"id", L"").c_str();
                            peer.name = json.GetNamedString(L"name", L"Windows PC").c_str();
                            peer.address = FormatPeerIpWide(from);
                            peer.port = static_cast<std::uint16_t>(port_num);
                            peer.busy = json.GetNamedBoolean(L"busy", false);
                            peer.last_seen_ms = GetTickCount64();
                            if (!peer.address.empty()) {
                                const std::lock_guard lock(mutex_);
                                const auto existing = std::ranges::find_if(peers_, [&](const LanPeer& item) {
                                    return (!peer.id.empty() && item.id == peer.id) ||
                                        (item.address == peer.address && item.port == peer.port);
                                });
                                if (existing != peers_.end()) {
                                    // Prefer non-loopback address if the same host responds on both.
                                    if (existing->address != L"127.0.0.1" && peer.address == L"127.0.0.1") {
                                        existing->name = peer.name;
                                        existing->busy = peer.busy;
                                        existing->last_seen_ms = peer.last_seen_ms;
                                    } else {
                                        *existing = std::move(peer);
                                    }
                                } else {
                                    peers_.push_back(std::move(peer));
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }

        const std::lock_guard lock(mutex_);
        std::erase_if(peers_, [now](const LanPeer& item) { return now - item.last_seen_ms > kPeerExpiryMs; });
    }
}

std::vector<std::wstring> LocalLanAddresses() {
    EnsureWinsock();
    std::vector<std::wstring> out;
    for (const auto& info : EnumerateLocalIpv4()) {
        if (std::ranges::find(out, info.address) == out.end()) out.push_back(info.address);
    }
    return out;
}

std::wstring FormatLocalLanAddresses() {
    const auto addresses = LocalLanAddresses();
    std::wstring joined;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (i) joined += L", ";
        joined += addresses[i];
    }
    return joined;
}

} // namespace p2premote
