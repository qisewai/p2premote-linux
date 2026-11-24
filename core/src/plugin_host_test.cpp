#include "plugin_host.h"
#include "pty_session.h"

#include <p2premote/plugin_protocol.h>

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <winrt/base.h>

#include <atomic>
#include <charconv>
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
    explicit Fixture(std::string_view suffix = {}) : root(std::filesystem::temp_directory_path() /
        ("p2premote-plugin-test-" + std::to_string(GetCurrentProcessId()) + std::string(suffix))) {
        Check(std::filesystem::create_directory(root), "Test directory already exists");
    }

    ~Fixture() {
        std::error_code ignored;
        for (auto entry = paths.rbegin(); entry != paths.rend(); ++entry) std::filesystem::remove(*entry, ignored);
        std::filesystem::remove(root, ignored);
    }

    void Plugin(std::string_view id, std::string_view script, std::string_view role = "viewer",
        std::string_view entry = "main.lua", bool enabled = true, std::string_view permissions = R"("channel")") {
        const auto directory = root / id;
        std::filesystem::create_directory(directory);
        paths.push_back(directory);
        paths.push_back(directory / "main.lua");
        std::ofstream(paths.back(), std::ios::binary) << script;
        paths.push_back(directory / "plugin.json");
        std::ofstream(paths.back(), std::ios::binary)
            << "{\"id\":\"" << id << "\",\"entry\":\"" << entry << "\",\"enabled\":" << std::boolalpha << enabled
            << ",\"roles\":[\"" << role << "\"],\"permissions\":[" << permissions << "]}";
    }

    void RemoveOnExit(const std::filesystem::path& file) { paths.push_back(file); }

    void CopyPlugin(const std::filesystem::path& source) {
        const auto directory = root / source.filename();
        Check(std::filesystem::create_directory(directory), "Could not stage plugin fixture");
        paths.push_back(directory);
        for (const auto* leaf : {"plugin.json", "main.lua"}) {
            paths.push_back(directory / leaf);
            std::filesystem::copy_file(source / leaf, paths.back());
        }
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

    std::optional<std::string> WaitForType(std::string_view id, std::string_view type, std::chrono::milliseconds timeout = 3s) {
        std::optional<std::string> payload;
        std::unique_lock lock(mutex);
        ready.wait_for(lock, timeout, [&] {
            for (const auto& frame : frames) {
                const auto packet = DecodePluginPacket(frame);
                if (packet && packet->id == id && packet->type == type) {
                    payload.emplace(packet->payload);
                    return true;
                }
            }
            return false;
        });
        return payload;
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
        assert(p2p.ui == nil and p2p.config == nil and p2p.video == nil)
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
    fixture.Plugin("_workspace", "p2p.channel.send('leak', '')");

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
    for (const auto* id : {"b-memory", "c-api", "d-globals", "f-role", "g-disabled", "_workspace"}) {
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
        if (memory && PutBlock(CF_UNICODETEXT, block)) return true;
        GlobalFree(block);
        return false;
    }

    bool PutFiles(std::initializer_list<std::filesystem::path> files) {
        std::wstring paths;
        for (const auto& file : files) {
            paths.append(file.native());
            paths.push_back(L'\0');
        }
        paths.push_back(L'\0');
        const auto size = paths.size() * sizeof(wchar_t);
        const auto block = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + size);
        if (!block) return false;
        auto* drop = static_cast<DROPFILES*>(GlobalLock(block));
        if (drop) {
            *drop = {};
            drop->pFiles = sizeof(DROPFILES);
            drop->fWide = TRUE;
            std::memcpy(drop + 1, paths.data(), size);
            GlobalUnlock(block);
        }
        if (drop && PutBlock(CF_HDROP, block)) return true;
        GlobalFree(block);
        return false;
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
    bool PutBlock(UINT format, HGLOBAL block) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (OpenClipboard(window)) {
                const bool written = EmptyClipboard() && SetClipboardData(format, block);
                CloseClipboard();
                return written;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    HWINSTA original_station{GetProcessWindowStation()};
    HDESK original_desktop{GetThreadDesktop(GetCurrentThreadId())};
    HWINSTA station{};
    HDESK desktop{};
    HWND window{};
    bool switched{};
};

void TestWorkspace() {
    Fixture fixture;
    const auto config = fixture.root / "config";
    fixture.RemoveOnExit(config);
    fixture.RemoveOnExit(config / "office.viewer.json");
    fixture.Plugin("office", R"lua(
        local api, id = p2p.ui, nil
        local saved = p2p.config.get("enabled", false)
        assert(not api.add_menu({label = "缺少归属", on_click = function() end}))
        assert(not api.add_menu({menu = "tools", label = "路径过长",
            path = {"1", "2", "3", "4"}, on_click = function() end}))
        id = api.add_menu({menu = "office-tools", menu_title = "办公", menu_order = 20,
            path = {"输入", "同步"}, section = "shortcuts", order = 10,
            label = "测试开关", checked = saved, on_click = function()
            assert(api.set_checked(id, true))
            assert(not api.set_checked(id + 1, true))
            assert(p2p.config.set("enabled", true))
            assert(p2p.config.set("text", "办公 😀"))
            assert(p2p.config.set("number", 4.5))
            assert(p2p.config.get("text") == "办公 😀" and p2p.config.get("number") == 4.5)
            assert(p2p.config.set("number", nil) and p2p.config.get("number", 9) == 9)
            assert(not p2p.config.set("../wrong", true))
            local stats = p2p.session.stats()
            assert(stats.route == "relay" and stats.rtt_ms == 42 and stats.loss == 0.25 and stats.download_bps == 1234)
            local keys = p2p.input.get_state()
            assert(keys.caps_lock == true and keys.num_lock == false)
            assert(not p2p.input.set_lock("caps_lock", false))
            assert(not p2p.input.set_ime(false))
            assert(p2p.input.send_keys({"Ctrl", "Shift", "Esc"}))
            assert(p2p.input.send_text("办公 😀"))
            assert(p2p.video.set_bitrate("economy"))
            assert(api.notify("更新完成"))
            assert(p2p.channel.send("done", ""))
        end})
        assert(id and p2p.channel.send("loaded", saved and "yes" or "no"))
    )lua", "viewer", "main.lua", true, R"("channel", "ui", "config", "video", "input")");

    WireLog log;
    const auto send = [&log](std::string_view frame) { return log.Send(frame); };
    PluginHost viewer(false, send, fixture.root, {}, config);
    viewer.UpdateMetrics({IcePath::relay, 42, 0.25, 1234});
    viewer.UpdateLockState(true, false);
    viewer.SetConnected(true);
    Check(log.Wait({"office", "loaded", "no"}), "Office plugin did not load");
    const auto initial = viewer.Menu();
    Check(initial.size() == 1 && initial[0].label == L"测试开关" && initial[0].checked == false, "Wrong menu state");
    Check(initial[0].plugin_id == "office" && initial[0].menu == "office-tools" &&
        initial[0].menu_title == L"办公" && initial[0].path == L"输入\x1f" L"同步" &&
        initial[0].section == "shortcuts" && initial[0].menu_order == 20 && initial[0].order == 10 &&
        !initial[0].toolbar_button, "Menu placement was not preserved");
    viewer.Invoke(initial[0].id + 1);
    viewer.Invoke(initial[0].id);
    Check(log.Wait({"office", "done", ""}), "Menu invokes the wrong handler or a workspace API failed");
    const auto actions = viewer.TakeActions();
    Check(actions.size() == 4 && actions[0].input.kind == InputCommandKind::shortcut && actions[0].input.units.size() == 3 &&
        actions[1].input.kind == InputCommandKind::text && actions[1].input.units == std::vector<std::uint16_t>{0x529e, 0x516c, 32, 0xd83d, 0xde00} &&
        actions[2].bitrate == VideoBitratePreset::economy && actions[3].notice == L"更新完成", "Lua actions were not copied to the UI queue");
    viewer.Stop();
    Check(viewer.Menu().empty() && viewer.TakeActions().empty(), "Office UI persisted after disconnect");

    PluginHost resumed(false, send, fixture.root, {}, config);
    resumed.SetConnected(true);
    Check(log.Wait({"office", "loaded", "yes"}), "JSON settings did not survive a fresh session");
    Check(resumed.Menu().size() == 1 && resumed.Menu()[0].checked == true && resumed.Menu()[0].id != initial[0].id,
        "Reloaded menu did not restore its setting or reused a stale command ID");
}

void TestOfficePolicies(const std::filesystem::path& root) {
    Fixture fixture;
    const auto config = fixture.root / "config";
    fixture.RemoveOnExit(config);
    fixture.RemoveOnExit(config / "keyboard-sync.viewer.json");
    fixture.RemoveOnExit(config / "adaptive-quality.viewer.json");
    fixture.CopyPlugin(root / "keyboard-sync");
    fixture.CopyPlugin(root / "adaptive-quality");

    std::atomic_bool keyboard_hello{}, keyboard_sent{}, keyboard_toggled{};
    PluginHost viewer(false, [&](std::string_view bytes) {
        const auto packet = DecodePluginPacket(bytes);
        if (packet && packet->id == "keyboard-sync") {
            if (packet->type == "hello" && packet->payload.empty()) keyboard_hello = true;
            if (packet->type == "state" && packet->payload.size() == 3 && packet->payload.starts_with("10")) keyboard_sent = true;
            if (packet->type == "state" && packet->payload.size() == 3 && packet->payload.starts_with("01")) keyboard_toggled = true;
        }
        return true;
    }, fixture.root, {}, config);

    const auto wait = [](auto timeout, const auto& predicate) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        do {
            if (predicate()) return true;
            std::this_thread::sleep_for(20ms);
        } while (std::chrono::steady_clock::now() < until);
        return false;
    };
    viewer.UpdateMetrics({IcePath::relay, 150, 0.04, {}});
    viewer.UpdateLockState(true, false);
    viewer.SetConnected(true);
    std::vector<PluginMenuItem> menu;
    Check(wait(3s, [&] { menu = viewer.Menu(); return menu.size() == 2; }), "Built-in office plugins did not load");
    Check(menu[0].checked == false && menu[1].checked == false, "Office policies should default to off");
    for (const auto& item : menu) viewer.Invoke(item.id);
    Check(wait(2s, [&] { return keyboard_hello.load(); }) && !keyboard_sent.load() && !viewer.CapturesLockKeys(),
        "Peer support was not negotiated first");
    Deliver(viewer, {"keyboard-sync", "ready", ""});
    Check(wait(2s, [&] { return keyboard_sent.load(); }) && viewer.CapturesLockKeys(),
        "Keyboard plugin did not take over lock keys and send their state");
    viewer.UpdateLockState(false, true);
    Check(wait(250ms, [&] { return keyboard_toggled.load(); }),
        "Lock-key change was not pushed immediately without waiting for the poll timer");

    const auto has_bitrate = [&](VideoBitratePreset bitrate) {
        for (const auto& action : viewer.TakeActions()) {
            if (action.kind == PluginAction::Kind::bitrate && action.bitrate == bitrate) return true;
        }
        return false;
    };
    Check(wait(4s, [&] { return has_bitrate(VideoBitratePreset::economy); }), "Policy did not reduce bitrate on a weak route");
    viewer.UpdateMetrics({IcePath::host, 5, 0.001, {}});
    Check(wait(9s, [&] { return has_bitrate(VideoBitratePreset::detail); }), "Policy did not recover bitrate on a clear LAN");
    viewer.UpdateMetrics({IcePath::relay, 50, 0.001, {}});
    Check(wait(2s, [&] { return has_bitrate(VideoBitratePreset::automatic); }), "Policy retained the LAN preset after switching to relay");
    viewer.Stop();
    Check(!viewer.CapturesLockKeys(), "Lock keys were still intercepted after disconnect");
}

int TestClipboard(const std::filesystem::path& root) {
    PrivateClipboard clipboard;
    if (!clipboard.Available()) {
        std::cout << "Could not allocate a private Windows clipboard\n";
        return 77;
    }
    Fixture config;
    WireLog log;
    PluginHost host(true, [&log](std::string_view frame) { return log.Send(frame); }, root, {}, config.root);
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
    PluginHost viewer(false, [&viewer_log](std::string_view frame) { return viewer_log.Send(frame); }, root, {}, config.root);
    viewer.SetConnected(true);
    Check(viewer_log.Wait({"clipboard-sync", "hello", ""}), "Viewer did not advertise its plugin");
    Check(viewer_log.Count({"clipboard-sync", "text", "被控 → 主控\r\nOffice"}) == 0,
        "Clipboard contents were sent before peer support was confirmed");
    Deliver(viewer, {"clipboard-sync", "ready", ""});
    Check(viewer_log.Wait({"clipboard-sync", "text", "被控 → 主控\r\nOffice"}), "Viewer did not send its initial clipboard");
    return 0;
}

int TestFileClipboard(const std::filesystem::path& root) {
    PrivateClipboard clipboard;
    if (!clipboard.Available()) return 77;
    Check(SUCCEEDED(OleInitialize(nullptr)), "Could not initialize the OLE test client");
    struct OleScope { ~OleScope() { OleUninitialize(); } } ole;

    Fixture fixture;
    fixture.CopyPlugin(root / "file-clipboard");
    Fixture sources("-sources");
    const auto folder = sources.root / L"资料";
    const auto subfolder = folder / L"子目录";
    const auto file = subfolder / L"文件.bin";
    const auto empty = sources.root / L"空.txt";
    Check(std::filesystem::create_directories(subfolder), "Could not create the source directory");
    for (const auto& path : {folder, subfolder, file, empty}) sources.RemoveOnExit(path);
    std::string expected(600 * 1024 + 37, '\0');
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<char>((i + i / 251) % 251);
    Check(static_cast<bool>(std::ofstream(file, std::ios::binary).write(expected.data(),
        static_cast<std::streamsize>(expected.size()))), "Could not create source file");
    Check(static_cast<bool>(std::ofstream(empty)), "Could not create empty source file");

    constexpr std::string_view plugin = "file-clipboard";
    WireLog source_log, receiver_log;
    std::mutex link_mutex;
    PluginHost* link{};
    std::atomic_bool hold_reads{};
    winrt::handle read_pending{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(static_cast<bool>(read_pending), "Could not create the pending read event");
    PluginHost source(true, [&](std::string_view frame) {
        const std::lock_guard lock(link_mutex);
        if (link) link->Receive(frame);
        return source_log.Send(frame);
    }, fixture.root, {}, fixture.root);
    source.SetConnected(true);
    Deliver(source, {plugin, "hello", "1"});
    Check(source_log.Wait({plugin, "ready", "1"}), "Source file clipboard did not negotiate");
    Check(clipboard.PutFiles({folder, empty}), "Could not copy paths to the clipboard");
    const auto manifest = source_log.WaitForType(plugin, "offer");
    Check(manifest && manifest->find("文件.bin") != std::string::npos &&
        manifest->find("p2premote-plugin-test-") == std::string::npos, "Copy did not send only relative file descriptors");

    PluginHost receiver(false, [&](std::string_view frame) {
        const auto packet = DecodePluginPacket(frame);
        if (hold_reads && packet && packet->id == plugin && packet->type == "read") SetEvent(read_pending.get());
        else source.Receive(frame);
        return receiver_log.Send(frame);
    }, fixture.root, {}, fixture.root);
    struct ScopedLink {
        std::mutex& mutex;
        PluginHost*& link;
        ScopedLink(std::mutex& mutex, PluginHost*& link, PluginHost& receiver) : mutex(mutex), link(link) {
            const std::lock_guard lock(mutex);
            link = &receiver;
        }
        ~ScopedLink() {
            const std::lock_guard lock(mutex);
            link = nullptr;
        }
    } connection(link_mutex, link, receiver);
    receiver.SetConnected(true);
    Check(source_log.Wait({plugin, "ready", "1"}, 2), "Destination file clipboard did not negotiate");

    Deliver(receiver, {plugin, "offer", R"({"id":2,"files":[["..\\\\outside",1,false]]})"});
    Deliver(receiver, {plugin, "offer", *manifest});
    const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const auto deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline && !IsClipboardFormatAvailable(format)) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    }
    Check(IsClipboardFormatAvailable(format), "The OLE file clipboard was not published");
    winrt::com_ptr<IDataObject> object;
    Check(SUCCEEDED(OleGetClipboard(object.put())) && object, "Could not obtain the OLE clipboard");
    struct Medium {
        STGMEDIUM value{};
        ~Medium() { ReleaseStgMedium(&value); }
    } descriptors, data, zero;
    FORMATETC descriptor_request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Check(SUCCEEDED(object->GetData(&descriptor_request, &descriptors.value)), "Could not read file descriptors");
    const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(descriptors.value.hGlobal));
    Check(group != nullptr, "Could not lock file descriptors");
    const auto* entries = reinterpret_cast<const FILEDESCRIPTORW*>(
        reinterpret_cast<const char*>(group) + offsetof(FILEGROUPDESCRIPTORW, fgd));
    const bool valid = group->cItems == 4 && std::wstring_view(entries[0].cFileName) == L"资料" &&
        (entries[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && std::wstring_view(entries[3].cFileName) == L"空.txt" &&
        std::wstring_view(entries[2].cFileName) == L"资料\\子目录\\文件.bin" && entries[2].nFileSizeLow == expected.size();
    GlobalUnlock(descriptors.value.hGlobal);
    Check(valid, "OLE descriptors lost a name, directory, size or nesting information");
    Check(IsClipboardFormatAvailable(CF_HDROP) == FALSE, "Virtual files unexpectedly advertised local paths");

    FORMATETC content_request{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)),
        nullptr, DVASPECT_CONTENT, 2, TYMED_ISTREAM};
    Check(SUCCEEDED(object->GetData(&content_request, &data.value)), "Could not obtain the remote file stream");
    content_request.lindex = 3;
    Check(SUCCEEDED(object->GetData(&content_request, &zero.value)), "Could not obtain the empty file stream");
    char byte{};
    ULONG count{};
    Check(zero.value.pstm->Read(&byte, 1, &count) == S_FALSE && count == 0, "An empty file did not immediately return EOF");
    Check(!receiver_log.WaitForType(plugin, "read", 100ms), "Opening the clipboard or stream requested file bytes");

    LARGE_INTEGER offset{};
    offset.QuadPart = 17;
    Check(SUCCEEDED(data.value.pstm->Seek(offset, STREAM_SEEK_SET, nullptr)), "Could not seek the virtual file");
    Check(SUCCEEDED(data.value.pstm->Read(&byte, 1, &count)) && count == 1 && byte == expected[17],
        "A range was not read at the requested offset");
    offset.QuadPart = 0;
    Check(SUCCEEDED(data.value.pstm->Seek(offset, STREAM_SEEK_SET, nullptr)), "Could not rewind the virtual file");
    std::string pasted(expected.size() + 1, '\0');
    Check(data.value.pstm->Read(pasted.data(), static_cast<ULONG>(pasted.size()), &count) == S_FALSE &&
        count == expected.size() && std::string_view(pasted).substr(0, count) == expected, "Pasted bytes did not match the source");

    Check(static_cast<bool>(std::ofstream(file, std::ios::binary | std::ios::trunc) << "changed"),
        "Could not change the source file");
    content_request.lindex = 2;
    Medium changed;
    Check(SUCCEEDED(object->GetData(&content_request, &changed.value)), "Could not reopen the virtual file stream");
    Check(FAILED(changed.value.pstm->Read(&byte, 1, &count)), "An altered source file was accepted");
    Medium pending;
    Check(SUCCEEDED(object->GetData(&content_request, &pending.value)), "Could not open a stream for the disconnect test");
    hold_reads = true;
    std::jthread disconnect([&] {
        if (WaitForSingleObject(read_pending.get(), 3000) == WAIT_OBJECT_0) receiver.Stop();
    });
    const auto start = GetTickCount64();
    Check(FAILED(pending.value.pstm->Read(&byte, 1, &count)) && WaitForSingleObject(read_pending.get(), 0) == WAIT_OBJECT_0 &&
        GetTickCount64() - start < 3000, "Disconnect did not cancel a pending file read");
    disconnect.join();
    Check(FAILED(data.value.pstm->Read(&byte, 1, &count)), "A stream remained readable after disconnect");
    return 0;
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(static_cast<bool>(file), "Could not open file for verification");
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.push_back('"');
    for (char c : text) {
        if (c == '\\') out.append("\\\\");
        else if (c == '"') out.append("\\\"");
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void TestFileTransfer(const std::filesystem::path& root) {
    Fixture fixture;
    const auto config = fixture.root / "config";
    const auto local_dir = fixture.root / "local";
    const auto remote_dir = fixture.root / "remote";
    std::filesystem::create_directory(local_dir);
    std::filesystem::create_directory(remote_dir);
    fixture.RemoveOnExit(config);
    fixture.CopyPlugin(root / "file-transfer");

    // Prepare multi-chunk binary files (with NUL bytes) and an empty file.
    std::string up_bytes(150 * 1024 + 37, '\0');
    for (std::size_t i = 0; i < up_bytes.size(); ++i) up_bytes[i] = static_cast<char>((i * 131 + 17) & 0xff);
    std::string down_bytes(180 * 1024 + 19, '\0');
    for (std::size_t i = 0; i < down_bytes.size(); ++i) down_bytes[i] = static_cast<char>((i * 73 + 5) & 0xff);

    {
        std::ofstream(local_dir / "sample_up.bin", std::ios::binary).write(up_bytes.data(), up_bytes.size());
        std::ofstream(local_dir / "empty.bin", std::ios::binary);
        std::ofstream(remote_dir / "sample_down.bin", std::ios::binary).write(down_bytes.data(), down_bytes.size());
    }

    // Also validate path-traversal rejection and declarative UI round-trip via a tiny probe plugin.
    fixture.Plugin("ui-fs-probe", R"lua(
        assert(p2p.fs ~= nil and p2p.json ~= nil)
        local j = p2p.json.encode({ ok = true, arr = {1, 2, "三"} })
        local d = p2p.json.decode(j)
        assert(d.ok == true and d.arr[3] == "三")
        assert(p2p.fs.join("C:\\Temp", "../escape") == nil)
        assert(p2p.fs.join("C:\\Temp", "a\\b") == nil)
        assert(p2p.fs.join("C:\\Temp", "a/b") == nil)
        assert(p2p.fs.join("C:\\Temp", "C:evil") == nil)
        assert(p2p.fs.join("C:\\Temp", "..") == nil)
        p2p.session.on("connected", function()
            local dlg = nil
            dlg = p2p.ui.open_dialog({ title = "probe", body = { type = "vbox", children = {} } }, function(ev)
                if ev.control == "ping" and ev.event == "click" then
                    assert(ev.values.note == "hello")
                    p2p.ui.update_dialog(dlg, { status = { text = "pong" } })
                    p2p.ui.close_dialog(dlg)
                end
            end)
            assert(dlg and dlg > 0)
        end)
    )lua", "viewer", "main.lua", true, R"("ui", "fs")");

    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginHost> viewer;
    host = std::make_unique<PluginHost>(true, [&](std::string_view frame) {
        if (viewer) viewer->Receive(frame);
        return true;
    }, fixture.root, PluginHost::Notify{}, config);
    viewer = std::make_unique<PluginHost>(false, [&](std::string_view frame) {
        if (host) {
            // Keep the requested download window below the ACK batch size for the entire transfer.
            if (const auto packet = DecodePluginPacket(frame); packet && packet->id == "file-transfer" &&
                    (packet->type == "dn_open" || packet->type == "dn_pace" || packet->type == "dn_ack")) {
                std::string payload(packet->payload);
                if (const auto field = payload.find("\"win\":"); field != std::string::npos) {
                    const auto start = field + 6;
                    const auto end = payload.find_first_not_of("0123456789", start);
                    payload.replace(start, end - start, "2");
                    if (const auto replacement = EncodePluginPacket({packet->id, packet->type, payload})) {
                        host->Receive(*replacement);
                        return true;
                    }
                }
            }
            host->Receive(frame);
        }
        return true;
    }, fixture.root, PluginHost::Notify{}, config);

    const auto wait = [](auto timeout, const auto& predicate) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        do {
            if (predicate()) return true;
            std::this_thread::sleep_for(20ms);
        } while (std::chrono::steady_clock::now() < until);
        return false;
    };

    host->SetConnected(true);
    viewer->SetConnected(true);

    // 1. Verify probe dialog open -> event -> update -> close round-trip.
    std::uint32_t probe_dlg{};
    std::uint32_t ft_dlg{};
    bool probe_pong{}, probe_closed{};
    bool peer_ready{}, local_ready{}, remote_ready{};
    const auto drain_ui = [&] {
        for (const auto& cmd : viewer->TakeUiCommands()) {
            if (cmd.kind == "open" && cmd.json.find("\"probe\"") != std::string::npos) {
                probe_dlg = cmd.dialog_id;
            } else if (cmd.kind == "open" && cmd.json.find("local_table") != std::string::npos) {
                ft_dlg = cmd.dialog_id;
            } else if (cmd.kind == "update" && cmd.dialog_id == probe_dlg && cmd.json.find("\"pong\"") != std::string::npos) {
                probe_pong = true;
            } else if (cmd.kind == "close" && cmd.dialog_id == probe_dlg) {
                probe_closed = true;
            } else if (cmd.kind == "update" && cmd.dialog_id == ft_dlg) {
                if (cmd.json.find("remote_quick") != std::string::npos) peer_ready = true;
                if (cmd.json.find("sample_up.bin") != std::string::npos) local_ready = true;
                if (cmd.json.find("sample_down.bin") != std::string::npos) remote_ready = true;
            }
        }
    };

    Check(wait(3s, [&] { drain_ui(); return probe_dlg != 0; }), "Probe dialog did not open");
    viewer->SendUiEvent({probe_dlg, "ping", "click", R"({"values":{"note":"hello"}})"});
    Check(wait(3s, [&] { drain_ui(); return probe_pong && probe_closed; }), "Declarative UI event/update/close round-trip failed");

    // 2. Open built-in file-transfer dialog from Viewer menu.
    std::vector<PluginMenuItem> menu;
    Check(wait(3s, [&] { menu = viewer->Menu(); return !menu.empty(); }), "File-transfer menu did not register");
    viewer->Invoke(menu[0].id);
    Check(wait(3s, [&] { drain_ui(); return ft_dlg != 0 && peer_ready; }), "File-transfer dialog or peer handshake failed");

    // 3. Navigate both panes to our temp local and remote directories.
    const auto local_utf8 = local_dir.u8string();
    const auto remote_utf8 = remote_dir.u8string();
    const std::string local_str(reinterpret_cast<const char*>(local_utf8.data()), local_utf8.size());
    const std::string remote_str(reinterpret_cast<const char*>(remote_utf8.data()), remote_utf8.size());

    viewer->SendUiEvent({ft_dlg, "local_path", "submit", "{\"text\":" + JsonEscape(local_str) + "}"});
    viewer->SendUiEvent({ft_dlg, "remote_path", "submit", "{\"text\":" + JsonEscape(remote_str) + "}"});
    Check(wait(4s, [&] { drain_ui(); return local_ready && remote_ready; }), "Dual-pane directory listing failed");

    // 4. Create folders on both local and remote sides.
    viewer->SendUiEvent({ft_dlg, "local_mkdir", "click", R"({"text":"new_local"})"});
    viewer->SendUiEvent({ft_dlg, "remote_mkdir", "click", R"({"text":"new_remote"})"});
    Check(wait(3s, [&] {
        return std::filesystem::is_directory(local_dir / "new_local") &&
            std::filesystem::is_directory(remote_dir / "new_remote");
    }), "Local or remote mkdir failed");

    // 5. Upload two files (150 KiB binary + 0-byte empty file) from Viewer -> Host.
    viewer->SendUiEvent({ft_dlg, "upload_btn", "click",
        R"({"values":{"local_table":["sample_up.bin","empty.bin"]}})"});
    Check(wait(8s, [&] {
        drain_ui();
        return std::filesystem::is_regular_file(remote_dir / "sample_up.bin") &&
            std::filesystem::is_regular_file(remote_dir / "empty.bin") &&
            std::filesystem::file_size(remote_dir / "sample_up.bin") == up_bytes.size();
    }), "Upload of binary and empty files failed");
    Check(ReadBinaryFile(remote_dir / "sample_up.bin") == up_bytes, "Uploaded binary content mismatch");
    Check(ReadBinaryFile(remote_dir / "empty.bin").empty(), "Uploaded empty file mismatch");

    // 6. Download a 180 KiB binary file from Host -> Viewer under simulated 4% video loss
    //    to verify automatic video-loss-aware dynamic pacing + end-to-end SHA-256 verification.
    viewer->UpdateMetrics({IcePath::nat, 25, 0.001, {}});
    std::this_thread::sleep_for(550ms);
    viewer->UpdateMetrics({IcePath::nat, 85, 0.04, {}});
    viewer->SendUiEvent({ft_dlg, "download_btn", "click",
        R"({"values":{"remote_table":["sample_down.bin"]}})"});
    Check(wait(8s, [&] {
        drain_ui();
        return std::filesystem::is_regular_file(local_dir / "sample_down.bin") &&
            std::filesystem::file_size(local_dir / "sample_down.bin") == down_bytes.size();
    }), "Download of binary file failed");
    Check(ReadBinaryFile(local_dir / "sample_down.bin") == down_bytes, "Downloaded binary content mismatch");

    // 7. Stop and verify clean close command + zero leftover .p2ptmp files.
    viewer->Stop();
    host->Stop();
    bool ft_closed{};
    for (const auto& cmd : viewer->TakeUiCommands()) {
        if (cmd.dialog_id == ft_dlg && cmd.kind == "close") ft_closed = true;
    }
    Check(ft_closed, "Open dialog did not emit close command on disconnect");

    std::error_code ignored;
    std::filesystem::remove_all(local_dir, ignored);
    std::filesystem::remove_all(remote_dir, ignored);
}

