#include "plugin_host.h"

#include <p2premote/plugin_protocol.h>

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace p2premote;
using namespace std::chrono_literals;

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Fixture {
public:
    Fixture() : root(std::filesystem::temp_directory_path() /
        ("p2premote-plugin-test-" + std::to_string(GetCurrentProcessId()))) {
        Check(std::filesystem::create_directory(root), "Test directory already exists");
    }

    ~Fixture() {
        std::error_code ignored;
        for (auto entry = paths.rbegin(); entry != paths.rend(); ++entry) std::filesystem::remove(*entry, ignored);
        std::filesystem::remove(root, ignored);
    }

    void Plugin(std::string_view id, std::string_view script, std::string_view role = "viewer",
        std::string_view entry = "main.lua", bool enabled = true) {
        const auto directory = root / id;
        std::filesystem::create_directory(directory);
        paths.push_back(directory);
        paths.push_back(directory / "main.lua");
        std::ofstream(paths.back(), std::ios::binary) << script;
        paths.push_back(directory / "plugin.json");
        std::ofstream(paths.back(), std::ios::binary)
            << "{\"id\":\"" << id << "\",\"entry\":\"" << entry << "\",\"enabled\":" << std::boolalpha << enabled
            << ",\"roles\":[\"" << role << "\"],\"permissions\":[\"channel\"]}";
    }

    const std::filesystem::path root;

private:
    std::vector<std::filesystem::path> paths;
};

class WireLog {
public:
    bool Send(std::string_view data) {
        const std::lock_guard lock(mutex);
        frames.emplace_back(data);
        ready.notify_all();
        return true;
    }

    std::size_t Count(PluginPacket event) {
        const std::lock_guard lock(mutex);
        return CountLocked(event);
    }

    bool Wait(PluginPacket event, std::size_t count = 1) {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, 3s, [&] { return CountLocked(event) >= count; });
    }

private:
    std::size_t CountLocked(PluginPacket event) const {
        std::size_t matches{};
        for (const auto& frame : frames) {
            const auto packet = DecodePluginPacket(frame);
            if (packet && packet->id == event.id && packet->type == event.type && packet->payload == event.payload) ++matches;
        }
        return matches;
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> frames;
};

void Deliver(PluginHost& host, PluginPacket event) {
    const auto frame = EncodePluginPacket(event);
    Check(frame.has_value(), "Invalid test frame");
    host.Receive(*frame);
}

