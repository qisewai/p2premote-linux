#include "clipboard_relay.h"
#include "clipboard_endpoint.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <poll.h>
#include <unistd.h>

namespace p2premote::linux_host {
namespace {

class ClipboardRelay final : public ClipboardBridge {
public:
    ClipboardRelay(ActiveTarget target, Send send);
    void Receive(std::string_view bytes) override;

private:
    void Run(std::stop_token stop, const ActiveTarget& target, const Send& send);
    std::mutex mutex_;
    std::deque<std::string> pending_;
    size_t queued_bytes_{};
    bool attached_{}, failed_{};
    std::jthread worker_; // join before destroying the queue
};

ClipboardRelay::ClipboardRelay(ActiveTarget target, Send send)
    : worker_([this, target = std::move(target), send = std::move(send)](std::stop_token stop) { Run(stop, target, send); }) {}

void ClipboardRelay::Receive(std::string_view bytes) {
    if (!ClipboardFrame(bytes)) return;
    const std::lock_guard lock(mutex_);
    if (failed_) return;
    if (!attached_) {
        const auto msg = DecodePluginPacket(bytes);
        if (msg->type != "hello" || bytes.size() > 128 || pending_.size() >= 8) return;
        for (const auto& existing : pending_) if (existing == bytes) return;
    }
    if (pending_.size() >= 256 || queued_bytes_ + bytes.size() > 2 * 1024 * 1024) {
        failed_ = true;
        return;
    }
    pending_.emplace_back(bytes);
    queued_bytes_ += bytes.size();
}

void ClipboardRelay::Run(std::stop_token stop, const ActiveTarget& target, const Send& send) {
    using namespace std::chrono_literals;
    std::error_code ec;
    const auto dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    if (ec) return;
    const auto helper = dir / "p2premote_clipboard";
    int fd = -1;
    std::optional<DesktopTarget> desktop;
    auto check_at = std::chrono::steady_clock::time_point{};
    std::array<char, plugin_frame_limit> buffer;
    std::string outgoing;
    while (!stop.stop_requested()) {
        const auto frame_target = target();
        if (desktop && desktop != frame_target) break;
        const auto now = std::chrono::steady_clock::now();
        if (frame_target && now >= check_at) {
            check_at = now + 200ms;
            const auto active = ActiveUserDesktop();
            const bool accessible = active && active->valid() && frame_target->SameLogin(*active);
            if (desktop && !accessible) break;
            if (!desktop && accessible) {
                fd = OpenClipboardEndpoint(*active, false);
                if (fd >= 0 && !ClipboardPeer(fd, active->uid, helper.string())) {
                    close(fd);
                    fd = -1;
                }
                if (fd >= 0) {
                    desktop = frame_target;
                    const std::lock_guard lock(mutex_);
                    attached_ = true;
                }
            }
        }
        if (fd < 0) {
            std::this_thread::sleep_for(50ms);
            continue;
        }
        {
            const std::lock_guard lock(mutex_);
            while (!failed_ && !pending_.empty()) {
                const auto& packet = pending_.front();
                const auto n = ::send(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n != static_cast<ssize_t>(packet.size())) { failed_ = true; break; }
                queued_bytes_ -= packet.size();
                pending_.pop_front();
            }
            if (failed_) break;
        }
        if (!outgoing.empty()) {
            if (send(*desktop, outgoing)) outgoing.clear();
            else std::this_thread::sleep_for(5ms);
            continue;
        }
        pollfd wait{fd, POLLIN, 0};
        if (poll(&wait, 1, 10) <= 0) continue;
        const auto n = recv(fd, buffer.data(), buffer.size(), MSG_TRUNC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0 || static_cast<size_t>(n) > buffer.size()) break;
        const std::string_view packet(buffer.data(), n);
        if (!ClipboardFrame(packet)) break;
        if (!send(*desktop, packet)) outgoing.assign(packet);
    }
    if (fd >= 0) close(fd);
    const std::lock_guard lock(mutex_);
    failed_ = true;
    pending_.clear();
}

} // namespace

std::unique_ptr<ClipboardBridge> CreateClipboardBridge(ClipboardBridge::ActiveTarget target, ClipboardBridge::Send send) {
    return std::make_unique<ClipboardRelay>(std::move(target), std::move(send));
}

} // namespace p2premote::linux_host
