#include <p2premote/input_protocol.h>
#include <p2premote/input_state.h>
#include <p2premote/session_policy.h>

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

    ReconnectWindow reconnect(100);
    passed &= check(reconnect.Schedule(110) == 1 && !reconnect.Due(1109) && reconnect.Due(1110),
        "Initial reconnect delay was not one second");
    passed &= check(reconnect.BeginAttempt() == 1 && reconnect.Schedule(1110) == 2 &&
        reconnect.SecondsRemaining(2111) == 1 && reconnect.Due(3110), "Second reconnect delay or countdown was wrong");
    reconnect.BeginAttempt();
    passed &= check(reconnect.Schedule(3110) == 4 && reconnect.Expired(30100) && !reconnect.Due(30100),
        "Reconnect did not honor the capped delay and deadline");
    passed &= check(ClassifySignalTermination("ended", "", true, false) == SignalTermination::reconnect &&
        ClassifySignalTermination("error", "设备不可连接", false, true) == SignalTermination::reconnect &&
        ClassifySignalTermination("error", "连接验证码错误", false, true) == SignalTermination::stop &&
        ClassifySignalTermination("error", "", true, false) == SignalTermination::stop &&
        ParseSessionMode("view") == SessionMode::view && ParseSessionMode("invalid") == SessionMode::invalid,
        "Misclassified a signaling result");
    passed &= check(ClassifyViewerSignal(ViewerSignalStage::challenge, "accepted") == ViewerSignal::unexpected &&
        ClassifyViewerSignal(ViewerSignalStage::authentication, "accepted") == ViewerSignal::accepted &&
        ClassifyViewerSignal(ViewerSignalStage::approval, "waiting") == ViewerSignal::waiting &&
        ClassifyViewerSignal(ViewerSignalStage::approval, "minirtc") == ViewerSignal::unexpected &&
        ClassifyViewerSignal(ViewerSignalStage::media, "minirtc") == ViewerSignal::minirtc &&
        ClassifyLanMedia("offer", true) == LanMediaSignal::description &&
        ClassifyLanMedia("offer", false) == LanMediaSignal::invalid &&
        ClassifyLanMedia("new_candidate_mid", false) == LanMediaSignal::candidate_mid,
        "Accepted signaling outside the correct phase or direction");

    const std::string host_nonce(64, 'a'), viewer_nonce(64, 'b');
    const auto fake_mac = [&](std::string_view bytes) {
        return std::string(64, bytes == host_nonce + ":" + viewer_nonce + ":viewer" ? 'c' : 'd');
    };
    const auto compare = [](std::string_view lhs, std::string_view rhs) { return lhs == rhs; };
    const auto proof = fake_mac(LanProofMessage(host_nonce, viewer_nonce, LanProofRole::viewer));
    passed &= check(IsLanNonce(host_nonce) && !IsLanNonce("short") &&
        VerifyLanProof(host_nonce, viewer_nonce, LanProofRole::viewer, proof, fake_mac, compare) &&
        !VerifyLanProof(host_nonce, viewer_nonce, LanProofRole::host, proof, fake_mac, compare) &&
        !VerifyLanProof("", viewer_nonce, LanProofRole::viewer, proof, fake_mac, compare),
        "LAN challenge did not bind the nonce order and proof to the peer role");

    VideoSettingsNegotiation settings{{1'500, 3'000, 2}};
    std::vector<InputPacket> sends;
    const auto send = [&](const InputPacket& packet) { sends.push_back(packet); return true; };
    std::uint32_t sequence{};
    const VideoSettings desired{hd, 8'000'000};
    settings.Update(desired, 100, sequence, send);
    settings.Update(desired, 1599, sequence, send);
    passed &= check(sends.size() == 1 && sends.front().sequence == 1, "Settings retransmitted early");
    settings.Update(desired, 1600, sequence, send);
    passed &= check(sends.size() == 2 && sends.back().sequence == 1, "Settings retry replaced the request sequence");
    passed &= check(!settings.Acknowledge(VideoSettingsPacket(InputKind::video_settings_reply, StreamQuality::p720,
        8'000'000, 1, 100)), "Accepted a reply for a different resolution");
    const auto confirmed = settings.Acknowledge(VideoSettingsPacket(InputKind::video_settings_reply, hd, 7'850'000, 1, 100));
    passed &= check(confirmed && confirmed->settings == desired && confirmed->applied_bitrate == 7'850'000,
        "Settings acknowledgement lost the selected or applied bitrate");
    settings.Update(desired, 1700, sequence, send);
    passed &= check(sends.size() == 2, "Acknowledged settings were sent again");
    const VideoSettings changed{StreamQuality::p720, 2'000'000};
    settings.Update(changed, 1700, sequence, send);
    passed &= check(settings.Update(changed, 4700, sequence, send) && sends.size() == 3,
        "Unacknowledged settings did not settle at their timeout");

    InputSendState pointer;
    std::vector<InputPacket> reliable;
    const auto unreliable = [](const InputBytes&) { return false; };
    const auto control = [&](const InputBytes& bytes) { reliable.push_back(*DecodeInput(bytes)); return true; };
    pointer.Send({InputKind::button_down, 1, 0, 0, 1, 100}, unreliable, control);
    pointer.Send({InputKind::move, 0, 10, 5, 2, 103}, unreliable, control);
    pointer.Send({InputKind::heartbeat, 0, 0, 0, 3, 108}, unreliable, control);
    pointer.Send({InputKind::move, 0, 12, 8, 4, 111}, unreliable, control);
    pointer.Send({InputKind::reset, 0, 0, 0, 5, 120}, unreliable, control);
    pointer.Send({InputKind::heartbeat, 0, 0, 0, 6, 130}, unreliable, control);
    passed &= check(reliable.size() == 5 && reliable[1].kind == InputKind::move && reliable[1].sequence == 2 &&
        reliable[3].kind == InputKind::reset && reliable[4].sequence == 6,
        "Drag recovery lost the final pointer sequence or resent it after a reset");
    PressedInput<> held;
    held.keys.set(52);
    held.buttons.set(1);
    held.ReleaseKeys([](std::uint16_t) { return false; });
    held.ReleaseButtons([](std::uint16_t code) { return code == 1; });
    passed &= check(held.Any() && !held.Dragging() && held.keys.test(52), "Failed key release was lost");
    held.ReleaseKeys([](std::uint16_t code) { return code == 52; });
    passed &= check(!held.Any(), "Key remained held after release");
    return passed ? 0 : 1;
}
