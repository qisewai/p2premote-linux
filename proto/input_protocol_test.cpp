#include <p2premote/input_protocol.h>

#include <iostream>

using p2premote::DecodeInput;
using p2premote::EncodeInput;
using p2premote::InputBytes;
using p2premote::InputKind;
using p2premote::InputReceiveClock;

int main() {
    const auto check = [](bool passed, const char* description) {
        if (!passed) std::cerr << description << '\n';
        return passed;
    };

    const InputBytes wire{1, 5, 0x4d, 0x01, 0xff, 0xff, 0x34, 0x12,
        0x78, 0x56, 0x34, 0x12, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    const auto packet = DecodeInput(wire);
    bool passed = check(packet && packet->kind == InputKind::key_down && packet->code == 0x14d
        && packet->x == 65535 && packet->y == 0x1234 && packet->sequence == 0x12345678
        && packet->sent_ms == 0x0102030405060708, "Could not decode little-endian packet with extended scan code");
    passed &= check(packet && EncodeInput(*packet) == wire, "Encoder did not preserve the wire format");
    passed &= check(!DecodeInput(std::span(wire).first(19)), "Accepted a truncated packet");

    auto invalid = wire;
    invalid[0] = 2;
    passed &= check(!DecodeInput(invalid), "Accepted an unsupported version");
    passed &= check(!DecodeInput(EncodeInput({static_cast<InputKind>(255), 0, 0, 0, 1, 1})), "Accepted an unknown event");
    passed &= check(!DecodeInput(EncodeInput({InputKind::button_down, 4, 0, 0, 1, 1})), "Accepted an unsupported button");
    passed &= check(!DecodeInput(EncodeInput({InputKind::key_up, 512, 0, 0, 1, 1})), "Accepted an invalid scan code");
    passed &= check(!DecodeInput(EncodeInput({InputKind::move, 0, 0, 0, 0, 1})), "Accepted sequence number zero");
    passed &= check(DecodeInput(EncodeInput({InputKind::wheel, 0xff88, 0, 0, 1, 1})).has_value(), "Rejected negative wheel delta");
    passed &= check(DecodeInput(EncodeInput({InputKind::text, u'字', 0, 0, 1, 1})).has_value(), "Rejected UTF-16 character");
    const auto hd = static_cast<std::uint16_t>(p2premote::StreamQuality::p1080);
    const auto economy = static_cast<std::uint16_t>(p2premote::VideoBitratePreset::economy);
    const auto request = DecodeInput(EncodeInput({InputKind::set_video_settings, hd, economy, 0, 27, 50}));
    passed &= check(request && request->kind == InputKind::set_video_settings && request->code == hd
        && request->x == economy && request->y == 0 && request->sequence == 27,
        "Could not request 1080p at the economy bitrate independently");
    const auto reply = DecodeInput(EncodeInput({InputKind::video_settings_reply, hd, economy, 1, 27, 50}));
    passed &= check(reply && reply->kind == InputKind::video_settings_reply && reply->sequence == 27 && reply->y == 1,
        "Could not decode a correlated video settings rejection");
    passed &= check(!DecodeInput(EncodeInput({InputKind::set_video_settings, 0xffff, economy, 0, 27, 50}))
        && !DecodeInput(EncodeInput({InputKind::set_video_settings, hd, 0, 0, 27, 50}))
        && !DecodeInput(EncodeInput({InputKind::set_video_settings, hd, economy, 1, 27, 50}))
        && !DecodeInput(EncodeInput({InputKind::video_settings_reply, hd, economy, 2, 27, 50})),
        "Accepted malformed video settings");

    InputReceiveClock clock;
    passed &= check(clock.IsFresh(900'000, 2'010), "Rejected input from a peer with longer uptime");
    passed &= check(clock.IsFresh(900'150, 2'155), "Rejected a fresher clock sample");
    passed &= check(!clock.IsFresh(900'200, 4'206), "Accepted input delayed more than two seconds");
    passed &= check(clock.IsFresh(902'210, 4'215), "A stale packet changed the clock baseline");
    InputReceiveClock reverse_clock;
    passed &= check(reverse_clock.IsFresh(2'000, 900'005), "Rejected input from a peer with shorter uptime");
    passed &= check(!reverse_clock.IsFresh(~std::uint64_t{}, 900'006), "Accepted an out-of-range clock value");

    p2premote::InputReceiveState input;
    passed &= check(input.Accept({InputKind::wheel, 120, 0, 0, 2, 50}, 200).has_value(), "Rejected a fresh wheel event");
    passed &= check(!input.Accept({InputKind::move, 0, 0, 0, 1, 50}, 201), "Old motion could reverse the wheel position");
    passed &= check(input.Accept({InputKind::move, 0, 1, 1, 3, 60}, 210).has_value(), "Rejected motion after the wheel event");
    const auto stale_button = input.Accept({InputKind::button_up, 1, 1, 1, 4, 70}, 2300);
    passed &= check(stale_button && stale_button->kind == InputKind::reset, "Stale button release did not clear held input");
    return passed ? 0 : 1;
}
