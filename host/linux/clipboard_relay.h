#pragma once
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace p2premote::linux_host {

class ClipboardRelay {
public:
    using FrameVt = std::function<unsigned()>;
    using Send = std::function<bool(unsigned, std::string_view)>;
    ClipboardRelay(FrameVt vt, Send send);
    void Receive(std::string_view bytes);

private:
    void Run(std::stop_token stop, const FrameVt& vt, const Send& send);
    std::mutex mutex_;
    std::deque<std::string> pending_;
    size_t queued_bytes_{};
    bool attached_{}, failed_{};
    std::jthread worker_; // join before destroying the queue
};

} // namespace p2premote::linux_host
