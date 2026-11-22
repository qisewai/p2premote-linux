#include "desktop_input.h"

#include "latency_trace.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>

namespace p2premote {
namespace {

constexpr std::array<DWORD, 4> button_down{0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
constexpr std::array<DWORD, 4> button_up{0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};

INPUT Key(std::uint16_t scan, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scan & 0xff;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0)
        | (up ? KEYEVENTF_KEYUP : 0);
    return input;
}

INPUT Mouse(DWORD flags, LONG x = 0, LONG y = 0, DWORD delta = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi = {x, y, delta, flags, 0, 0};
    return input;
}

std::optional<INPUT> Move(HMONITOR monitor, const InputPacket& input) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return std::nullopt;
    const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) return std::nullopt;

    const auto& bounds = info.rcMonitor;
    const auto x = bounds.left - GetSystemMetrics(SM_XVIRTUALSCREEN) + MulDiv(input.x, bounds.right - bounds.left - 1, 65535);
    const auto y = bounds.top - GetSystemMetrics(SM_YVIRTUALSCREEN) + MulDiv(input.y, bounds.bottom - bounds.top - 1, 65535);
    // Aim at the center of the target pixel within the virtual desktop.
    const auto absolute = [](LONG pixel, int extent) {
        return static_cast<LONG>((2 * static_cast<std::int64_t>(pixel) + 1) * 32768 / extent);
    };
    return Mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, absolute(x, width), absolute(y, height));
}

bool Inject(INPUT input) {
    return SendInput(1, &input, sizeof(input)) == 1;
}

} // namespace

DesktopInput::DesktopInput(HMONITOR monitor, LatencyTrace* trace)
    : monitor_(monitor), trace_(trace), worker_([this](std::stop_token stop) { Run(stop); }) {}

DesktopInput::~DesktopInput() {
    worker_.request_stop();
    worker_.join();
}

void DesktopInput::Submit(const InputPacket& input) {
    {
        const std::lock_guard lock(mutex_);
        if (input.kind == InputKind::reset || pending_.size() >= 64) {
            pending_.clear();
            pending_.push_back({InputKind::reset});
        } else if (input.kind == InputKind::move && !pending_.empty() && pending_.back().kind == InputKind::move) {
            pending_.back() = input;
        } else {
            pending_.push_back(input);
        }
    }
    changed_.notify_one();
}

void DesktopInput::Run(std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now();
    while (!stop.stop_requested()) {
        std::unique_lock lock(mutex_);
        const auto queued = [this] { return !pending_.empty(); };
        if (keys_.any() || buttons_.any()) {
            changed_.wait_until(lock, stop, deadline, queued);
        } else {
            changed_.wait(lock, stop, queued);
        }
        if (stop.stop_requested()) break;
        std::optional<InputPacket> input;
        if (queued()) {
            input = pending_.front();
            pending_.pop_front();
        }
        lock.unlock();

        const auto now = Clock::now();
        if ((keys_.any() || buttons_.any()) && now >= deadline) {
            Release();
            deadline = now + std::chrono::milliseconds(input_heartbeat_ms);
        }
        if (input) {
            if (trace_) trace_->Pointer("input_dispatched", *input);
            Apply(*input);
            if (input->kind != InputKind::move) deadline = now + std::chrono::milliseconds(input_lease_ms);
        }
    }
    Release();
}

void DesktopInput::Apply(const InputPacket& input) {
    switch (input.kind) {
    case InputKind::reset:
        Release();
        return;
    case InputKind::key_down:
        if (Inject(Key(input.code, false))) keys_.set(input.code);
        return;
    case InputKind::key_up:
        if (keys_.test(input.code) && Inject(Key(input.code, true))) keys_.reset(input.code);
        return;
    case InputKind::move: {
        const auto position = Move(monitor_, input);
        const bool injected = position && Inject(*position);
        if (trace_) trace_->Pointer(injected ? "input_injected" : "input_injection_failed", input);
        return;
    }
    case InputKind::button_down:
    case InputKind::button_up:
    case InputKind::wheel: {
        const bool wheel = input.kind == InputKind::wheel;
        const bool down = input.kind == InputKind::button_down;
        if (!wheel && buttons_.test(input.code) == down) return;
        const auto flags = wheel ? MOUSEEVENTF_WHEEL : (down ? button_down[input.code] : button_up[input.code]);
        const auto delta = wheel ? static_cast<DWORD>(std::bit_cast<std::int16_t>(input.code)) : 0;
        const auto action = Mouse(flags, 0, 0, delta);
        if (auto position = Move(monitor_, input)) {
            std::array events{*position, action};
            const bool injected = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) == events.size();
            if (trace_) trace_->Pointer(injected ? "input_injected" : "input_injection_failed", input);
            if (injected && !wheel) {
                buttons_.set(input.code, down);
            }
        } else if (input.kind == InputKind::button_up && Inject(action)) {
            buttons_.reset(input.code);
        }
        return;
    }
    case InputKind::text:
    case InputKind::set_quality:
    case InputKind::set_video_settings:
    case InputKind::video_settings_reply:
    case InputKind::cursor_ready:
    case InputKind::heartbeat:
        return;
    }
}

void DesktopInput::Release() {
    for (std::uint16_t scan = 1; scan < keys_.size(); ++scan) {
        if (keys_.test(scan) && Inject(Key(scan, true))) keys_.reset(scan);
    }
    for (std::size_t button = 1; button < buttons_.size(); ++button) {
        if (buttons_.test(button) && Inject(Mouse(button_up[button]))) buttons_.reset(button);
    }
}

} // namespace p2premote
