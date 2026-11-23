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

struct RtcCounters {
    std::uint64_t received{};
    std::uint64_t decoded{};
    std::uint64_t sent_packets{};
    std::uint64_t video_packets_received{};
    std::string video_codec;
    double packet_send_queue_seconds{};
    IcePath ice_path{};
    std::string ice_protocol;
    std::optional<double> ice_rtt_ms;
    std::optional<std::uint32_t> upload_bps;
    std::optional<std::uint32_t> download_bps;
    std::optional<double> video_loss_fraction;
    bool srtp_active{};
};

enum class StreamEventType {
    signal, signal_lost, media_lost, error, connected, stats, input_ready, input, cursor, minirtc_signal, video_settings
};

struct StreamEvent {
    StreamEventType type;
    std::string name;
    std::string payload;
    RtcCounters counters{};
    InputPacket input{};
    bool retryable{};
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
        if (desktop_ && input.kind != InputKind::cursor_ready) {
            if (const auto accepted = desktop_input_state_.Accept(input, GetTickCount64())) desktop_->Submit(*accepted);
        } else if (input.kind != InputKind::move) {
            Enqueue({StreamEventType::input, {}, {}, {}, input});
        } else if (!motion_ || motion_->sequence < input.sequence) {
            motion_ = input;
            SetEvent(ready_.get());
        }
    }

    void PushInputCommand(InputCommand command) {
        const std::lock_guard lock(mutex_);
        if (desktop_ && desktop_input_state_.AcceptCommand(command, GetTickCount64())) desktop_->Submit(std::move(command));
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