void TestSandbox() {
    Fixture fixture;
    fixture.Plugin("sandbox", R"lua(
        assert(io == nil and os == nil and debug == nil and package == nil and coroutine == nil)
        assert(load == nil and dofile == nil and loadfile == nil and rawset == nil and pcall == nil)
        assert(string.dump == nil and string.match == nil and table.move == nil)
        assert(p2p.clipboard == nil and p2p.log == nil and p2p.input == nil)
        assert(p2p.session.role() == "viewer")
        assert(p2p.crypto.sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        p2p.session.on("connected", function()
            assert(p2p.session.connected())
            local repeating
            repeating = p2p.timer.every(10, function()
                p2p.timer.cancel(repeating)
                assert(p2p.channel.send("ready", "timer"))
            end)
            p2p.timer.after(15, function() assert(p2p.channel.send("ready", "once")) end)
        end)
        p2p.channel.on_message(function(kind, data) assert(p2p.channel.send(kind, data)) end)
    )lua");
    fixture.Plugin("other", R"lua(
        p2p.channel.on_message(function(kind, data) assert(p2p.channel.send(kind, "other:" .. data)) end)
    )lua");
    fixture.Plugin("a-loop", "while true do end");
    fixture.Plugin("b-memory", "local huge = string.rep('x', 4 * 1024 * 1024); p2p.channel.send('leak', '')");
    fixture.Plugin("c-api", "p2p.channel.send = nil; p2p.channel.on_message = nil; p2p.channel.send('leak', '')");
    fixture.Plugin("d-globals", "local send = p2p.channel.send; p2p = {}; send('leak', '')");
    fixture.Plugin("e-path", "", "viewer", "../sandbox/main.lua");
    fixture.Plugin("f-role", "p2p.channel.send('leak', '')", "host");
    fixture.Plugin("g-disabled", "p2p.channel.send('leak', '')", "viewer", "main.lua", false);

    WireLog log;
    PluginHost host(false, [&log](std::string_view frame) { return log.Send(frame); }, fixture.root);
    Deliver(host, {"sandbox", "premature", ""});
    host.SetConnected(true);
    Check(log.Wait({"sandbox", "ready", "timer"}), "Sandbox or repeating timer failed");
    Check(log.Wait({"sandbox", "ready", "once"}), "One-shot timer failed");

    host.Receive("malformed");
    Deliver(host, {"unknown", "echo", "ignored"});
    Deliver(host, {"other", "echo", "A"});
    Deliver(host, {"sandbox", "echo", std::string_view("A\0B", 3)});
    Check(log.Wait({"sandbox", "echo", std::string_view("A\0B", 3)}), "Routed message lost its binary payload");
    Check(log.Count({"other", "echo", "other:A"}) == 1 && log.Count({"sandbox", "echo", "A"}) == 0,
        "Plugin payload crossed routing namespaces");
    Check(log.Count({"sandbox", "premature", ""}) == 0 && log.Count({"unknown", "echo", "ignored"}) == 0,
        "Accepted a message outside the available routes");
    for (const auto* id : {"b-memory", "c-api", "d-globals", "f-role", "g-disabled"}) {
        Check(log.Count({id, "leak", ""}) == 0, "Sandbox, role or manifest restriction failed");
    }
    Check(log.Count({"sandbox", "ready", "timer"}) == 1, "Cancelled timer fired again");
    host.SetConnected(false);
    Deliver(host, {"sandbox", "after-stop", ""});
    host.Stop();
    Check(log.Count({"sandbox", "after-stop", ""}) == 0, "Plugin received a message after disconnect");
}

class PrivateClipboard {
public:
    PrivateClipboard() {
        const auto name = L"P2PRemoteTest-" + std::to_wstring(GetCurrentProcessId());
        station = CreateWindowStationW(name.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        if (!station && GetLastError() == ERROR_ACCESS_DENIED) {
            station = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        }
        if (!station && GetLastError() == ERROR_ALREADY_EXISTS) {
            station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
            USEROBJECTFLAGS flags{};
            DWORD required{};
            if (!station || !GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &required) ||
                (flags.dwFlags & WSF_VISIBLE)) {
                std::cout << "The generated window station is unavailable or interactive\n";
                return;
            }
        }
        if (!station || !SetProcessWindowStation(station)) {
            std::cout << (station ? "SetProcessWindowStation: " : "CreateWindowStation: ") << GetLastError() << '\n';
            return;
        }
        switched = true;
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) {
            std::cout << (desktop ? "SetThreadDesktop: " : "CreateDesktop: ") << GetLastError() << '\n';
            return;
        }
        window = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        if (!window) std::cout << "CreateWindow: " << GetLastError() << '\n';
        if (window && CountClipboardFormats() != 0) {
            DestroyWindow(window);
            window = nullptr;
        }
    }

    ~PrivateClipboard() {
        if (window) {
            if (OpenClipboard(window)) {
                EmptyClipboard();
                CloseClipboard();
            }
            DestroyWindow(window);
        }
        if (switched && SetProcessWindowStation(original_station)) SetThreadDesktop(original_desktop);
        if (desktop) CloseDesktop(desktop);
        if (station) CloseWindowStation(station);
    }

    bool Available() const noexcept { return window != nullptr; }

    bool Put(const std::wstring& text) {
        const auto size = (text.size() + 1) * sizeof(wchar_t);
        const auto block = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!block) return false;
        auto* memory = GlobalLock(block);
        if (memory) {
            std::memcpy(memory, text.c_str(), size);
            GlobalUnlock(block);
        }
        bool written{};
        for (int attempt = 0; memory && attempt < 50; ++attempt) {
            if (OpenClipboard(window)) {
                written = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, block) != nullptr;
                CloseClipboard();
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        if (!written) GlobalFree(block);
        return written;
    }

    bool Contains(const std::wstring& expected) {
        bool same{};
        if (OpenClipboard(window)) {
            if (const auto block = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* text = static_cast<const wchar_t*>(GlobalLock(block))) {
                    same = text == expected;
                    GlobalUnlock(block);
                }
            }
            CloseClipboard();
        }
        return same;
    }

    bool Wait(const std::wstring& expected) {
        for (int attempt = 0; attempt < 150; ++attempt) {
            if (Contains(expected)) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

private:
    HWINSTA original_station{GetProcessWindowStation()};
    HDESK original_desktop{GetThreadDesktop(GetCurrentThreadId())};
    HWINSTA station{};
    HDESK desktop{};
    HWND window{};
    bool switched{};
};

int TestClipboard(const std::filesystem::path& root) {
    PrivateClipboard clipboard;
    if (!clipboard.Available()) {
        std::cout << "Could not allocate a private Windows clipboard\n";
        return 77;
    }
    WireLog log;
    PluginHost host(true, [&log](std::string_view frame) { return log.Send(frame); }, root);
    host.SetConnected(true);
    Deliver(host, {"clipboard-sync", "text", "before handshake"});
    Deliver(host, {"clipboard-sync", "hello", ""});
    Check(log.Wait({"clipboard-sync", "ready", ""}), "Clipboard plugin did not negotiate");
    const std::string incoming = "主控 → 被控 \xf0\x9f\x98\x80\r\n" + std::string(60 * 1024 - 30, 'x');
    const std::wstring incoming_wide = L"主控 → 被控 \U0001f600\r\n" + std::wstring(60 * 1024 - 30, L'x');
    Deliver(host, {"clipboard-sync", "text", incoming});
    Check(clipboard.Wait(incoming_wide), "Received UTF-8 text was not written to the Win32 clipboard");
    Check(clipboard.Put(L"被控 → 主控\r\nOffice"), "Could not copy local Unicode text");
    Check(log.Wait({"clipboard-sync", "text", "被控 → 主控\r\nOffice"}), "Win32 copy was not sent to the other role");
    Check(log.Count({"clipboard-sync", "text", incoming}) == 0, "Remote update was echoed back");
    host.Stop();

    WireLog viewer_log;
    PluginHost viewer(false, [&viewer_log](std::string_view frame) { return viewer_log.Send(frame); }, root);
    viewer.SetConnected(true);
    Check(viewer_log.Wait({"clipboard-sync", "hello", ""}), "Viewer did not advertise its plugin");
    Check(viewer_log.Count({"clipboard-sync", "text", "被控 → 主控\r\nOffice"}) == 0,
        "Clipboard contents were sent before peer support was confirmed");
    Deliver(viewer, {"clipboard-sync", "ready", ""});
    Check(viewer_log.Wait({"clipboard-sync", "text", "被控 → 主控\r\nOffice"}), "Viewer did not send its initial clipboard");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2) return TestClipboard(argv[1]);
        TestSandbox();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