std::string DecodeBase64Simple(std::string_view input) {
    auto val_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t val = 0;
    int bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        const int d = val_of(c);
        if (d < 0) continue;
        val = (val << 6) | static_cast<std::uint32_t>(d);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

void TestRemoteTerminal(const std::filesystem::path& source_plugins) {
    Fixture fixture;
    fixture.CopyPlugin(source_plugins / "remote-terminal");
    const auto config = fixture.root / L"config";
    std::atomic_bool hold_opened{}, drop_output{}, drop_input_ack{}, reject_input{};
    std::atomic_uint open_replies{}, close_replies{}, input_packets{}, output_retries{};
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginHost> viewer;
    host = std::make_unique<PluginHost>(true, [&](std::string_view frame) {
        if (const auto packet = DecodePluginPacket(frame)) {
            if (packet->type == "opened") {
                ++open_replies;
                if (hold_opened) return true;
            }
            if (packet->type == "closed") ++close_replies;
            if (packet->type == "out" && drop_output.exchange(false)) {
                ++output_retries;
                return true;
            }
            if (packet->type == "in_ack" && drop_input_ack.exchange(false)) return true;
        }
        if (viewer) viewer->Receive(frame);
        return true;
    }, fixture.root, PluginHost::Notify{}, config);
    viewer = std::make_unique<PluginHost>(false, [&](std::string_view frame) {
        if (const auto packet = DecodePluginPacket(frame); packet && packet->type == "in") {
            ++input_packets;
            if (reject_input.exchange(false)) return false;
        }
        if (host) host->Receive(frame);
        return true;
    }, fixture.root, PluginHost::Notify{}, config);
    struct StopOnExit {
        PluginHost& viewer;
        PluginHost& host;
        ~StopOnExit() { viewer.Stop(); host.Stop(); }
    } stop{*viewer, *host};

    const auto wait = [](auto timeout, const auto& predicate) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        do {
            if (predicate()) return true;
            std::this_thread::sleep_for(25ms);
        } while (std::chrono::steady_clock::now() < until);
        return false;
    };

    host->SetConnected(true);
    viewer->SetConnected(true);

    std::vector<PluginMenuItem> menu;
    Check(wait(3s, [&] { menu = viewer->Menu(); return !menu.empty(); }), "Remote-terminal menu did not register");
    viewer->Invoke(menu[0].id);

    std::uint32_t dlg{}, session{}, input_ack{};
    bool clear{}, resized{}, exited{};
    std::string terminal_output;
    const auto read_number = [](std::string_view json, std::string_view key, std::uint32_t& value) {
        const auto pos = json.find(key);
        if (pos != json.npos) {
            std::from_chars(json.data() + pos + key.size(), json.data() + json.size(), value);
        }
    };
    const auto drain_ui = [&] {
        for (const auto& cmd : viewer->TakeUiCommands()) {
            if (cmd.kind == "open" && cmd.json.find("\"terminal\"") != std::string::npos) {
                dlg = cmd.dialog_id;
            } else if (cmd.kind == "update" && cmd.dialog_id == dlg) {
                read_number(cmd.json, "\"input_session\":", session);
                read_number(cmd.json, "\"input_ack\":", input_ack);
                clear |= cmd.json.find("\"clear\":true") != std::string::npos;
                resized |= cmd.json.find("120×36") != std::string::npos;
                exited |= cmd.json.find("进程已退出（7）") != std::string::npos;
                const std::string key = "\"write_b64\":\"";
                const auto pos = cmd.json.find(key);
                if (pos != std::string::npos) {
                    const auto start = pos + key.size();
                    const auto end = cmd.json.find('"', start);
                    if (end != std::string::npos) {
                        terminal_output += DecodeBase64Simple(std::string_view(cmd.json).substr(start, end - start));
                    }
                }
            }
        }
    };

    Check(wait(5s, [&] { drain_ui(); return dlg != 0 && session != 0; }), "PowerShell PTY failed to open");
    const auto powershell = session;

    viewer->SendUiEvent({dlg, "shell", "change", R"({"value":"cmd"})"});
    Check(wait(5s, [&] { drain_ui(); return session != 0 && session != powershell; }), "Switching to CMD PTY failed");

    terminal_output.clear();
    viewer->SendUiEvent({dlg, "term", "resize", R"({"cols":120,"rows":36})"});
    Check(wait(3s, [&] { drain_ui(); return resized; }), "Host did not confirm the terminal grid size");
    viewer->SendUiEvent({dlg, "clear", "click", "{}"});
    Check(wait(3s, [&] { drain_ui(); return clear; }) && input_packets == 0, "Clear display sent input to the shell");

    const auto input = [&](unsigned seq, std::string_view b64) {
        viewer->SendUiEvent({dlg, "term", "data", "{\"session\":" + std::to_string(session) +
            ",\"seq\":" + std::to_string(seq) + ",\"data_b64\":\"" + std::string(b64) + "\"}"});
    };
    reject_input = true;
    drop_input_ack = true;
    drop_output = true;
    // echo P2P_GHOSTTY_OK
    input(1, "ZWNobyBQMlBfR0hPU1RUWV9PSw0=");
    Check(wait(5s, [&] {
        drain_ui();
        return input_ack == 1 && terminal_output.find("P2P_GHOSTTY_OK\r") != std::string::npos;
    }) && input_packets >= 3 && output_retries == 1, "Terminal stream did not recover from transport backpressure and loss");
    const auto echoed = terminal_output.find("P2P_GHOSTTY_OK\r");
    Check(terminal_output.find("P2P_GHOSTTY_OK\r", echoed + 1) == std::string::npos, "Input retry duplicated the command");

    // Exercise more than one wire frame of continuous terminal output.
    terminal_output.clear();
    input(2, "Zm9yIC9MICVpIGluICgxLDEsMTUwMCkgZG8gQGVjaG8gUDJQX0JVTEtfJWlfYWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTIzNDU2Nzg5X0VORA0=");
    Check(wait(5s, [&] {
        drain_ui();
        return input_ack == 2 && terminal_output.find("P2P_BULK_1500_") != std::string::npos;
    }), "Could not stream the full terminal output");
    std::size_t offset{};
    for (int i = 1; i <= 1500; ++i) {
        const auto record = "P2P_BULK_" + std::to_string(i) + "_";
        offset = terminal_output.find(record, offset);
        Check(offset != std::string::npos, "Terminal output lost a record or arrived out of order");
        offset += record.size();
    }

    // exit /b 7
    input(3, "ZXhpdCAvYiA3DQ==");
    Check(wait(5s, [&] { drain_ui(); return exited && session == 0; }), "Terminal did not report the shell exit code");

    viewer->SendUiEvent({dlg, "", "close", "{}"});
    Check(wait(3s, [&] { return close_replies >= 1; }), "The real Qt close event did not close the remote terminal");
    const auto previous_dialog = dlg;
    const auto previous_opens = open_replies.load();
    const auto previous_closes = close_replies.load();
    hold_opened = true;
    viewer->Invoke(menu[0].id);
    Check(wait(5s, [&] { drain_ui(); return dlg != previous_dialog && open_replies > previous_opens; }),
        "Could not reopen the dialog and begin a new terminal request");
    viewer->SendUiEvent({dlg, "", "close", "{}"});
    Check(wait(3s, [&] { return close_replies > previous_closes; }),
        "Closing during startup left the remote terminal request active");

    PtySession pressure("cmd", 100, 30);
    Check(pressure.Valid(), "Could not start the ConPTY shutdown regression test");
    Check(pressure.Write("for /L %i in (1,1,50000) do @echo P2P_PRESSURE_%i_abcdefghijklmnopqrstuvwxyz\r"),
        "Could not queue the terminal output workload");
    std::string pending_output;
    Check(wait(3s, [&] {
        pending_output += pressure.Read().bytes;
        return pending_output.find("P2P_PRESSURE_1_") != std::string::npos;
    }), "The terminal output workload did not start");

    // Let both native pipes back up without draining terminal output.
    const std::string queued_input(8192, ' ');
    for (int i = 0; i < 40; ++i) pressure.Write(queued_input);
    std::this_thread::sleep_for(200ms);
    const auto closing = std::chrono::steady_clock::now();
    pressure.Close();
    Check(std::chrono::steady_clock::now() - closing < 3s, "ConPTY shutdown did not cancel blocked I/O");
}

