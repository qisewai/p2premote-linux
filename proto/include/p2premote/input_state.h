#pragma once

#include <p2premote/input_protocol.h>

#include <bitset>

namespace p2premote {

template<std::size_t KeyCount = 512, std::size_t ButtonCount = 4>
struct PressedInput {
    std::bitset<KeyCount> keys;
    std::bitset<ButtonCount> buttons;

    bool Any() const noexcept { return keys.any() || buttons.any(); }
    bool Dragging() const noexcept { return buttons.any(); }
    void Clear() noexcept {
        keys.reset();
        buttons.reset();
    }

    template<class Release>
    void ReleaseKeys(Release release) { ReleaseHeld(keys, release); }
    template<class Release>
    void ReleaseButtons(Release release) { ReleaseHeld(buttons, release); }

private:
    template<std::size_t Count, class Release>
    static void ReleaseHeld(std::bitset<Count>& held, Release release) {
        // Code zero is reserved; it is never sent as a key or mouse button.
        for (std::size_t code = 1; code < Count; ++code) {
            if (held.test(code) && release(static_cast<std::uint16_t>(code))) held.reset(code);
        }
    }
};

class InputSendState {
public:
    void Reset() noexcept {
        buttons_.reset();
        drag_checkpoint_.reset();
    }

    template<class SendPointer, class SendControl>
    bool Send(const InputPacket& packet, SendPointer pointer, SendControl control) {
        if (packet.kind == InputKind::move) {
            if (buttons_.any()) drag_checkpoint_ = packet;
            return pointer(EncodeInput(packet));
        }
        // Reuse the original sequence so the receiver drops stale or duplicated positions.
        if (packet.kind == InputKind::heartbeat && drag_checkpoint_ && control(EncodeInput(*drag_checkpoint_))) {
            drag_checkpoint_.reset();
        }
        if (!control(EncodeInput(packet))) return false;
        if (packet.kind == InputKind::reset) {
            Reset();
        } else if ((packet.kind == InputKind::button_down || packet.kind == InputKind::button_up) &&
            packet.code < buttons_.size()) {
            buttons_.set(packet.code, packet.kind == InputKind::button_down);
            drag_checkpoint_.reset();
        } else if (packet.kind == InputKind::wheel) {
            drag_checkpoint_.reset();
        }
        return true;
    }

private:
    std::bitset<4> buttons_;
    std::optional<InputPacket> drag_checkpoint_;
};

} // namespace p2premote
