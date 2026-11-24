#include "desktop_input.h"

#include "desktop_switch.h"
#include "latency_trace.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <utility>

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

INPUT UnicodeKey(std::uint16_t code, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = code;
    input.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    return input;
}

bool IsExtendedVirtualKey(std::uint16_t key) noexcept {
    switch (key) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_DIVIDE:
        return true;
    default:
        return false;
    }
}

INPUT Mouse(DWORD flags, LONG x = 0, LONG y = 0, DWORD delta = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi = {x, y, delta, flags, 0, 0};
    return input;
}

bool Inject(INPUT input) {
    return SendInput(1, &input, sizeof(input)) == 1;
}

} // namespace

std::optional<INPUT> DesktopInput::BuildMove(const InputPacket& input) {
    const auto now = std::chrono::steady_clock::now();
    if (!metrics_.valid || now - metrics_.updated >= std::chrono::milliseconds(500)) {
        MONITORINFO info{sizeof(info)};
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (!GetMonitorInfoW(monitor_, &info) || width <= 0 || height <= 0) {
            metrics_.valid = false;
            return std::nullopt;
        }
        metrics_.monitor_bounds = info.rcMonitor;
        metrics_.virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        metrics_.virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        metrics_.virtual_width = width;
        metrics_.virtual_height = height;
        metrics_.updated = now;
        metrics_.valid = true;
    }

    const auto& bounds = metrics_.monitor_bounds;
    const auto x = bounds.left - metrics_.virtual_left + MulDiv(input.x, bounds.right - bounds.left - 1, 65535);
    const auto y = bounds.top - metrics_.virtual_top + MulDiv(input.y, bounds.bottom - bounds.top - 1, 65535);
    // Aim at the center of the target pixel within the virtual desktop.
    const auto absolute = [](LONG pixel, int extent) {
        return static_cast<LONG>((2 * static_cast<std::int64_t>(pixel) + 1) * 32768 / extent);
    };
    return Mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
        absolute(x, metrics_.virtual_width), absolute(y, metrics_.virtual_height));
}

DesktopInput::DesktopInput(HMONITOR monitor, LatencyTrace* trace)
    : monitor_(monitor), trace_(trace), worker_([this](std::stop_token stop) { Run(stop); }) {}

DesktopInput::~DesktopInput() {
    worker_.request_stop();
    worker_.join();
}

bool DesktopInput::DragRecentlyActive() const noexcept {
    const auto last = last_drag_tick_.load(std::memory_order_relaxed);
    return last && GetTickCount64() - last < 64;
}

void DesktopInput::Submit(const InputPacket& input) {
    {
        const std::lock_guard lock(mutex_);
        auto* previous = pending_.empty() ? nullptr : std::get_if<InputPacket>(&pending_.back());
        if (input.kind == InputKind::reset || pending_.size() >= 64) {
            pending_.clear();
            pending_.push_back(InputPacket{InputKind::reset});
        } else if (input.kind == InputKind::move && previous && previous->kind == InputKind::move) {
            *previous = input;
        } else {
            pending_.push_back(input);
        }
    }
    changed_.notify_one();
}

void DesktopInput::Submit(InputCommand command) {
    {
        const std::lock_guard lock(mutex_);
        if (pending_.size() >= 64) return;
        pending_.push_back(std::move(command));
    }
    changed_.notify_one();
}

bool DesktopInput::EnsureDesktop(bool force) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_desktop_check_ < std::chrono::milliseconds(250)) return false;
    last_desktop_check_ = now;
    if (SyncThreadToInputDesktop(attached_desktop_, desktop_name_)) {
        metrics_.valid = false;
        latest_move_.reset();
        return true;
    }
    return false;
}

void DesktopInput::Run(std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    EnsureDesktop(true);
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
        std::optional<PendingInput> input;
        if (queued()) {
            input = std::move(pending_.front());
            pending_.pop_front();
        }
        lock.unlock();

        EnsureDesktop(false);
        const auto now = Clock::now();
        if ((keys_.any() || buttons_.any()) && now >= deadline) {
            Release();
            deadline = now + std::chrono::milliseconds(input_heartbeat_ms);
        }
        if (input) {
            if (const auto* packet = std::get_if<InputPacket>(&*input)) {
                if (trace_) trace_->Pointer("input_dispatched", *packet);
                Apply(*packet);
                if (packet->kind != InputKind::move) deadline = now + std::chrono::milliseconds(input_lease_ms);
            } else {
                Apply(std::get<InputCommand>(*input));
            }
        }
    }
    Release();
    if (attached_desktop_) {
        CloseDesktop(attached_desktop_);
        attached_desktop_ = nullptr;
    }
}