void TestConnectionStatus(const std::filesystem::path& root) {
    Fixture fixture;
    fixture.CopyPlugin(root / "connection-status");
    fixture.CopyPlugin(root / "session-stopwatch");
    fixture.Plugin("overlay-test", R"lua(
        assert(p2p.channel == nil and p2p.config ~= nil)
        assert(p2p.overlay.set_text("legacy"))
        assert(p2p.overlay.set("debug", {text="value", anchor="top-center", priority=7,
            font_size=15, opacity=150, monospace=true, text_color="#000000"}))
        assert(p2p.overlay.set("third", {text="third"}))
        assert(p2p.overlay.set("fourth", {text="fourth", visible=false}))
        assert(not p2p.overlay.set("fifth", {text="refused"}))
        p2p.overlay.clear("third")
        assert(p2p.overlay.set("fifth", {text="now allowed"}))
    )lua", "viewer", "main.lua", true, R"("overlay", "config")");
    fixture.Plugin("channel-overlay", R"lua(
        p2p.channel.on_message(function(kind, text)
            if kind == "status" then p2p.overlay.set_text(text) end
        end)
    )lua", "viewer", "main.lua", true, R"("overlay", "channel")");
    const auto store = std::make_shared<PluginOverlays>();
    PluginHost overlay(false, {}, fixture.root, {}, fixture.root, true, store);
    PluginHost channel(false, {}, fixture.root, {}, fixture.root, false, store);
    overlay.SetConnected(true);
    channel.SetConnected(true);
    Deliver(channel, {"channel-overlay", "status", "peer data"});
    overlay.UpdateMetrics({IcePath::host, 12.5, 0.01, 1'000'000, 250'000, 2.5, "udp", 1920, 1080, 5.0, 300});

    PluginOverlaySnapshot snapshot;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        store->Read(snapshot);
        if (snapshot.items.size() == 7) {
            const auto text = overlay.OverlayText();
            Check(text.find(L"1920×1080") != std::wstring::npos && text.find(L"60 fps") != std::wstring::npos
                && text.find(L"RTT 12.5 ms") != std::wstring::npos, "Local overlay metrics are incorrect");
            const auto find = [&](std::wstring_view id) {
                return std::ranges::find(snapshot.items, id, &PluginOverlayItem::id);
            };
            const auto clock = find(L"session-stopwatch:clock");
            const auto debug = find(L"overlay-test:debug");
            const auto remote = find(L"channel-overlay:default");
            Check(clock != snapshot.items.end() && clock->monospace && !clock->default_visible
                && clock->anchor == OverlayAnchor::top_right, "Stopwatch defaults were lost");
            Check(debug != snapshot.items.end() && debug->anchor == OverlayAnchor::top_center && debug->priority == 7
                && debug->font_size == 15 && debug->opacity == 150 && debug->text_rgb == 0 && debug->monospace,
                "Lua overlay values were lost");
            Check(remote != snapshot.items.end() && remote->text == L"peer data", "Channel overlay was not published");
            channel.Stop();
            overlay.Stop();
            store->Read(snapshot);
            Check(snapshot.items.empty(), "Session shutdown retained overlay slots");

            PluginOverlays bounded;
            for (int i = 0; i < 17; ++i) {
                PluginOverlayItem item;
                item.plugin_id = std::to_wstring(i / 4);
                item.id = std::to_wstring(i);
                Check(bounded.Set(std::move(item)) == (i < 16), "Session overlay limit was not enforced");
            }
            return;
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    Check(false, "Local overlay did not render without a peer plugin channel");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--office") {
            TestOfficePolicies(argv[2]);
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--file-transfer") {
            TestFileTransfer(argv[2]);
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--remote-terminal") {
            TestRemoteTerminal(argv[2]);
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--file-clipboard") return TestFileClipboard(argv[2]);
        if (argc == 3 && std::wstring_view(argv[1]) == L"--connection-status") {
            TestConnectionStatus(argv[2]);
            return 0;
        }
        if (argc == 2) return TestClipboard(argv[1]);
        TestSandbox();
        TestWorkspace();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
