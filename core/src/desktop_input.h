#pragma once

#include <p2premote/input_protocol.h>

#include <windows.h>

#include <bitset>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace p2premote {

class LatencyTrace;

class DesktopInput {
public:
    explicit DesktopInput(HMONITOR monitor, LatencyTrace* trace = nullptr);
    ~DesktopInput();

    void Submit(const InputPacket& input);

private:
    void Run(std::stop_token stop);
    void Apply(const InputPacket& input);
    void Release();

    HMONITOR monitor_;
    LatencyTrace* trace_;
    std::bitset<512> keys_;
    std::bitset<4> buttons_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<InputPacket> pending_;
    std::jthread worker_;
};

} // namespace p2premote
