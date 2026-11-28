#pragma once

#include <cstdint>

namespace p2premote::linux_host {

// Maps a PS/2 Set 1 scancode (with 0x100 flag for E0-extended keys, matching
// Windows SendInput / P2P Remote input_protocol) to a Linux evdev KEY_* code.
// Returns 0 if unmapped.
std::uint16_t Ps2ScancodeToLinuxKey(std::uint16_t scancode) noexcept;

// Maps a Windows Virtual-Key (VK_*) code used in InputCommand shortcuts to a Linux evdev KEY_* code.
std::uint16_t WindowsVkToLinuxKey(std::uint16_t vk) noexcept;

} // namespace p2premote::linux_host
