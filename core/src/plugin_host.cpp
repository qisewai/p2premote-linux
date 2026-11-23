#include "plugin_host.h"

#include "clipboard_bridge.h"

#include <p2premote/plugin_protocol.h>

#include <bcrypt.h>
#include <lua.hpp>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace p2premote {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::IJsonObjectStatics;
constexpr std::size_t max_plugins = 16;
constexpr std::size_t max_pending_frames = 32;
constexpr std::size_t lua_memory_limit = 2 * 1024 * 1024;
constexpr int lua_instruction_limit = 200'000;

struct Manifest {
    std::string id;
    bool channel{};
    bool clipboard{};
    bool log{};
};

void Log(std::string_view id, std::string_view level, std::string_view message) noexcept {
    try {
        std::string line("P2PRemote plugin [");
        line.append(id).append("] ").append(level).append(" ").append(message.substr(0, 256));
        std::ranges::replace(line, '\r', ' ');
        std::ranges::replace(line, '\n', ' ');
        line += '\n';
        OutputDebugStringA(line.c_str());
    } catch (...) {}
}

std::filesystem::path PluginDirectory() {
    std::wstring path(32'768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot locate executable directory");
    path.resize(length);
    return std::filesystem::path(path).parent_path() / L"plugins";
}

bool IsPlainPath(const std::filesystem::path& path, bool directory) noexcept {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

std::string ReadFile(const std::filesystem::path& path, std::size_t limit) {
    if (!IsPlainPath(path, false)) throw std::runtime_error("Invalid plugin file");
    std::ifstream file(path, std::ios::binary);
    std::string content(limit + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file.eof() || file.bad()) throw std::runtime_error("Could not read plugin file within the size limit");
    content.resize(static_cast<std::size_t>(file.gcount()));
    if (content.starts_with("\xef\xbb\xbf")) content.erase(0, 3);
    return content;
}

std::optional<Manifest> ReadManifest(const IJsonObjectStatics& json_factory, const std::filesystem::path& directory, bool host,
    std::filesystem::path& entry) {
    const auto json = json_factory.Parse(winrt::to_hstring(ReadFile(directory / "plugin.json", 16 * 1024)));
    if (!json.GetNamedBoolean(L"enabled", false)) return std::nullopt;
    Manifest manifest{winrt::to_string(json.GetNamedString(L"id"))};
    if (!IsPluginIdentifier(manifest.id) || directory.filename() != std::filesystem::path(manifest.id)) {
        throw std::runtime_error("Plugin ID must match its directory");
    }
    bool selected{};
    for (const auto& role : json.GetNamedArray(L"roles")) {
        const auto name = role.GetString();
        if (name != L"host" && name != L"viewer") throw std::runtime_error("Unknown plugin role");
        selected |= name == (host ? L"host" : L"viewer");
    }
    if (!selected) return std::nullopt;
    for (const auto& permission : json.GetNamedArray(L"permissions")) {
        const auto name = permission.GetString();
        if (name == L"channel") manifest.channel = true;
        else if (name == L"clipboard") manifest.clipboard = true;
        else if (name == L"log") manifest.log = true;
        else throw std::runtime_error("Unknown plugin permission");
    }
    const auto filename = winrt::to_string(json.GetNamedString(L"entry"));
    if (!filename.ends_with(".lua") || !IsPluginIdentifier(std::string_view(filename).substr(0, filename.size() - 4))) {
        throw std::runtime_error("Plugin entry must be a Lua file in its own directory");
    }
    entry = directory / filename;
    return manifest;
}

class PluginRuntime {
public:
    PluginRuntime(bool host, const PluginHost::SendFrame& send, const std::atomic_bool& connected)
        : host(host), send_(send), connected_(connected) {}

    bool Connected() const noexcept { return connected_.load(); }

    bool Send(PluginPacket packet) noexcept {
        if (!Connected()) return false;
        const auto now = GetTickCount64();
        if (now >= send_period_end_) {
            send_period_end_ = now + 1'000;
            send_bytes_left_ = 4 * plugin_frame_limit;
            send_frames_left_ = 32;
        }
        if (!send_frames_left_ || packet.payload.size() > send_bytes_left_) return false;
        try {
            auto frame = EncodePluginPacket(packet);
            if (!frame || frame->size() > send_bytes_left_) return false;
            --send_frames_left_;
            send_bytes_left_ -= frame->size();
            return send_(*frame);
        } catch (...) { return false; }
    }

    const bool host;
    std::optional<ClipboardBridge> clipboard;

private:
    const PluginHost::SendFrame& send_;
    const std::atomic_bool& connected_;
    ULONGLONG send_period_end_{};
    std::size_t send_bytes_left_{};
    unsigned send_frames_left_{};
};

class LuaPlugin {
public:
    LuaPlugin(Manifest manifest, PluginRuntime& runtime)
        : manifest_(std::move(manifest)), runtime_(runtime), state_(lua_newstate(Allocate, &memory_)) {
        if (!state_) throw std::bad_alloc();
        *static_cast<LuaPlugin**>(lua_getextraspace(state_)) = this;
    }

    ~LuaPlugin() { lua_close(state_); }
    LuaPlugin(const LuaPlugin&) = delete;
    LuaPlugin& operator=(const LuaPlugin&) = delete;

    bool Load(std::string_view script) { return Run(Initialize, &script); }
    const Manifest& Info() const noexcept { return manifest_; }

    void ConnectionChanged(bool connected) { Call(connected ? callbacks_.connected : callbacks_.disconnected); }

    void Receive(PluginPacket packet) {
        if (manifest_.channel) Call(callbacks_.message, 2, packet.type, packet.payload);
    }

    void ClipboardChanged(std::string_view text) {
        if (manifest_.clipboard) Call(callbacks_.clipboard, 1, text);
    }

    void RunTimer(ULONGLONG now) {
        if (!healthy_) return;
        const auto next = std::ranges::find_if(timers_, [now](const auto& timer) {
            return timer.ref != LUA_NOREF && timer.due <= now;
        });
        if (next == timers_.end()) return;
        const auto timer = *next;
        if (timer.interval) next->due = now + timer.interval;
        else *next = {};
        Call(timer.ref);
        if (!timer.interval) luaL_unref(state_, LUA_REGISTRYINDEX, timer.ref);
    }

    DWORD Wait(ULONGLONG now) const noexcept {
        DWORD delay = INFINITE;
        if (!healthy_) return delay;
        for (const auto& timer : timers_) {
            if (timer.ref != LUA_NOREF) delay = std::min(delay, timer.due <= now ? 0UL : static_cast<DWORD>(timer.due - now));
        }
        return delay;
    }

private:
    struct Timer {
        lua_Integer id{};
        ULONGLONG due{};
        ULONGLONG interval{};
        int ref{LUA_NOREF};
    };

    struct Invocation {
        int ref;
        int count;
        std::string_view first;
        std::string_view second;
    };

    static LuaPlugin& Self(lua_State* state) noexcept {
        return **static_cast<LuaPlugin**>(lua_getextraspace(state));
    }

    static void* Allocate(void* context, void* pointer, size_t old_size, size_t new_size) noexcept {
        auto& used = *static_cast<std::size_t*>(context);
        const auto previous = pointer ? old_size : 0;
        if (!new_size) {
            std::free(pointer);
            used -= previous;
            return nullptr;
        }
        if (new_size > lua_memory_limit - (used - previous)) return nullptr;
        const auto value = std::realloc(pointer, new_size);
        if (value) used = used - previous + new_size;
        return value;
    }

    bool Run(lua_CFunction function, void* data) {
        lua_pushcfunction(state_, function);
        lua_pushlightuserdata(state_, data);
        lua_sethook(state_, [](lua_State* state, lua_Debug*) {
            luaL_error(state, "instruction budget exceeded");
        }, LUA_MASKCOUNT, lua_instruction_limit);
        const auto status = lua_pcall(state_, 1, 0, 0);
        lua_sethook(state_, nullptr, 0, 0);
        if (status == LUA_OK) return true;
        const auto* message = lua_type(state_, -1) == LUA_TSTRING ? lua_tostring(state_, -1) : nullptr;
        Log(manifest_.id, "error", message ? message : "Lua error");
        lua_pop(state_, 1);
        healthy_ = false;
        return false;
    }

    static void PushString(lua_State* state, std::string_view text) {
        lua_pushlstring(state, text.empty() ? "" : text.data(), text.size());
    }

    static std::string_view CheckString(lua_State* state, int index) {
        luaL_checktype(state, index, LUA_TSTRING);
        std::size_t size{};
        const auto* value = lua_tolstring(state, index, &size);
        return {value, size};
    }

    void Call(int ref, int count = 0, std::string_view first = {}, std::string_view second = {}) {
        if (!healthy_ || ref == LUA_NOREF) return;
        Invocation call{ref, count, first, second};
        Run([](lua_State* state) {
            const auto& args = *static_cast<const Invocation*>(lua_touserdata(state, 1));
            lua_rawgeti(state, LUA_REGISTRYINDEX, args.ref);
            if (args.count >= 1) PushString(state, args.first);
            if (args.count >= 2) PushString(state, args.second);
            lua_call(state, args.count, 0);
            return 0;
        }, &call);
    }

    static void Subscribe(lua_State* state, int index, int& ref) {
        luaL_checktype(state, index, LUA_TFUNCTION);
        lua_pushvalue(state, index);
        const auto next = luaL_ref(state, LUA_REGISTRYINDEX);
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
        ref = next;
    }

    static int OnConnection(lua_State* state) {
        const auto event = CheckString(state, 1);
        auto& callbacks = Self(state).callbacks_;
        if (event == "connected") Subscribe(state, 2, callbacks.connected);
        else if (event == "disconnected") Subscribe(state, 2, callbacks.disconnected);
        else return luaL_argerror(state, 1, "unknown session event");
        return 0;
    }

    static int Send(lua_State* state) {
        auto& self = Self(state);
        const auto type = CheckString(state, 1);
        const auto payload = CheckString(state, 2);
        lua_pushboolean(state, self.runtime_.Send({self.manifest_.id, type, payload}));
        return 1;
    }

    static int GetText(lua_State* state) {
        const auto& clipboard = Self(state).runtime_.clipboard;
        if (clipboard && clipboard->Available()) PushString(state, clipboard->Text());
        else lua_pushnil(state);
        return 1;
    }

    static int SetText(lua_State* state) {
        auto& runtime = Self(state).runtime_;
        const auto text = CheckString(state, 1);
        bool queued{};
        try { queued = runtime.Connected() && runtime.clipboard && runtime.clipboard->SetText(text); }
        catch (...) {}
        lua_pushboolean(state, queued);
        return 1;
    }

    static int Sha256(lua_State* state) {
        const auto input = CheckString(state, 1);
        if (input.size() > plugin_frame_limit) return luaL_argerror(state, 1, "data exceeds 64 KiB");
        std::array<UCHAR, 32> hash{};
        auto* bytes = reinterpret_cast<PUCHAR>(const_cast<char*>(input.data()));
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes, static_cast<ULONG>(input.size()),
                hash.data(), static_cast<ULONG>(hash.size())) < 0) {
            return luaL_error(state, "SHA-256 failed");
        }
        constexpr char hex[] = "0123456789abcdef";
        std::array<char, 64> output{};
        for (std::size_t i = 0; i < hash.size(); ++i) {
            output[i * 2] = hex[hash[i] >> 4];
            output[i * 2 + 1] = hex[hash[i] & 0xf];
        }
        lua_pushlstring(state, output.data(), output.size());
        return 1;
    }

    static int AddTimer(lua_State* state, bool repeating) {
        auto& self = Self(state);
        const auto ms = luaL_checkinteger(state, 1);
        if (ms < (repeating ? 10 : 1) || ms > 24 * 60 * 60 * 1'000) return luaL_argerror(state, 1, "delay out of range");
        const auto slot = std::ranges::find_if(self.timers_, [](const auto& timer) { return timer.ref == LUA_NOREF; });
        if (slot == self.timers_.end()) {
            lua_pushnil(state);
            return 1;
        }
        int ref{LUA_NOREF};
        Subscribe(state, 2, ref);
        const auto delay = static_cast<ULONGLONG>(ms);
        *slot = {++self.next_timer_, GetTickCount64() + delay, repeating ? delay : 0, ref};
        lua_pushinteger(state, slot->id);
        return 1;
    }

    static int CancelTimer(lua_State* state) {
        const auto id = luaL_checkinteger(state, 1);
        auto& timers = Self(state).timers_;
        const auto timer = std::ranges::find_if(timers, [id](const auto& item) {
            return item.id == id && item.ref != LUA_NOREF;
        });
        if (timer != timers.end()) {
            luaL_unref(state, LUA_REGISTRYINDEX, timer->ref);
            *timer = {};
        }
        return 0;
    }

    static void ReadOnly(lua_State* state) {
        lua_newtable(state);
        lua_newtable(state);
        lua_pushvalue(state, -3);
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, [](lua_State* l) { return luaL_error(l, "read-only API"); });
        lua_setfield(state, -2, "__newindex");
        lua_pushboolean(state, false);
        lua_setfield(state, -2, "__metatable");
        lua_setmetatable(state, -2);
        lua_remove(state, -2);
    }

    static void Namespace(lua_State* state, const char* name, const luaL_Reg* functions) {
        lua_newtable(state);
        luaL_setfuncs(state, functions, 0);
        ReadOnly(state);
        lua_setfield(state, -2, name);
    }

    void BuildApi(lua_State* state) const {
        static const luaL_Reg session[] = {
            {"role", [](lua_State* l) { lua_pushstring(l, Self(l).runtime_.host ? "host" : "viewer"); return 1; }},
            {"connected", [](lua_State* l) { lua_pushboolean(l, Self(l).runtime_.Connected()); return 1; }},
            {"on", OnConnection}, {nullptr, nullptr}
        };
        static const luaL_Reg channel[] = {
            {"send", Send},
            {"on_message", [](lua_State* l) { Subscribe(l, 1, Self(l).callbacks_.message); return 0; }},
            {nullptr, nullptr}
        };
        static const luaL_Reg clipboard[] = {
            {"get_text", GetText}, {"set_text", SetText},
            {"on_change", [](lua_State* l) { Subscribe(l, 1, Self(l).callbacks_.clipboard); return 0; }},
            {nullptr, nullptr}
        };
        static const luaL_Reg crypto[] = {{"sha256", Sha256}, {nullptr, nullptr}};
        static const luaL_Reg timer[] = {
            {"after", [](lua_State* l) { return AddTimer(l, false); }},
            {"every", [](lua_State* l) { return AddTimer(l, true); }},
            {"cancel", CancelTimer}, {nullptr, nullptr}
        };
        static const luaL_Reg log[] = {
            {"info", [](lua_State* l) { Log(Self(l).manifest_.id, "info", CheckString(l, 1)); return 0; }},
            {"warn", [](lua_State* l) { Log(Self(l).manifest_.id, "warn", CheckString(l, 1)); return 0; }},
            {"error", [](lua_State* l) { Log(Self(l).manifest_.id, "error", CheckString(l, 1)); return 0; }},
            {nullptr, nullptr}
        };
        lua_newtable(state);
        Namespace(state, "session", session);
        Namespace(state, "crypto", crypto);
        Namespace(state, "timer", timer);
        if (manifest_.channel) Namespace(state, "channel", channel);
        if (manifest_.clipboard) Namespace(state, "clipboard", clipboard);
        if (manifest_.log) Namespace(state, "log", log);
        ReadOnly(state);
        lua_setglobal(state, "p2p");
    }

    static int Initialize(lua_State* state) {
        const auto script = *static_cast<const std::string_view*>(lua_touserdata(state, 1));
        static const luaL_Reg libraries[] = {{"_G", luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
            {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
        for (const auto& library : libraries) {
            luaL_requiref(state, library.name, library.func, 1);
            lua_pop(state, 1);
        }
        for (const auto* name : {"_G", "load", "loadfile", "dofile", "collectgarbage", "print", "warn",
                "pcall", "xpcall", "rawset", "getmetatable", "setmetatable"}) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        // These functions can perform unbounded work in C without an instruction hook.
        lua_getglobal(state, "table");
        lua_pushnil(state);
        lua_setfield(state, -2, "move");
        lua_pop(state, 1);
        lua_getglobal(state, "string");
        for (const auto* name : {"dump", "find", "match", "gsub", "gmatch"}) {
            lua_pushnil(state);
            lua_setfield(state, -2, name);
        }
        lua_pop(state, 1);

        const auto& self = Self(state);
        self.BuildApi(state);
        if (luaL_loadbufferx(state, script.data(), script.size(), self.manifest_.id.c_str(), "t") != LUA_OK) {
            return lua_error(state);
        }
        lua_pushglobaltable(state);
        ReadOnly(state);
        lua_setupvalue(state, -2, 1);
        lua_call(state, 0, 0);
        return 0;
    }

    const Manifest manifest_;
    PluginRuntime& runtime_;
    std::size_t memory_{};
    lua_State* state_{};
    bool healthy_{true};
    struct {
        int connected{LUA_NOREF};
        int disconnected{LUA_NOREF};
        int message{LUA_NOREF};
        int clipboard{LUA_NOREF};
    } callbacks_;
    std::array<Timer, 32> timers_{};
    lua_Integer next_timer_{};
};

void LoadPlugins(const std::filesystem::path& root, PluginRuntime& runtime,
    std::vector<std::unique_ptr<LuaPlugin>>& plugins) {
    if (!IsPlainPath(root, true)) return;
    // Keep the activation factory scoped to this worker's COM apartment.
    const auto json_factory = winrt::try_get_activation_factory<JsonObject, IJsonObjectStatics>();
    if (!json_factory) throw std::runtime_error("Could not activate plugin manifest parser");
    for (const auto& directory : std::filesystem::directory_iterator(root)) {
        if (!runtime.Connected() || plugins.size() == max_plugins) break;
        if (!IsPlainPath(directory.path(), true)) continue;
        try {
            std::filesystem::path entry;
            auto manifest = ReadManifest(json_factory, directory.path(), runtime.host, entry);
            if (!manifest) continue;
            const auto script = ReadFile(entry, 128 * 1024);
            if (manifest->clipboard && !runtime.clipboard) {
                runtime.clipboard.emplace();
                runtime.clipboard->Poll(GetTickCount64());
            }
            auto plugin = std::make_unique<LuaPlugin>(std::move(*manifest), runtime);
            if (plugin->Load(script)) plugins.push_back(std::move(plugin));
        } catch (const winrt::hresult_error&) {
            Log("loader", "error", "Invalid plugin manifest");
        } catch (const std::exception& error) {
            Log("loader", "error", error.what());
        }
    }
}

} // namespace

struct PluginHost::Impl {
    Impl(bool host, SendFrame send, std::filesystem::path root)
        : host(host), send(std::move(send)), root(std::move(root)) {
        winrt::check_pointer(wake.get());
        worker = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    void Stop() {
        connected = false;
        worker.request_stop();
        SetEvent(wake.get());
        if (worker.joinable()) worker.join();
    }

    void Run(std::stop_token stop) noexcept {
        try {
            while (!stop.stop_requested() && !connected.load()) WaitForSingleObject(wake.get(), INFINITE);
            if (stop.stop_requested()) return;
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
            PluginRuntime runtime(host, send, connected);
            std::vector<std::unique_ptr<LuaPlugin>> plugins;
            LoadPlugins(root.empty() ? PluginDirectory() : root, runtime, plugins);
            for (const auto& plugin : plugins) {
                if (connected.load()) plugin->ConnectionChanged(true);
            }
            while (!stop.stop_requested() && connected.load()) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
                auto now = GetTickCount64();
                if (runtime.clipboard && connected.load() && runtime.clipboard->Poll(now)) {
                    for (const auto& plugin : plugins) plugin->ClipboardChanged(runtime.clipboard->Text());
                }

                std::string frame;
                {
                    const std::lock_guard lock(mutex);
                    if (!frames.empty()) {
                        frame = std::move(frames.front());
                        frames.pop_front();
                    }
                }
                if (const auto packet = DecodePluginPacket(frame); packet && connected.load()) {
                    const auto plugin = std::ranges::find_if(plugins, [&](const auto& entry) { return entry->Info().id == packet->id; });
                    if (plugin != plugins.end()) (*plugin)->Receive(*packet);
                }

                for (const auto& plugin : plugins) {
                    if (connected.load()) plugin->RunTimer(now);
                }
                if (!frame.empty()) continue;
                now = GetTickCount64();
                DWORD wait = runtime.clipboard ? runtime.clipboard->Wait(now) : INFINITE;
                for (const auto& plugin : plugins) wait = std::min(wait, plugin->Wait(now));
                const HANDLE event = wake.get();
                if (MsgWaitForMultipleObjectsEx(1, &event, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) break;
            }
            connected = false;
            if (runtime.clipboard) runtime.clipboard->CancelWrite();
            for (const auto& plugin : plugins) plugin->ConnectionChanged(false);
        } catch (...) {
            Log("host", "error", "Plugin worker stopped");
        }
        connected = false;
        const std::lock_guard lock(mutex);
        frames.clear();
    }

    const bool host;
    const SendFrame send;
    const std::filesystem::path root;
    winrt::handle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic_bool connected{};
    std::mutex mutex;
    std::deque<std::string> frames;
    std::jthread worker;
};

PluginHost::PluginHost(bool host, SendFrame send, std::filesystem::path root)
    : impl_(std::make_unique<Impl>(host, std::move(send), std::move(root))) {}
PluginHost::~PluginHost() { Stop(); }

void PluginHost::SetConnected(bool connected) noexcept {
    impl_->connected = connected;
    SetEvent(impl_->wake.get());
}

void PluginHost::Receive(std::string_view frame) noexcept {
    if (!impl_->connected.load() || !DecodePluginPacket(frame)) return;
    try {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->connected.load() || impl_->frames.size() >= max_pending_frames) return;
        impl_->frames.emplace_back(frame);
        SetEvent(impl_->wake.get());
    } catch (...) {}
}

void PluginHost::Stop() { impl_->Stop(); }

} // namespace p2premote
