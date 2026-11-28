#include "linux_key_map.h"

#include <linux/input-event-codes.h>

namespace p2premote::linux_host {

std::uint16_t Ps2ScancodeToLinuxKey(std::uint16_t scancode) noexcept {
    const bool extended = (scancode & 0x100u) != 0;
    const std::uint8_t base = static_cast<std::uint8_t>(scancode & 0xffu);
    if (!extended) {
        // Standard PS/2 Set 1 scancodes 0x01..0x58 map 1:1 to Linux evdev KEY_* codes!
        if (base >= 0x01 && base <= 0x58) {
            return base;
        }
        return 0;
    }
    // Extended (E0-prefixed) PS/2 Set 1 scancodes -> Linux KEY_* codes
    switch (base) {
    case 0x1c: return KEY_KPENTER;
    case 0x1d: return KEY_RIGHTCTRL;
    case 0x35: return KEY_KPSLASH;
    case 0x37: return KEY_SYSRQ;
    case 0x38: return KEY_RIGHTALT;
    case 0x45: return KEY_NUMLOCK;
    case 0x46: return KEY_PAUSE;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PAGEUP;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x4f: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGEDOWN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5b: return KEY_LEFTMETA;
    case 0x5c: return KEY_RIGHTMETA;
    case 0x5d: return KEY_COMPOSE;
    case 0x20: return KEY_MUTE;
    case 0x2e: return KEY_VOLUMEDOWN;
    case 0x30: return KEY_VOLUMEUP;
    case 0x10: return KEY_PREVIOUSSONG;
    case 0x19: return KEY_NEXTSONG;
    case 0x22: return KEY_PLAYPAUSE;
    case 0x24: return KEY_STOPCD;
    default: return 0;
    }
}

std::uint16_t WindowsVkToLinuxKey(std::uint16_t vk) noexcept {
    switch (vk) {
    case 0x08: return KEY_BACKSPACE;
    case 0x09: return KEY_TAB;
    case 0x0D: return KEY_ENTER;
    case 0x10:
    case 0xA0: return KEY_LEFTSHIFT;
    case 0xA1: return KEY_RIGHTSHIFT;
    case 0x11:
    case 0xA2: return KEY_LEFTCTRL;
    case 0xA3: return KEY_RIGHTCTRL;
    case 0x12:
    case 0xA4: return KEY_LEFTALT;
    case 0xA5: return KEY_RIGHTALT;
    case 0x1B: return KEY_ESC;
    case 0x20: return KEY_SPACE;
    case 0x21: return KEY_PAGEUP;
    case 0x22: return KEY_PAGEDOWN;
    case 0x23: return KEY_END;
    case 0x24: return KEY_HOME;
    case 0x25: return KEY_LEFT;
    case 0x26: return KEY_UP;
    case 0x27: return KEY_RIGHT;
    case 0x28: return KEY_DOWN;
    case 0x2D: return KEY_INSERT;
    case 0x2E: return KEY_DELETE;
    case 0x5B: return KEY_LEFTMETA;
    case 0x5C: return KEY_RIGHTMETA;
    case 0x30: return KEY_0;
    case 0x31: return KEY_1;
    case 0x32: return KEY_2;
    case 0x33: return KEY_3;
    case 0x34: return KEY_4;
    case 0x35: return KEY_5;
    case 0x36: return KEY_6;
    case 0x37: return KEY_7;
    case 0x38: return KEY_8;
    case 0x39: return KEY_9;
    case 0x41: return KEY_A;
    case 0x42: return KEY_B;
    case 0x43: return KEY_C;
    case 0x44: return KEY_D;
    case 0x45: return KEY_E;
    case 0x46: return KEY_F;
    case 0x47: return KEY_G;
    case 0x48: return KEY_H;
    case 0x49: return KEY_I;
    case 0x4A: return KEY_J;
    case 0x4B: return KEY_K;
    case 0x4C: return KEY_L;
    case 0x4D: return KEY_M;
    case 0x4E: return KEY_N;
    case 0x4F: return KEY_O;
    case 0x50: return KEY_P;
    case 0x51: return KEY_Q;
    case 0x52: return KEY_R;
    case 0x53: return KEY_S;
    case 0x54: return KEY_T;
    case 0x55: return KEY_U;
    case 0x56: return KEY_V;
    case 0x57: return KEY_W;
    case 0x58: return KEY_X;
    case 0x59: return KEY_Y;
    case 0x5A: return KEY_Z;
    case 0x70: return KEY_F1;
    case 0x71: return KEY_F2;
    case 0x72: return KEY_F3;
    case 0x73: return KEY_F4;
    case 0x74: return KEY_F5;
    case 0x75: return KEY_F6;
    case 0x76: return KEY_F7;
    case 0x77: return KEY_F8;
    case 0x78: return KEY_F9;
    case 0x79: return KEY_F10;
    case 0x7A: return KEY_F11;
    case 0x7B: return KEY_F12;
    default: return 0;
    }
}

} // namespace p2premote::linux_host
