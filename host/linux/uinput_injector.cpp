#include "uinput_injector.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace p2premote::linux_host {
namespace {

std::uint16_t ButtonToLinuxCode(std::uint16_t button) noexcept {
    switch (static_cast<InputButton>(button)) {
    case InputButton::left: return BTN_LEFT;
    case InputButton::right: return BTN_RIGHT;
    case InputButton::middle: return BTN_MIDDLE;
    }
    return 0;
}

} // namespace

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

UinputInjector::UinputInjector() {
    CreateKeyboard();
    CreateMouse();
}

UinputInjector::~UinputInjector() {
    ReleaseAll();
    if (kbd_fd_ >= 0) {
        ioctl(kbd_fd_, UI_DEV_DESTROY);
        close(kbd_fd_);
    }
    if (mouse_fd_ >= 0) {
        ioctl(mouse_fd_, UI_DEV_DESTROY);
        close(mouse_fd_);
    }
}

bool UinputInjector::CreateKeyboard() {
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        close(fd);
        return false;
    }
    for (int key = 1; key <= 248; ++key) {
        ioctl(fd, UI_SET_KEYBIT, key);
    }

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x0246;
    setup.id.version = 1;
    std::strncpy(setup.name, "P2P Remote Virtual Keyboard", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return false;
    }
    kbd_fd_ = fd;
    return true;
}

bool UinputInjector::CreateMouse() {
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        close(fd);
        return false;
    }
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    uinput_abs_setup abs_x{};
    abs_x.code = ABS_X;
    abs_x.absinfo.minimum = 0;
    abs_x.absinfo.maximum = 65535;
    uinput_abs_setup abs_y{};
    abs_y.code = ABS_Y;
    abs_y.absinfo.minimum = 0;
    abs_y.absinfo.maximum = 65535;
    if (ioctl(fd, UI_ABS_SETUP, &abs_x) < 0 || ioctl(fd, UI_ABS_SETUP, &abs_y) < 0) {
        close(fd);
        return false;
    }

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x0247;
    setup.id.version = 1;
    std::strncpy(setup.name, "P2P Remote Virtual Mouse", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return false;
    }
    mouse_fd_ = fd;
    return true;
}

void UinputInjector::Emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) noexcept {
    if (fd < 0) return;
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    [[maybe_unused]] const auto written = write(fd, &ev, sizeof(ev));
}

void UinputInjector::Sync(int fd) noexcept {
    Emit(fd, EV_SYN, SYN_REPORT, 0);
}

void UinputInjector::WakeDisplay() noexcept {
    if (kbd_fd_ < 0) return;
    Emit(kbd_fd_, EV_KEY, KEY_LEFTSHIFT, 1);
    Sync(kbd_fd_);
    Emit(kbd_fd_, EV_KEY, KEY_LEFTSHIFT, 0);
    Sync(kbd_fd_);
}

void UinputInjector::ReleaseAll() noexcept {
    if (mouse_fd_ >= 0 && !pressed_buttons_.empty()) {
        for (const auto btn : pressed_buttons_) {
            Emit(mouse_fd_, EV_KEY, btn, 0);
        }
        Sync(mouse_fd_);
        pressed_buttons_.clear();
    }
    if (kbd_fd_ >= 0 && !pressed_keys_.empty()) {
        for (const auto key : pressed_keys_) {
            Emit(kbd_fd_, EV_KEY, key, 0);
        }
        Sync(kbd_fd_);
        pressed_keys_.clear();
    }
    wheel_accum_ = 0;
}

