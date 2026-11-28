#pragma once

#include <p2premote/input_protocol.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace p2premote {

class ReconnectWindow {
public:
    static constexpr std::uint64_t duration_ms = 30'000;

    explicit ReconnectWindow(std::uint64_t now = 0) noexcept : deadline_(now + duration_ms) {}

    unsigned Schedule(std::uint64_t now) noexcept {
        const auto seconds = 1u << std::min(attempts_, 2u);
        next_attempt_ = now + 1'000 * seconds;
        return seconds;
    }
    [[nodiscard]] bool Expired(std::uint64_t now) const noexcept { return now >= deadline_; }
    [[nodiscard]] bool Due(std::uint64_t now) const noexcept { return now >= next_attempt_ && !Expired(now); }
    [[nodiscard]] std::uint64_t SecondsRemaining(std::uint64_t now) const noexcept {
        return now < next_attempt_ ? (next_attempt_ - now + 999) / 1'000 : 0;
    }
    unsigned BeginAttempt() noexcept { return ++attempts_; }
    [[nodiscard]] unsigned Attempts() const noexcept { return attempts_; }

private:
    std::uint64_t deadline_{};
    std::uint64_t next_attempt_{};
    unsigned attempts_{};
};

enum class SignalTermination { none, reconnect, stop };

constexpr SignalTermination ClassifySignalTermination(std::string_view type, std::string_view message,
    bool retryable, bool reconnecting) noexcept {
    if (type == "ended") return retryable ? SignalTermination::reconnect : SignalTermination::stop;
    if (type != "error") return SignalTermination::none;
    const bool transient = retryable || message == "设备不可连接" || message == "设备未在线或正在使用中";
    return reconnecting && transient ? SignalTermination::reconnect : SignalTermination::stop;
}

enum class SessionMode { invalid, view, control, diagnostics };

constexpr SessionMode ParseSessionMode(std::string_view mode) noexcept {
    if (mode == "view") return SessionMode::view;
    if (mode == "control") return SessionMode::control;
    if (mode == "diagnostics") return SessionMode::diagnostics;
    return SessionMode::invalid;
}

enum class ViewerSignalStage { inactive, challenge, authentication, approval, media };
enum class ViewerSignal { unexpected, challenge, accepted, waiting, minirtc };

constexpr ViewerSignal ClassifyViewerSignal(ViewerSignalStage stage, std::string_view type) noexcept {
    if (stage == ViewerSignalStage::challenge && type == "challenge") return ViewerSignal::challenge;
    if ((stage == ViewerSignalStage::authentication || stage == ViewerSignalStage::approval) && type == "accepted") {
        return ViewerSignal::accepted;
    }
    if (stage == ViewerSignalStage::approval && type == "waiting") return ViewerSignal::waiting;
    if (stage == ViewerSignalStage::media && type == "minirtc") return ViewerSignal::minirtc;
    return ViewerSignal::unexpected;
}

enum class LanMediaSignal { invalid, join, description, candidate, candidate_mid };

constexpr LanMediaSignal ClassifyLanMedia(std::string_view type, bool from_host) noexcept {
    if (!from_host && (type == "join_transmission" || type == "user_join_transmission")) return LanMediaSignal::join;
    if ((from_host && type == "offer") || (!from_host && type == "answer")) return LanMediaSignal::description;
    if (type == "new_candidate") return LanMediaSignal::candidate;
    if (type == "new_candidate_mid") return LanMediaSignal::candidate_mid;
    return LanMediaSignal::invalid;
}

enum class LanProofRole { host, viewer };
inline constexpr char lan_access_key_prefix[] = "p2premote-lan-v1:";

constexpr bool IsLanNonce(std::string_view nonce) noexcept {
    return nonce.size() == 64 && std::ranges::all_of(nonce, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

inline std::string LanProofMessage(std::string_view host_nonce, std::string_view viewer_nonce, LanProofRole role) {
    std::string message;
    message.reserve(host_nonce.size() + viewer_nonce.size() + 8);
    message.append(host_nonce).append(":").append(viewer_nonce);
    return message.append(role == LanProofRole::host ? ":host" : ":viewer");
}

template<class Mac, class Equals>
bool VerifyLanProof(std::string_view host_nonce, std::string_view viewer_nonce, LanProofRole role,
    std::string_view proof, Mac make_mac, Equals equals) {
    if (host_nonce.empty() || viewer_nonce.empty() || proof.size() != 64) return false;
    return equals(proof, make_mac(LanProofMessage(host_nonce, viewer_nonce, role)));
}

struct VideoSettings {
    StreamQuality quality;
    std::uint32_t bitrate;
    bool operator==(const VideoSettings&) const = default;
};

class VideoSettingsNegotiation {
public:
    struct RetryPolicy {
        std::uint64_t interval_ms;
        std::uint64_t timeout_ms;
        unsigned max_sends;
    };
    struct Reply {
        VideoSettings settings;
        std::uint32_t applied_bitrate;
    };

    explicit VideoSettingsNegotiation(RetryPolicy policy) noexcept : policy_(policy) {}

    template<class Send>
    bool Update(VideoSettings desired, std::uint64_t now, std::uint32_t& sequence, Send send) {
        bool timed_out = false;
        if (pending_) {
            auto& request = *pending_;
            if (now < request.deadline) {
                if (request.sends < policy_.max_sends && now >= request.next_send && send(request.packet)) {
                    ++request.sends;
                    request.next_send = now + policy_.interval_ms;
                }
                return false;
            }
            settled_ = request.settings;
            pending_.reset();
            timed_out = true;
        }
        if (settled_ != desired) {
            const auto packet = VideoSettingsPacket(InputKind::set_video_settings, desired.quality, desired.bitrate,
                ++sequence, now);
            if (send(packet)) pending_ = Pending{desired, packet, now + policy_.timeout_ms, now + policy_.interval_ms, 1};
        }
        return timed_out;
    }

    std::optional<Reply> Acknowledge(const InputPacket& reply) noexcept {
        if (!pending_ || reply.kind != InputKind::video_settings_reply || reply.sequence != pending_->packet.sequence ||
            reply.code != pending_->packet.code) return std::nullopt;
        settled_ = pending_->settings;
        pending_.reset();
        return Reply{*settled_, VideoSettingsBitrate(reply)};
    }

    void RequestAgain() noexcept { settled_.reset(); }
    void Reset() noexcept {
        pending_.reset();
        settled_.reset();
    }

private:
    struct Pending {
        VideoSettings settings;
        InputPacket packet;
        std::uint64_t deadline;
        std::uint64_t next_send;
        unsigned sends;
    };
    RetryPolicy policy_;
    std::optional<Pending> pending_;
    std::optional<VideoSettings> settled_;
};

} // namespace p2premote
