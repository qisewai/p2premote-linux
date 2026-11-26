#pragma once

#include <p2premote/input_protocol.h>

#include <cstdint>
#include <set>

namespace p2premote::linux_host {

// Maps a PS/2 Set 1 scancode (with 0x100 flag for E0-extended keys, matching
// Windows SendInput / P2P Remote input_protocol) to a Linux evdev KEY_* code.
// Returns 0 if unmapped.
std::uint16_t Ps2ScancodeToLinuxKey(std::uint16_t scancode) noexcept;

class UinputInjector final {
public:
    UinputInjector();
    ~UinputInjector();

    UinputInjector(const UinputInjector&) = delete;
    UinputInjector& operator=(const UinputInjector&) = delete;

    bool Valid() const noexcept { return kbd_fd_ >= 0 && mouse_fd_ >= 0; }

    // Sends a harmless wakeup key tap to bring a DPMS-blanked display back online.
    void WakeDisplay() noexcept;

    // Applies a single decoded P2P Remote input packet (move, buttons, wheel, keys, reset).
    void Apply(const InputPacket& packet) noexcept;

    // Releases all currently held keys and mouse buttons.
    void ReleaseAll() noexcept;

private:
    bool CreateKeyboard();
    bool CreateMouse();
    void Emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) noexcept;
    void Sync(int fd) noexcept;

    int kbd_fd_{-1};
    int mouse_fd_{-1};
    int wheel_accum_{};
    std::set<std::uint16_t> pressed_keys_;
    std::set<std::uint16_t> pressed_buttons_;
};

} // namespace p2premote::linux_host
