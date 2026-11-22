#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

namespace p2premote {

class StreamEvents;

class SignalSocket {
public:
    SignalSocket(std::shared_ptr<StreamEvents> events, std::wstring endpoint, std::string kind, std::string payload,
        std::wstring token = {}, bool managed = false, bool minirtc = false);
    void Send(std::string kind, std::string payload = {}, bool minirtc = false);
    void EndSession() noexcept { end_session_.store(true); }

private:
    void Run(std::stop_token stop, const std::wstring& endpoint, const std::string& kind, const std::string& payload,
        const std::wstring& token, bool managed, bool minirtc);

    std::shared_ptr<StreamEvents> events_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::tuple<std::string, std::string, bool>> pending_;
    std::atomic_bool end_session_{};
    std::jthread worker_;
};

} // namespace p2premote
