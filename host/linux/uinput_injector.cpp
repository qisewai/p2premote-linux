#include "uinput_injector.h"
#include "linux_key_map.h"

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
    if (mouse_fd_ >= 0 && pressed_.Dragging()) {
        pressed_.ReleaseButtons([this](std::uint16_t button) {
            Emit(mouse_fd_, EV_KEY, button, 0);
            return true;
        });
        Sync(mouse_fd_);
    }
    if (kbd_fd_ >= 0 && pressed_.keys.any()) {
        pressed_.ReleaseKeys([this](std::uint16_t key) {
            Emit(kbd_fd_, EV_KEY, key, 0);
            return true;
        });
        Sync(kbd_fd_);
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
            pressed_.buttons.set(btn, down);
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
            pressed_.keys.set(key, down);
        }
        break;
    case InputKind::reset:
        ReleaseAll();
        break;
    default:
        break;
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
