#pragma once

#include "input_backend.h"

#include <cstdint>
#include <linux/input-event-codes.h>
#include <p2premote/input_state.h>

namespace p2premote::linux_host {

class UinputInjector final : public InputSink {
public:
    UinputInjector();
    ~UinputInjector() override;

    UinputInjector(const UinputInjector&) = delete;
    UinputInjector& operator=(const UinputInjector&) = delete;

    bool IsAvailable() const noexcept override { return kbd_fd_ >= 0 && mouse_fd_ >= 0; }

    std::string Describe() const override { return "uinput"; }

    // Sends a harmless wakeup key tap to bring a DPMS-blanked display back online.
    void WakeDisplay() noexcept override;

    // Applies a single decoded P2P Remote input packet (move, buttons, wheel, keys, reset).
    void Apply(const InputPacket& packet) noexcept override;

    // Applies a shortcut or text InputCommand (used by shortcut-macros plugin).
    void ApplyCommand(const InputCommand& command) noexcept override;

    // Releases all currently held keys and mouse buttons.
    void ReleaseAll() noexcept override;

private:
    bool CreateKeyboard();
    bool CreateMouse();
    void Emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) noexcept;
    void Sync(int fd) noexcept;

    int kbd_fd_{-1};
    int mouse_fd_{-1};
    int wheel_accum_{};
    PressedInput<KEY_CNT, BTN_TASK + 1> pressed_;
};

} // namespace p2premote::linux_host
