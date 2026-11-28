#pragma once

#include "desktop_session.h"

#include <utility>

namespace p2premote::linux_host {

// Owned by a host session and serialized with that session's input mutex.
class DesktopAccess {
public:
    void Attach(InputSink* sink) noexcept { sink_ = sink; }
    bool CanInject() const noexcept { return sink_ && sink_->IsAvailable(); }
    const DesktopTarget& Target() const noexcept { return target_; }
    bool Matches(DesktopSessionState state, const DesktopTarget& target) const {
        return state == DesktopSessionState::ready && target_.valid() && target_ == target;
    }
    std::optional<DesktopTarget> UserTarget() const {
        if (!target_.valid() || target_.session_id.empty()) return {};
        return target_;
    }

    bool Activate(DesktopTarget target) {
        if (!target.valid() || target_ == target) return false;
        Clear();
        target_ = std::move(target);
        if (CanInject()) sink_->WakeDisplay();
        return true;
    }

    bool Recheck(DesktopSessionState state, const DesktopTarget& target, std::uint64_t now) {
        if (target_.valid() && !Matches(state, target)) {
            Clear();
            return true;
        }
        if (last_input_ms_ && now - last_input_ms_ > input_lease_ms) Release();
        return false;
    }

    void Clear() noexcept {
        Release();
        target_ = {};
        received_ = {};
    }

    void Apply(const InputPacket& packet, std::uint64_t now) {
        if (!CanInject()) return;
        if (const auto accepted = received_.Accept(packet, now)) {
            last_input_ms_ = now;
            if (accepted->kind != InputKind::heartbeat) sink_->Apply(*accepted);
        }
    }

    void Apply(const InputCommand& command, std::uint64_t now) {
        if (!CanInject() || !received_.AcceptCommand(command, now)) return;
        last_input_ms_ = now;
        sink_->ApplyCommand(command);
    }

private:
    void Release() noexcept {
        if (sink_) sink_->ReleaseAll();
        last_input_ms_ = 0;
    }

    InputSink* sink_{};
    InputReceiveState received_;
    std::uint64_t last_input_ms_{};
    DesktopTarget target_;
};

} // namespace p2premote::linux_host
