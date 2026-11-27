#include "lan_signal_server.h"
#include "uinput_injector.h"
#include "video_frame_cadence.h"
#include "x11_shm_capturer.h"

#include <linux/input-event-codes.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <source_location>
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
    const CapturedFrame frame{2, 2, 8, pixels};
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

} // namespace

int main() {
    using namespace p2premote::linux_host;
    CheckVideoCadence();

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
    std::vector<std::uint32_t> pixels(4 * 4);
    pixels[0] = 0xff112233UL; // Opaque pixel at (0,0): R=0x11, G=0x22, B=0x33
    const auto pkt = EncodeXFixesCursor(4, 4, 1, 2, pixels.data());
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
