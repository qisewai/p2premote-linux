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

    const InputBytes wire{3, 5, 0x4d, 0x01, 0xff, 0xff, 0x34, 0x12,
        0x78, 0x56, 0x34, 0x12, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    const auto packet = DecodeInput(wire);
    bool passed = check(packet && packet->kind == InputKind::key_down && packet->code == 0x14d
        && packet->x == 65535 && packet->y == 0x1234 && packet->sequence == 0x12345678
        && packet->sent_ms == 0x0102030405060708, "Could not decode little-endian packet with extended scan code");
    passed &= check(packet && EncodeInput(*packet) == wire, "Encoder did not preserve the wire format");
    passed &= check(!DecodeInput(std::span(wire).first(19)), "Accepted a truncated packet");

    auto invalid = wire;
    invalid[0] = 1;
    passed &= check(!DecodeInput(invalid), "Accepted input from the previous protocol");
    passed &= check(!DecodeInput(EncodeInput({static_cast<InputKind>(255), 0, 0, 0, 1, 1})), "Accepted an unknown event");
    passed &= check(!DecodeInput(EncodeInput({InputKind::button_down, 4, 0, 0, 1, 1})), "Accepted an unsupported button");
    passed &= check(!DecodeInput(EncodeInput({InputKind::key_up, 512, 0, 0, 1, 1})), "Accepted an invalid scan code");
    passed &= check(!DecodeInput(EncodeInput({InputKind::move, 0, 0, 0, 0, 1})), "Accepted sequence number zero");
    passed &= check(DecodeInput(EncodeInput({InputKind::wheel, 0xff88, 0, 0, 1, 1})).has_value(), "Rejected negative wheel delta");
    using namespace p2premote;
    constexpr auto hd = StreamQuality::p1080;
    const auto encode_settings = [](InputKind kind, StreamQuality resolution, std::uint32_t bps) {
        return EncodeInput(VideoSettingsPacket(kind, resolution, bps, 27, 50));
    };
    auto request_bytes = encode_settings(InputKind::set_video_settings, hd, 16'000'000);
    const auto request = DecodeInput(request_bytes);
    passed &= check(request && request->kind == InputKind::set_video_settings
        && request->code == static_cast<std::uint16_t>(hd) && request->sequence == 27
        && VideoSettingsBitrate(*request) == 16'000'000, "Could not decode the exact requested bitrate");
    request_bytes[0] = 1;
    passed &= check(!DecodeInput(request_bytes), "Accepted a video request from the previous protocol");
    const auto reply = DecodeInput(encode_settings(InputKind::video_settings_reply, hd, 7'850'000));
    passed &= check(reply && reply->kind == InputKind::video_settings_reply && reply->sequence == 27
        && VideoSettingsBitrate(*reply) == 7'850'000, "Could not decode the independent applied bitrate");
    const auto rejected = DecodeInput(encode_settings(InputKind::video_settings_reply, hd, 0));
    passed &= check(rejected && VideoSettingsBitrate(*rejected) == 0, "Could not decode a video settings rejection");
    passed &= check(!DecodeInput(encode_settings(InputKind::set_video_settings, static_cast<StreamQuality>(0xffff), 2'000'000))
        && !DecodeInput(encode_settings(InputKind::set_video_settings, hd, 0))
        && !DecodeInput(encode_settings(InputKind::set_video_settings, hd, 1'999'999))
        && !DecodeInput(encode_settings(InputKind::video_settings_reply, hd, 16'000'001)),
        "Accepted malformed video settings");
    const std::uint16_t audio_flags = p2premote::audio_flag_speaker | p2premote::audio_flag_mic;
    const auto audio_req = DecodeInput(EncodeInput({InputKind::set_audio_settings, audio_flags, 0, 0, 28, 55}));
    passed &= check(audio_req && audio_req->kind == InputKind::set_audio_settings && audio_req->code == audio_flags
        && audio_req->x == 0 && audio_req->y == 0, "Could not decode valid audio settings");
    passed &= check(!DecodeInput(EncodeInput({InputKind::set_audio_settings, 0x08, 0, 0, 28, 55}))
        && !DecodeInput(EncodeInput({InputKind::set_audio_settings, audio_flags, 1, 0, 28, 55})),
        "Accepted malformed audio settings");

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

    const InputCommand chord{InputCommandKind::shortcut, {0xa2, 0xa0, 0x1b}, 73};
    const auto chord_bytes = EncodeInputCommand(chord);
    const auto decoded_chord = DecodeInputCommand(chord_bytes);
    passed &= check(decoded_chord && decoded_chord->kind == chord.kind && decoded_chord->units == chord.units
        && decoded_chord->sent_ms == 73 && !DecodeInput(chord_bytes), "Keyboard chord lost order or decoded as a fixed input packet");
    auto old_chord = chord_bytes;
    old_chord[0] = 2;
    passed &= check(!DecodeInputCommand(old_chord), "Accepted a keyboard command from the previous protocol");
    passed &= check(!DecodeInputCommand(std::span(chord_bytes).first(chord_bytes.size() - 1))
        && EncodeInputCommand({InputCommandKind::shortcut, {0x1d, 0x1d}, 0}).empty()
        && EncodeInputCommand({InputCommandKind::shortcut, {512}, 0}).empty(), "Accepted an invalid key sequence");
    const InputCommand text{InputCommandKind::text, {0x6d4b, '\t', 0xd83d, 0xde00, '\r', '\n'}, 74};
    const auto decoded_text = DecodeInputCommand(EncodeInputCommand(text));
    passed &= check(decoded_text && decoded_text->units == text.units, "Unicode text did not survive encoding");
    passed &= check(EncodeInputCommand({InputCommandKind::text, {0xd83d}, 0}).empty()
        && EncodeInputCommand({InputCommandKind::text, {0xde00}, 0}).empty()
        && EncodeInputCommand({InputCommandKind::text, {0}, 0}).empty()
        && EncodeInputCommand({InputCommandKind::text, std::vector<std::uint16_t>(input_text_limit + 1, 'x'), 0}).empty(),
        "Accepted malformed or oversized Unicode text");
    passed &= check(!input.AcceptCommand(text, 2300), "Accepted stale macro text");
    return passed ? 0 : 1;
}
