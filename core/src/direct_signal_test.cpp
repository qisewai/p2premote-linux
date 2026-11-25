#include "direct_signal.h"
#include "stream_events.h"

#include <winrt/Windows.Data.Json.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace p2premote;

namespace {

bool Check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

std::optional<StreamEvent> WaitForEvent(StreamEvents& events, std::string_view expected_name,
    StreamEventType expected_type = StreamEventType::signal, DWORD timeout_ms = 5000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        for (auto& ev : events.Drain()) {
            if (ev.type == expected_type && (expected_name.empty() || ev.name == expected_name)) {
                return ev;
            }
        }
        WaitForSingleObject(events.Ready(), 50);
    }
    return std::nullopt;
}

} // namespace

int main() {
    winrt::init_apartment();
    using winrt::Windows::Data::Json::JsonObject;
    constexpr std::uint16_t tcp_port = 39887;
    constexpr std::uint16_t udp_port = 39888;
    const std::wstring code = L"LanSecret2026";

    auto host_events = std::make_shared<StreamEvents>();
    DirectSignalServer server(host_events, L"127.0.0.1", tcp_port, code, udp_port);
    if (!Check(WaitForEvent(*host_events, "ready").has_value(), "server did not emit ready")) return 1;

    // 1. Test UDP auto-discovery on dedicated test port.
    {
        LanDiscoveryScanner scanner(udp_port);
        bool found = false;
        for (int i = 0; i < 40 && !found; ++i) {
            scanner.Probe();
            Sleep(50);
            for (const auto& peer : scanner.Snapshot()) {
                if (peer.port == tcp_port && !peer.busy) {
                    found = true;
                    break;
                }
            }
        }
        if (!Check(found, "UDP discovery did not find LAN host")) return 1;
    }

    // 2. Test wrong access code rejection.
    {
        auto bad_events = std::make_shared<StreamEvents>();
        DirectSignalClient bad_client(bad_events, L"127.0.0.1", tcp_port, L"WrongCode999");
        const auto err = WaitForEvent(*bad_events, "", StreamEventType::error, 5000);
        if (!Check(err.has_value(), "wrong access code was not rejected with error")) return 1;
    }

    // 3. Test valid mutual HMAC handshake + MiniRTC signal relay + clean end.
    {
        auto viewer_events = std::make_shared<StreamEvents>();
        DirectSignalClient client(viewer_events, L"127.0.0.1", tcp_port, code);
        const auto req = WaitForEvent(*host_events, "request", StreamEventType::signal, 5000);
        if (!Check(req.has_value(), "server did not emit request for valid viewer")) return 1;

        server.Send("accept", "control");
        const auto acc = WaitForEvent(*viewer_events, "accepted", StreamEventType::signal, 5000);
        if (!Check(acc.has_value() && acc->payload == "control", "viewer did not receive accepted control")) return 1;

        client.Send("minirtc", R"({"type":"join_transmission","user_id":"viewer","transmission_id":"host"})");
        const auto joined = WaitForEvent(*host_events, "minirtc");
        if (!Check(joined.has_value(), "host did not receive the join notification")) return 1;
        const auto notice = JsonObject::Parse(winrt::to_hstring(joined->payload));
        if (!Check(notice.GetNamedString(L"type") == L"user_join_transmission" &&
                notice.GetNamedString(L"status") == L"success" && notice.GetNamedString(L"user_id") == L"viewer",
                "native join request was not translated for the host")) return 1;

        server.Send("minirtc", R"({"type":"offer","sdp":"v=0","remote_user_id":"viewer"})");
        const auto offer = WaitForEvent(*viewer_events, "minirtc");
        if (!Check(offer.has_value(), "viewer did not receive the offer")) return 1;
        const auto description = JsonObject::Parse(winrt::to_hstring(offer->payload));
        if (!Check(description.GetNamedString(L"sdp") == L"v=0" && description.GetNamedString(L"remote_user_id") == L"host",
                "offer did not identify the sender")) return 1;

        client.Send("minirtc", R"({"type":"answer","sdp":"v=0","remote_user_id":"host"})");
        const auto answer = WaitForEvent(*host_events, "minirtc");
        if (!Check(answer.has_value() && JsonObject::Parse(winrt::to_hstring(answer->payload)).GetNamedString(L"remote_user_id") ==
                L"viewer", "answer did not identify the sender")) return 1;

        // 4. Test concurrent second viewer is rejected as busy.
        {
            auto busy_events = std::make_shared<StreamEvents>();
            DirectSignalClient busy_client(busy_events, L"127.0.0.1", tcp_port, code);
            const auto busy_err = WaitForEvent(*busy_events, "", StreamEventType::error, 5000);
            if (!Check(busy_err.has_value(), "second concurrent viewer was not rejected")) return 1;
        }

        client.EndSession();
    }

    // Server should detect viewer disconnect and emit ended.
    if (!Check(WaitForEvent(*host_events, "ended", StreamEventType::signal, 5000).has_value(),
            "server did not emit ended after viewer disconnect")) return 1;

    std::puts("PASS direct_signal_test");
    return 0;
}