void UinputInjector::Apply(const InputPacket& packet) noexcept {
    switch (packet.kind) {
    case InputKind::move:
        if (mouse_fd_ >= 0) {
            Emit(mouse_fd_, EV_ABS, ABS_X, static_cast<std::int32_t>(packet.x));
            Emit(mouse_fd_, EV_ABS, ABS_Y, static_cast<std::int32_t>(packet.y));
            Sync(mouse_fd_);
        }
        break;
    case InputKind::button_down:
    case InputKind::button_up:
        if (mouse_fd_ >= 0) {
            const auto btn = ButtonToLinuxCode(packet.code);
            if (!btn) break;
            const bool down = packet.kind == InputKind::button_down;
            Emit(mouse_fd_, EV_ABS, ABS_X, static_cast<std::int32_t>(packet.x));
            Emit(mouse_fd_, EV_ABS, ABS_Y, static_cast<std::int32_t>(packet.y));
            Emit(mouse_fd_, EV_KEY, btn, down ? 1 : 0);
            Sync(mouse_fd_);
            if (down) pressed_buttons_.insert(btn);
            else pressed_buttons_.erase(btn);
        }
        break;
    case InputKind::wheel:
        if (mouse_fd_ >= 0) {
            const auto delta = static_cast<std::int16_t>(packet.code);
            wheel_accum_ += delta;
            const int clicks = wheel_accum_ / 120;
            if (clicks != 0) {
                wheel_accum_ -= clicks * 120;
                Emit(mouse_fd_, EV_REL, REL_WHEEL, clicks);
                Sync(mouse_fd_);
            }
        }
        break;
    case InputKind::key_down:
    case InputKind::key_up:
        if (kbd_fd_ >= 0) {
            const auto key = Ps2ScancodeToLinuxKey(packet.code);
            if (!key) break;
            const bool down = packet.kind == InputKind::key_down;
            Emit(kbd_fd_, EV_KEY, key, down ? 1 : 0);
            Sync(kbd_fd_);
            if (down) pressed_keys_.insert(key);
            else pressed_keys_.erase(key);
        }
        break;
    case InputKind::reset:
        ReleaseAll();
        break;
    default:
        break;
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

void UinputInjector::ApplyCommand(const InputCommand& command) noexcept {
    if (kbd_fd_ < 0 || command.units.empty()) return;
    if (command.kind == InputCommandKind::shortcut) {
        std::uint16_t keys[shortcut_limit]{};
        std::size_t count = 0;
        for (const auto vk : command.units) {
            if (count >= shortcut_limit) break;
            const auto key = WindowsVkToLinuxKey(vk);
            if (!key) return;
            keys[count++] = key;
        }
        for (std::size_t i = 0; i < count; ++i) {
            Emit(kbd_fd_, EV_KEY, keys[i], 1);
            Sync(kbd_fd_);
        }
        for (std::size_t i = count; i > 0; --i) {
            Emit(kbd_fd_, EV_KEY, keys[i - 1], 0);
            Sync(kbd_fd_);
        }
    } else if (command.kind == InputCommandKind::text) {
        for (const auto unit : command.units) {
            bool shift = false;
            std::uint16_t key = 0;
            if (unit >= 'a' && unit <= 'z') {
                key = WindowsVkToLinuxKey(static_cast<std::uint16_t>('A' + (unit - 'a')));
            } else if (unit >= 'A' && unit <= 'Z') {
                key = WindowsVkToLinuxKey(unit);
                shift = true;
            } else if (unit >= '0' && unit <= '9') {
                key = WindowsVkToLinuxKey(unit);
            } else if (unit == ' ') {
                key = KEY_SPACE;
            } else if (unit == '\n' || unit == '\r') {
                key = KEY_ENTER;
            } else if (unit == '\t') {
                key = KEY_TAB;
            } else if (unit == '-') {
                key = KEY_MINUS;
            } else if (unit == '_') {
                key = KEY_MINUS;
                shift = true;
            } else if (unit == '.') {
                key = KEY_DOT;
            } else if (unit == ',') {
                key = KEY_COMMA;
            } else if (unit == '/') {
                key = KEY_SLASH;
            } else if (unit == ':') {
                key = KEY_SEMICOLON;
                shift = true;
            }
            if (!key) continue;
            if (shift) { Emit(kbd_fd_, EV_KEY, KEY_LEFTSHIFT, 1); Sync(kbd_fd_); }
            Emit(kbd_fd_, EV_KEY, key, 1);
            Sync(kbd_fd_);
            Emit(kbd_fd_, EV_KEY, key, 0);
            Sync(kbd_fd_);
            if (shift) { Emit(kbd_fd_, EV_KEY, KEY_LEFTSHIFT, 0); Sync(kbd_fd_); }
        }
    }
}

} // namespace p2premote::linux_host
