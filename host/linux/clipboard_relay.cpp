#include "clipboard_relay.h"
#include "clipboard_endpoint.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <poll.h>
#include <unistd.h>

namespace p2premote::linux_host {

ClipboardRelay::ClipboardRelay(FrameVt vt, Send send)
    : worker_([this, vt = std::move(vt), send = std::move(send)](std::stop_token stop) { Run(stop, vt, send); }) {}

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

void ClipboardRelay::Run(std::stop_token stop, const FrameVt& vt, const Send& send) {
    using namespace std::chrono_literals;
    std::error_code ec;
    const auto dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    if (ec) return;
    const auto helper = dir / "p2premote_clipboard";
    int fd = -1;
    std::optional<ClipboardDesktop> desktop;
    auto check_at = std::chrono::steady_clock::time_point{};
    std::array<char, plugin_frame_limit> buffer;
    std::string outgoing;
    while (!stop.stop_requested()) {
        const unsigned frame_vt = vt();
        if (desktop && desktop->vt != frame_vt) break;
        const auto now = std::chrono::steady_clock::now();
        if (frame_vt && now >= check_at) {
            check_at = now + 200ms;
            const auto active = ActiveClipboardDesktop();
            if (desktop && desktop != active) break;
            if (!desktop && active && active->vt == frame_vt) {
                fd = OpenClipboardEndpoint(*active, false);
                if (fd >= 0 && !ClipboardPeer(fd, active->uid, helper.string())) {
                    close(fd);
                    fd = -1;
                }
                if (fd >= 0) {
                    desktop = active;
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
            if (send(frame_vt, outgoing)) outgoing.clear();
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
        if (!send(frame_vt, packet)) outgoing.assign(packet);
    }
    if (fd >= 0) close(fd);
    const std::lock_guard lock(mutex_);
    failed_ = true;
    pending_.clear();
}

} // namespace p2premote::linux_host
