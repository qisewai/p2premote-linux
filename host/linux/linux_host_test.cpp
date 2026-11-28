#include "lan_signal_server.h"
#include "linux_key_map.h"
#include "latest_frame_slot.h"
#include "video_frame_cadence.h"
#include "cursor_packet.h"
#include "desktop_access.h"
#include "host_session.h"

#include <linux/input-event-codes.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>
#include <vector>

namespace {

void Check(bool condition, std::source_location source = std::source_location::current()) {
    if (!condition) {
        std::cerr << source.file_name() << ':' << source.line() << ": check failed\n";
        std::exit(1);
    }
}

void CheckListener() {
    using p2premote::linux_host::LanSignalServer;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto occupied = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Check(occupied >= 0 && bind(occupied, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    Check(listen(occupied, 1) == 0);
    socklen_t size = sizeof(address);
    Check(getsockname(occupied, reinterpret_cast<sockaddr*>(&address), &size) == 0);

    LanSignalServer server("Loopback test", ntohs(address.sin_port), "test-access-code", {}, 0);
    Check(!server.Start());
    close(occupied);
    Check(server.Start());

    const auto next_client = [&] {
        const auto fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        Check(fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        pollfd ready{fd, POLLIN, 0};
        Check(poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN));
        return fd;
    };
    const auto first = next_client();
    const auto second = next_client();
    close(second);
    close(first);
    server.Stop();
}

void CheckVideoCadence() {
    using namespace p2premote::linux_host;
    using namespace std::chrono_literals;
    VideoFrameCadence cadence;
    auto now = VideoFrameCadence::Clock::time_point{};
    std::uint8_t pixels[16]{};
    const VideoFrameView frame{2, 2, VideoPixelFormat::bgra32, pixels, 8, nullptr, 0, 0, 0, {}};
    Check(cadence.Next(frame, 2, 2, now) == FrameSubmission::convert);
    // Retry a prepared frame immediately if send_video did not accept it.
    Check(cadence.Next(frame, 2, 2, now) == FrameSubmission::repeat);
    cadence.Submitted(now);
    for (int i = 0; i < 4; ++i) {
        Check(cadence.Next(frame, 2, 2, now + 66ms) == FrameSubmission::skip);
        now += 67ms;
        Check(cadence.Next(frame, 2, 2, now) == FrameSubmission::repeat);
        cadence.Submitted(now);
    }
    Check(cadence.Next(frame, 2, 2, now + 899ms) == FrameSubmission::skip);
    now += 900ms;
    Check(cadence.Next(frame, 2, 2, now) == FrameSubmission::repeat);
    cadence.Submitted(now);
    pixels[4] = 255;
    Check(cadence.Next(frame, 2, 2, now + 33ms) == FrameSubmission::convert);
    cadence.Submitted(now + 33ms);
    Check(cadence.Next(frame, 4, 4, now + 66ms) == FrameSubmission::convert);
    cadence.Submitted(now + 66ms);
    cadence.Reset();
    Check(cadence.Next(frame, 4, 4, now + 67ms) == FrameSubmission::convert);
}

void CheckFrameFormatsAndActivity() {
    using namespace p2premote::linux_host;
    using namespace std::chrono_literals;
    LatestFrameSlot slot;
    Check(!slot.Wait(1ms));
    const auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        std::initializer_list<std::uint8_t>{1, 2, 99, 99, 3, 4, 99, 99, 5, 6});
    VideoFrameView frame{2, 2, VideoPixelFormat::nv12, pixels->data(), 4,
        pixels->data() + 8, 2, 1, 0, pixels};
    slot.Push(frame);
    frame.sequence++;
    slot.Push(frame);
    Check(slot.Wait(50ms));
    const auto latest = slot.Acquire();
    Check(latest && latest->sequence == 2 && !slot.Wait(1ms));
    slot.Clear();
    Check(slot.Wait(50ms) && !slot.Acquire());
    Check(latest->plane0[4] == 3);

    Nv12FrameBuffer converted;
    Check(converted.Convert(*latest, 2, 2));
    const std::vector<std::uint8_t> expected{1, 2, 3, 4, 5, 6};
    Check(std::ranges::equal(converted.Pixels(), expected));
    Check(converted.Convert(*latest, 4, 4) && converted.Pixels().size() == 24);
    auto invalid = *latest;
    invalid.stride1 = 1;
    Check(!converted.Convert(invalid, 2, 2));
    std::array<std::uint8_t, 16> bgrx{0, 0, 255, 0, 0, 0, 255, 1, 0, 0, 255, 2, 0, 0, 255, 3};
    const VideoFrameView packed{2, 2, VideoPixelFormat::bgrx32, bgrx.data(), 8, nullptr, 0, 0, 0, {}};
    Check(converted.Convert(packed, 4, 4) && converted.Pixels().size() == 24);
    const std::vector<std::uint8_t> scaled(converted.Pixels().begin(), converted.Pixels().end());
    auto bgra = packed;
    bgra.format = VideoPixelFormat::bgra32;
    Check(converted.Convert(bgra, 4, 4) && std::ranges::equal(converted.Pixels(), scaled));

    VideoFrameCadence cadence;
    const auto now = VideoFrameCadence::Clock::now();
    Check(cadence.Next(*latest, 2, 2, now) == FrameSubmission::convert);
    cadence.Submitted(now);
    (*pixels)[2] = 42; // Padded bytes do not affect visual cadence.
    Check(cadence.Next(*latest, 2, 2, now) == FrameSubmission::skip);
    (*pixels)[9] = 42; // Chroma is part of NV12 frame identity.
    Check(cadence.Next(*latest, 2, 2, now) == FrameSubmission::convert);
    Check(cadence.Next(packed, 2, 2, now) == FrameSubmission::convert);
    cadence.Submitted(now);
    bgrx[3] = 255;
    Check(cadence.Next(packed, 2, 2, now) == FrameSubmission::skip);
    bgrx[2] = 120;
    Check(cadence.Next(packed, 2, 2, now) == FrameSubmission::convert);
    Check(WaitingDesktopStatus(DesktopSessionState::waiting_permission) == "permission");
    Check(WaitingDesktopStatus(DesktopSessionState::revoked) == "revoked");

    std::stop_source stopping;
    std::jthread stopper([&] { stopping.request_stop(); });
    Check(!slot.Wait(1s, stopping.get_token()));
    slot.Clear();
    Check(!slot.Acquire());
    std::jthread source([&] { slot.Push(frame); });
    Check(slot.Wait(1s));
    const auto pushed = slot.Acquire();
    Check(pushed && pushed->sequence == frame.sequence);
}

class MockInputSink final : public p2premote::linux_host::InputSink {
public:
    bool IsAvailable() const noexcept override { return available; }
    void WakeDisplay() noexcept override { ++wakes; }
    void Apply(const p2premote::InputPacket&) noexcept override { ++packets; }
    void ApplyCommand(const p2premote::InputCommand&) noexcept override { ++commands; }
    void ReleaseAll() noexcept override { ++releases; }
    std::string Describe() const override { return "mock input"; }

    bool available{};
    int wakes{}, packets{}, commands{}, releases{};
};

class MockDesktopSession final : public p2premote::linux_host::DesktopSession {
public:
    using StateType = p2premote::linux_host::DesktopSessionState;
    using Frame = p2premote::linux_host::VideoFrameView;

    bool Start() override { state = StateType::waiting_permission; return true; }
    void Stop() noexcept override { state = StateType::idle; }
    bool WaitForActivity(std::chrono::microseconds timeout, std::stop_token stop) override {
        return frames.Wait(timeout, stop);
    }
    std::optional<Frame> AcquireLatestFrame() override { return frames.Acquire(); }
    bool IsFrameCurrent(const Frame& frame) const override {
        return state == StateType::ready && frame.desktop_epoch == target.epoch;
    }
    std::optional<p2premote::linux_host::CursorUpdate> PollCursor(bool) override { return {}; }
    void MarkFrameSent() override {}
    StateType State() const override { return state; }
    p2premote::linux_host::DesktopTarget Target() const override { return target; }
    p2premote::linux_host::DesktopCapabilities Capabilities() const override {
        return {true, input.available, false, !target.session_id.empty()};
    }
    p2premote::linux_host::InputSink* Input() noexcept override { return &input; }
    std::string Describe() const override { return "mock desktop"; }

    StateType state{};
    p2premote::linux_host::DesktopTarget target;
    p2premote::linux_host::LatestFrameSlot frames;
    MockInputSink input;
};

void CheckDesktopAccess() {
    using namespace p2premote;
    using namespace p2premote::linux_host;
    MockDesktopSession desktop;
    DesktopAccess access;
    Check(desktop.Start() && desktop.State() == DesktopSessionState::waiting_permission);
    access.Attach(desktop.Input());
    Check(desktop.Capabilities().capture_video && !access.CanInject());
    Check(!access.Matches(desktop.State(), desktop.Target()) && !access.UserTarget());
    desktop.target = {DisplayServer::x11, 1000, "3", 2, "/run/user/1000", 1, true};
    desktop.state = DesktopSessionState::ready;
    Check(access.Activate(desktop.Target()) && access.UserTarget() == desktop.Target());
    access.Apply(InputPacket{InputKind::button_down, 1, 0, 0, 1, 100}, 100);
    Check(desktop.input.packets == 0 && desktop.input.wakes == 0);
    desktop.input.available = true;
    access.Apply(InputPacket{InputKind::button_down, 1, 0, 0, 1, 101}, 101);
    access.Apply(InputCommand{InputCommandKind::shortcut, {0x1b}, 102}, 102);
    Check(desktop.input.packets == 1 && desktop.input.commands == 1);

    MockDesktopSession::Frame old_frame{};
    old_frame.desktop_epoch = desktop.target.epoch;
    const auto releases = desktop.input.releases;
    desktop.target.epoch++;
    Check(!desktop.IsFrameCurrent(old_frame) && access.Recheck(desktop.State(), desktop.Target(), 103));
    Check(desktop.input.releases == releases + 1 && !access.UserTarget());
    Check(access.Activate(desktop.Target()) && desktop.input.wakes == 1);
    access.Apply(InputPacket{InputKind::key_down, 30, 0, 0, 1, 104}, 104);
    Check(!access.Recheck(desktop.State(), desktop.Target(), 855) && desktop.input.releases == releases + 3);

    desktop.state = DesktopSessionState::revoked;
    Check(access.Recheck(desktop.State(), desktop.Target(), 856) && !access.UserTarget());
    desktop.target.session_id.clear(); // Greeter remains controllable, with no user audio or clipboard.
    desktop.state = DesktopSessionState::ready;
    Check(access.Activate(desktop.Target()) && access.CanInject() && !access.UserTarget());
    desktop.Stop();
}

void CheckMockHostSession() {
    using namespace p2premote::linux_host;
    auto backend = std::make_unique<MockDesktopSession>();
    const auto* desktop = backend.get();
    LinuxHostSession::CloudCallbacks unused{
        [](std::string, std::uint64_t) {}, [](std::uint64_t) {}, [](std::string_view) {}};
    LinuxHostSession host(HostConfig{}, std::move(backend), unused);
    Check(host.Valid() && !host.SupportsControl() && !host.PeerActive());
    Check(desktop->State() == DesktopSessionState::waiting_permission);
}

} // namespace

int main() {
    using namespace p2premote::linux_host;
    CheckVideoCadence();
    CheckFrameFormatsAndActivity();
    CheckDesktopAccess();
    CheckMockHostSession();

    // 1. Verify PS/2 Set 1 scancode mapping (standard + extended keys)
    Check(Ps2ScancodeToLinuxKey(0x01) == KEY_ESC);
    Check(Ps2ScancodeToLinuxKey(0x1c) == KEY_ENTER);
    Check(Ps2ScancodeToLinuxKey(0x1e) == KEY_A);
    Check(Ps2ScancodeToLinuxKey(0x39) == KEY_SPACE);
    Check(Ps2ScancodeToLinuxKey(0x148) == KEY_UP);
    Check(Ps2ScancodeToLinuxKey(0x150) == KEY_DOWN);
    Check(Ps2ScancodeToLinuxKey(0x14b) == KEY_LEFT);
    Check(Ps2ScancodeToLinuxKey(0x14d) == KEY_RIGHT);
    Check(Ps2ScancodeToLinuxKey(0x153) == KEY_DELETE);
    Check(Ps2ScancodeToLinuxKey(0x15b) == KEY_LEFTMETA);
    Check(Ps2ScancodeToLinuxKey(0x11d) == KEY_RIGHTCTRL);
    Check(Ps2ScancodeToLinuxKey(0x138) == KEY_RIGHTALT);
    Check(Ps2ScancodeToLinuxKey(0x00) == 0);

    // 1b. Verify Windows VK -> Linux KEY mapping for shortcut-macros InputCommand
    Check(WindowsVkToLinuxKey(0x11) == KEY_LEFTCTRL);
    Check(WindowsVkToLinuxKey(0x12) == KEY_LEFTALT);
    Check(WindowsVkToLinuxKey(0x10) == KEY_LEFTSHIFT);
    Check(WindowsVkToLinuxKey(0x5B) == KEY_LEFTMETA);
    Check(WindowsVkToLinuxKey(0x09) == KEY_TAB);
    Check(WindowsVkToLinuxKey(0x1B) == KEY_ESC);
    Check(WindowsVkToLinuxKey(0x2E) == KEY_DELETE);
    Check(WindowsVkToLinuxKey(0x4C) == KEY_L);
    Check(WindowsVkToLinuxKey(0x00) == 0);

    // 2. Verify JSON extraction & MiniRTC signaling relay rewriting
    const std::string nested = "{\"type\":\"minirtc\",\"payload\":\"{\\\"type\\\":\\\"join_transmission\\\"}\"}";
    Check(ExtractJsonString(nested, "type") == "minirtc");
    const auto inner = ExtractJsonString(nested, "payload");
    Check(ExtractJsonString(inner, "type") == "join_transmission");

    const auto relayed_join = RelayMiniRtcJson(inner, false);
    Check(ExtractJsonString(relayed_join, "type") == "user_join_transmission");
    Check(ExtractJsonString(relayed_join, "user_id") == "viewer");

    const auto relayed_offer = RelayMiniRtcJson("{\"type\":\"offer\",\"sdp\":\"v=0\\r\\n\"}", true);
    Check(ExtractJsonString(relayed_offer, "type") == "offer");
    Check(ExtractJsonString(relayed_offer, "remote_user_id") == "host");
    Check(ExtractJsonString(relayed_offer, "sdp") == "v=0\r\n");

    // 3. Verify XFixes cursor encoding matches P2P Remote wire format
    std::vector<std::uint8_t> pixels(4 * 4 * 4);
    pixels[0] = 0x11;
    pixels[1] = 0x22;
    pixels[2] = 0x33;
    pixels[3] = 0xff;
    const auto pkt = EncodeRgbaCursorPacket(4, 4, 1, 2, pixels);
    // 9 header + 4*4 mask (stride=4) + 4*4*4 color = 9 + 16 + 64 = 89 bytes
    Check(pkt.size() == 89);
    Check(pkt[0] == 'C');
    Check(static_cast<unsigned char>(pkt[1]) == 4);
    Check(static_cast<unsigned char>(pkt[3]) == 4);
    Check(static_cast<unsigned char>(pkt[5]) == 1);
    Check(static_cast<unsigned char>(pkt[7]) == 2);
    // Pixel (0,0) mask bit must be 0 (opaque), pixel (1,0) mask bit must be 1 (transparent)
    Check((static_cast<unsigned char>(pkt[9]) & 0x80u) == 0);
    Check((static_cast<unsigned char>(pkt[9]) & 0x40u) != 0);
    // Color at (0,0) must be BGRA = 0x33, 0x22, 0x11, 0xff
    Check(static_cast<unsigned char>(pkt[25]) == 0x33);
    Check(static_cast<unsigned char>(pkt[26]) == 0x22);
    Check(static_cast<unsigned char>(pkt[27]) == 0x11);
    Check(static_cast<unsigned char>(pkt[28]) == 0xff);

    CheckListener();
    std::cout << "linux_host_test passed\n";
    return 0;
}
