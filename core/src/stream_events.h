#pragma once

#include "desktop_input.h"
#include "latency_trace.h"

#include <p2premote/stream_session.h>

#include <windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace p2premote {

struct IceCheckStats {
    std::uint64_t requests_sent{};
    std::uint64_t requests_received{};
    std::uint64_t responses_received{};
    std::string dtls_state;
};

struct RtcCounters {
    std::uint64_t received{};
    std::uint64_t decoded{};
    std::uint64_t encoded{};
    double decode_seconds{};
    double encode_seconds{};
    std::uint64_t rtc_frames_dropped{};
    std::uint64_t jitter_buffer_frames{};
    std::uint64_t sent_packets{};
    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::uint64_t video_packets_received{};
    std::int64_t video_packets_lost{};
    std::string video_codec;
    double jitter_buffer_seconds{};
    double packet_send_queue_seconds{};
    IcePath ice_path{};
    std::string ice_protocol;
    std::string relay_protocol;
    std::optional<double> ice_rtt_ms;
    std::optional<IceCheckStats> ice_checks;
    IceDiagnostics ice_diagnostics;
    std::optional<std::uint32_t> upload_bps;
    std::optional<std::uint32_t> download_bps;
    std::optional<double> video_loss_fraction;
    bool srtp_active{};
};

enum class StreamEventType { signal, signal_lost, media_lost, error, gathered, local_set, remote_set,
    ice_candidate, connected, stats, input_ready, input, cursor, minirtc_signal, video_settings };

struct StreamEvent {
    StreamEventType type;
    std::string name;
    std::string payload;
    RtcCounters counters{};
    InputPacket input{};
    bool retryable{};
    bool minirtc{};
};

class StreamEvents {
public:
    explicit StreamEvents(std::shared_ptr<LatencyTrace> trace = {}) : trace_(std::move(trace)) {
        winrt::check_pointer(ready_.get());
    }

    HANDLE Ready() const noexcept { return ready_.get(); }
    const std::shared_ptr<LatencyTrace>& Trace() const noexcept { return trace_; }

    void Notify() {
        const std::lock_guard lock(mutex_);
        SetEvent(ready_.get());
    }

    void Push(StreamEvent event) {
        const std::lock_guard lock(mutex_);
        Enqueue(std::move(event));
    }

    void SetDesktopInput(DesktopInput* desktop) {
        const std::lock_guard lock(mutex_);
        desktop_ = desktop;
    }

    void PushInput(const InputPacket& input) {
        const std::lock_guard lock(mutex_);
        if (desktop_ && input.kind != InputKind::set_quality && input.kind != InputKind::cursor_ready) {
            if (const auto accepted = desktop_input_state_.Accept(input, GetTickCount64())) desktop_->Submit(*accepted);
        } else if (input.kind != InputKind::move) {
            Enqueue({StreamEventType::input, {}, {}, {}, input});
        } else if (!motion_ || motion_->sequence < input.sequence) {
            motion_ = input;
            SetEvent(ready_.get());
        }
    }

    std::vector<StreamEvent> Drain() {
        const std::lock_guard lock(mutex_);
        ResetEvent(ready_.get());
        if (motion_) {
            events_.push_back({StreamEventType::input, {}, {}, {}, *motion_});
            motion_.reset();
        }
        return std::exchange(events_, {});
    }

private:
    void Enqueue(StreamEvent event) {
        if (events_.size() < 64) {
            events_.push_back(std::move(event));
        } else if (events_.size() == 64) {
            events_.push_back({StreamEventType::error, {}, "会话事件队列已满"});
        }
        SetEvent(ready_.get());
    }

    winrt::handle ready_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::shared_ptr<LatencyTrace> trace_;
    std::mutex mutex_;
    std::vector<StreamEvent> events_;
    std::optional<InputPacket> motion_;
    DesktopInput* desktop_{};
    InputReceiveState desktop_input_state_;
};

} // namespace p2premote
