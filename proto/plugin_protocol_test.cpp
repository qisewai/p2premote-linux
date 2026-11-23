#include <p2premote/plugin_protocol.h>

#include <iostream>

using namespace p2premote;

int main() {
    bool passed = true;
    const auto check = [&](bool condition, const char* name) {
        if (!condition) std::cerr << name << '\n';
        passed &= condition;
    };

    const std::string_view wire("\x01\x03\x04\x00\x03\x00\x00\x00" "a-1texta\x00\xff", 18);
    const auto packet = DecodePluginPacket(wire);
    check(packet && packet->id == "a-1" && packet->type == "text" && packet->payload == wire.substr(15),
        "Binary payload and routing fields did not round-trip");
    check(packet && EncodePluginPacket(*packet) == wire, "Wire header changed");
    check(!DecodePluginPacket(wire.substr(0, wire.size() - 1)), "Accepted truncated frame");
    check(!DecodePluginPacket(std::string(wire) + "x"), "Accepted trailing bytes");

    for (const auto offset : {0, 1, 2, 3, 7, 8, 11}) {
        auto malformed = std::string(wire);
        malformed[offset] = '\xff';
        check(!DecodePluginPacket(malformed), "Accepted unsupported header or identifier");
    }
    const std::string largest(plugin_frame_limit - 10, 'x');
    const auto full = EncodePluginPacket({"a", "b", largest});
    check(full && full->size() == plugin_frame_limit && DecodePluginPacket(*full)->payload == largest,
        "Rejected exactly one frame of data");
    check(!EncodePluginPacket({"a", "b", largest + 'x'}), "Accepted frame larger than 64 KiB");
    check(!EncodePluginPacket({"../a", "b", ""}) && !EncodePluginPacket({"a", "", ""}),
        "Accepted invalid routing key");
    check(EncodePluginPacket({"a", "b", ""}).has_value(), "Rejected empty payload");
    return passed ? 0 : 1;
}
