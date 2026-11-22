#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace p2premote {

class StreamEvents;

class SignalSocket {
public:
    SignalSocket(std::shared_ptr<StreamEvents> events, std::wstring endpoint, std::string kind, std::string payload,
        std::wstring token = {}, bool managed = false);
    void Send(std::string kind, std::string payload = {});
    void EndSession() noexcept { end_session_.store(true); }

private:
    void Run(std::stop_token stop, const std::wstring& endpoint, const std::string& kind, const std::string& payload,
        const std::wstring& token, bool managed);

    std::shared_ptr<StreamEvents> events_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::pair<std::string, std::string>> pending_;
    std::atomic_bool end_session_{};
    std::jthread worker_;
};

} // namespace p2premote
