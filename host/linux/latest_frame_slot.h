#pragma once

#include "desktop_session.h"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace p2premote::linux_host {

class LatestFrameSlot {
public:
    // The producer retains the plane data through frame.storage.
    void Push(VideoFrameView frame) {
        {
            const std::lock_guard lock(mutex_);
            latest_ = std::move(frame);
            changed_ = true;
        }
        activity_.notify_all();
    }

    void Clear() {
        {
            const std::lock_guard lock(mutex_);
            latest_.reset();
            changed_ = true;
        }
        activity_.notify_all();
    }

    bool Wait(std::chrono::microseconds timeout, std::stop_token stop = {}) {
        std::unique_lock lock(mutex_);
        return activity_.wait_for(lock, stop, timeout, [this] { return changed_; });
    }

    std::optional<VideoFrameView> Acquire() {
        const std::lock_guard lock(mutex_);
        changed_ = false;
        return latest_;
    }

private:
    std::mutex mutex_;
    std::condition_variable_any activity_;
    std::optional<VideoFrameView> latest_;
    bool changed_{};
};

} // namespace p2premote::linux_host
