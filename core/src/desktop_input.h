#pragma once

#include <p2premote/input_protocol.h>

#include <windows.h>

#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace p2premote {

class LatencyTrace;

class DesktopInput {
public:
    explicit DesktopInput(HMONITOR monitor, LatencyTrace* trace = nullptr);
    ~DesktopInput();

    void Submit(const InputPacket& input);

private:
    struct ScreenMetrics {
        RECT monitor_bounds{};
        int virtual_left{};
        int virtual_top{};
        int virtual_width{};
        int virtual_height{};
        std::chrono::steady_clock::time_point updated{};
        bool valid{};
    };

    void Run(std::stop_token stop);
    std::optional<INPUT> BuildMove(const InputPacket& input);
    void Apply(const InputPacket& input);
    void Release();

    HMONITOR monitor_;
    LatencyTrace* trace_;
    ScreenMetrics metrics_;
    std::bitset<512> keys_;
    std::bitset<4> buttons_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<InputPacket> pending_;
    std::jthread worker_;
};

} // namespace p2premote