void DesktopInput::Apply(const InputCommand& command) {
    const auto layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    std::array<INPUT, 2 * input_text_limit> events;
    std::array<std::uint16_t, shortcut_limit> scans{};
    UINT count{};
    if (command.kind == InputCommandKind::shortcut) {
        // Windows blocks simulated Win+L via SendInput; lock the workstation directly instead.
        if (command.units.size() == 2 && (command.units[0] == VK_LWIN || command.units[0] == VK_RWIN) && command.units[1] == 'L') {
            Release();
            LockWorkStation();
            return;
        }
        // Windows blocks simulated Ctrl+Alt+Del via SendInput; invoke SendSAS when running as SYSTEM.
        if (command.units.size() == 3 &&
            (command.units[0] == VK_LCONTROL || command.units[0] == VK_RCONTROL || command.units[0] == VK_CONTROL) &&
            (command.units[1] == VK_LMENU || command.units[1] == VK_RMENU || command.units[1] == VK_MENU) &&
            command.units[2] == VK_DELETE) {
            Release();
            SendSecureAttentionSequence();
            return;
        }
        for (std::size_t i = 0; i < command.units.size(); ++i) {
            const auto key = command.units[i];
            const auto scan = MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, layout);
            if (!scan || (scan > 0xff && (scan & 0xff00) != 0xe000)) return;
            const bool extended = scan > 0xff || IsExtendedVirtualKey(key);
            scans[i] = static_cast<std::uint16_t>((scan & 0xff) | (extended ? 0x100 : 0));
            if (keys_.test(scans[i]) || (GetAsyncKeyState(key) & 0x8000)) return;
            events[count++] = Key(scans[i], false);
        }
        for (auto i = command.units.size(); i > 0; --i) events[count++] = Key(scans[i - 1], true);
    } else {
        bool previous_cr{};
        for (const auto unit : command.units) {
            const bool skip = unit == '\n' && previous_cr;
            previous_cr = unit == '\r';
            if (skip) continue;
            // Enter and Tab need physical key events to behave as typed input.
            if (unit == '\n' || unit == '\r' || unit == '\t') {
                const auto scan = static_cast<std::uint16_t>(MapVirtualKeyExW(unit == '\t' ? VK_TAB : VK_RETURN, MAPVK_VK_TO_VSC, layout));
                if (!scan || keys_.test(scan)) return;
                events[count++] = Key(scan, false);
                events[count++] = Key(scan, true);
            } else {
                events[count++] = UnicodeKey(unit, false);
                events[count++] = UnicodeKey(unit, true);
            }
        }
    }
    if (!count) return;
    const auto sent = SendInput(count, events.data(), sizeof(INPUT));
    if (sent == count) return;
    if (command.kind == InputCommandKind::shortcut) {
        // Release every key that might have made it down if Windows stopped mid-batch.
        auto pressed = std::min<std::size_t>(sent, command.units.size());
        while (pressed > 0) {
            const auto scan = scans[--pressed];
            if (!Inject(Key(scan, true))) keys_.set(scan);
        }
    } else if (sent % 2) {
        auto up = events[sent];
        Inject(up);
    }
}

void DesktopInput::Apply(const InputPacket& input) {
    auto inject_one = [this](INPUT event) {
        if (Inject(event)) return true;
        return EnsureDesktop(true) && Inject(event);
    };
    switch (input.kind) {
    case InputKind::reset:
        Release();
        return;
    case InputKind::key_down:
        if (inject_one(Key(input.code, false))) keys_.set(input.code);
        return;
    case InputKind::key_up:
        if (keys_.test(input.code) && inject_one(Key(input.code, true))) keys_.reset(input.code);
        return;
    case InputKind::move: {
        const auto position = BuildMove(input);
        const bool injected = position && inject_one(*position);
        if (injected) {
            latest_move_ = input;
            if (buttons_.any()) last_drag_tick_.store(GetTickCount64(), std::memory_order_relaxed);
        }
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
        if (auto position = BuildMove(input)) {
            std::array events{*position, action};
            bool injected = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) == events.size();
            if (!injected && EnsureDesktop(true)) {
                injected = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) == events.size();
            }
            if (trace_) trace_->Pointer(injected ? "input_injected" : "input_injection_failed", input);
            if (injected && !wheel) {
                buttons_.set(input.code, down);
                if (down) last_drag_tick_.store(GetTickCount64(), std::memory_order_relaxed);
            }
            // Motion can overtake an ordered control on the other channel. Apply
            // the edge at its original position, then catch up in the new button state.
            if (injected && latest_move_ && latest_move_->sequence > input.sequence) {
                if (auto latest = BuildMove(*latest_move_)) inject_one(*latest);
            } else if (injected) {
                latest_move_.reset();
            }
        } else if (input.kind == InputKind::button_up && inject_one(action)) {
            buttons_.reset(input.code);
        }
        return;
    }
    case InputKind::set_video_settings:
    case InputKind::video_settings_reply:
    case InputKind::cursor_ready:
    case InputKind::heartbeat:
        return;
    }
}

void DesktopInput::Release() {
    latest_move_.reset();
    last_drag_tick_.store(0, std::memory_order_relaxed);
    for (std::uint16_t scan = 1; scan < keys_.size(); ++scan) {
        if (keys_.test(scan) && Inject(Key(scan, true))) keys_.reset(scan);
    }
    for (std::size_t button = 1; button < buttons_.size(); ++button) {
        if (buttons_.test(button) && Inject(Mouse(button_up[button]))) buttons_.reset(button);
    }
}

} // namespace p2premote
