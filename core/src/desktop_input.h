#pragma once

#include <p2premote/input_protocol.h>

#include <windows.h>

#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace p2premote {

class LatencyTrace;

class DesktopInput {
public:
    explicit DesktopInput(HMONITOR monitor, LatencyTrace* trace = nullptr);
    ~DesktopInput();

    void Submit(const InputPacket& input);
    void Submit(InputCommand command);
    bool DragRecentlyActive() const noexcept;

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
    bool EnsureDesktop(bool force = false) noexcept;
    std::optional<INPUT> BuildMove(const InputPacket& input);
    void Apply(const InputPacket& input);
    void Apply(const InputCommand& command);
    void Release();

    HMONITOR monitor_;
    LatencyTrace* trace_;
    ScreenMetrics metrics_;
    HDESK attached_desktop_{};
    std::wstring desktop_name_;
    std::chrono::steady_clock::time_point last_desktop_check_{};
    std::bitset<512> keys_;
    std::bitset<4> buttons_;
    std::optional<InputPacket> latest_move_;
    std::atomic<ULONGLONG> last_drag_tick_{};
    std::mutex mutex_;
    std::condition_variable_any changed_;
    using PendingInput = std::variant<InputPacket, InputCommand>;
    std::deque<PendingInput> pending_;
    std::jthread worker_;
};

} // namespace p2premote
