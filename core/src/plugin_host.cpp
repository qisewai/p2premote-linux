#include "plugin_host.h"
#include "pty_session.h"
#if defined(_WIN32)
#include "agent_audit.h"
#include "clipboard_bridge.h"
#include "file_clipboard.h"
#include "plugin_settings.h"
#else
#include "plugin_platform.h"
#endif

#include <p2premote/plugin_protocol.h>

#include <lua.hpp>

#if defined(_WIN32)
#include <bcrypt.h>
#include <imm.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
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
std::atomic_uint64_t overlay_revision{1};
}

bool PluginOverlays::Set(PluginOverlayItem item) {
    const std::lock_guard lock(mutex_);
    auto& items = snapshot_.items;
    const auto existing = std::ranges::find(items, item.id, &PluginOverlayItem::id);
    if (existing != items.end()) {
        if (*existing == item) return true;
        *existing = std::move(item);
    } else {
        if (items.size() >= 16 || std::ranges::count(items, item.plugin_id, &PluginOverlayItem::plugin_id) >= 4)
            return false;
        items.push_back(std::move(item));
    }
    snapshot_.revision = overlay_revision.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PluginOverlays::Clear(std::wstring_view plugin_id, std::wstring_view id) {
    const std::lock_guard lock(mutex_);
    if (std::erase_if(snapshot_.items, [&](const auto& item) {
            return item.plugin_id == plugin_id && (id.empty() || item.id == id);
        })) {
        snapshot_.revision = overlay_revision.fetch_add(1, std::memory_order_relaxed);
    }
}

void PluginOverlays::Read(PluginOverlaySnapshot& snapshot) const {
    const std::lock_guard lock(mutex_);
    if (snapshot.revision != snapshot_.revision) snapshot = snapshot_;
}

namespace {

#if defined(_WIN32)
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::IJsonObjectStatics;
using winrt::Windows::Data::Json::IJsonValueStatics;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
#endif

using Tick = std::uint64_t;
using WaitTime = unsigned long;
constexpr auto no_timeout = static_cast<WaitTime>(-1);

Tick TickMilliseconds() {
#if defined(_WIN32)
    return GetTickCount64();
#else
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}
constexpr std::size_t max_plugins = 16;
constexpr std::size_t max_pending_frames = 512;
constexpr std::size_t lua_memory_limit = 2 * 1024 * 1024;
constexpr int lua_instruction_limit = 200'000;
constexpr std::size_t max_json_bytes = 512 * 1024;
constexpr int max_json_depth = 16;
constexpr std::size_t max_fs_read_chunk = 60 * 1024;
constexpr std::size_t max_fs_list_entries = 1500;

struct Manifest {
    std::string id;
    bool channel{};
    bool clipboard{};
    bool file_clipboard{};
    bool log{};
    bool ui{};
    bool input{};
    bool config{};
    bool video{};
    bool fs{};
    bool pty{};
    bool overlay{};
    bool sys{};
    PluginOverlayItem overlay_defaults;
};

void Log(std::string_view id, std::string_view level, std::string_view message) noexcept {
    try {
        std::string line("P2PRemote plugin [");
        line.append(id).append("] ").append(level).append(" ").append(message.substr(0, 256));
        std::ranges::replace(line, '\r', ' ');
        std::ranges::replace(line, '\n', ' ');
        line += '\n';
#if defined(_WIN32)
        OutputDebugStringA(line.c_str());
#else
        std::fputs(line.c_str(), stderr);
#endif
    } catch (...) {}
}

#if defined(_WIN32)
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

#else
std::filesystem::path PluginDirectory() {
    const auto local = (QCoreApplication::applicationDirPath() + QStringLiteral("/plugins")).toStdString();
#if defined(P2PREMOTE_LINUX_PLUGIN_DIR)
    std::error_code error;
    if (!std::filesystem::is_directory(local, error)) return P2PREMOTE_LINUX_PLUGIN_DIR;
#endif
    return local;
}

bool IsPlainPath(const std::filesystem::path& path, bool directory) noexcept {
    std::error_code error;
    const auto info = std::filesystem::symlink_status(path, error);
    return directory ? std::filesystem::is_directory(info) : std::filesystem::is_regular_file(info);
}

#endif
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

std::optional<Manifest> ReadManifest(
#if defined(_WIN32)
    const IJsonObjectStatics& json_factory,
#endif
    const std::filesystem::path& directory, bool host, std::filesystem::path& entry) {
#if defined(_WIN32)
    const auto json = json_factory.Parse(winrt::to_hstring(ReadFile(directory / "plugin.json", 16 * 1024)));
    if (!json.GetNamedBoolean(L"enabled", false)) return std::nullopt;
    Manifest manifest{winrt::to_string(json.GetNamedString(L"id"))};
#else
    const auto raw = ReadFile(directory / "plugin.json", 16 * 1024);
    const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(raw)).object();
    if (!json.value("enabled").toBool()) return std::nullopt;
    Manifest manifest{};
    manifest.id = json.value("id").toString().toStdString();
#endif
    if (!IsPluginIdentifier(manifest.id) || manifest.id == workspace_protocol_id || directory.filename() != std::filesystem::path(manifest.id)) {
        throw std::runtime_error("Plugin ID must match its directory");
    }
    bool selected{};
#if defined(_WIN32)
    for (const auto& role : json.GetNamedArray(L"roles")) {
        const auto name = role.GetString();
        if (name != L"host" && name != L"viewer") throw std::runtime_error("Unknown plugin role");
        selected |= name == (host ? L"host" : L"viewer");
    }
#else
    for (const auto& role : json.value("roles").toArray()) {
        const auto name = role.toString();
        if (name != "host" && name != "viewer") throw std::runtime_error("Unknown plugin role");
        selected |= name == (host ? "host" : "viewer");
    }
#endif
    if (!selected) return std::nullopt;
#if defined(_WIN32)
    for (const auto& permission : json.GetNamedArray(L"permissions")) {
        const auto name = winrt::to_string(permission.GetString());
#else
    for (const auto& permission : json.value("permissions").toArray()) {
        const auto name = permission.toString().toStdString();
#endif
        if (name == "channel") manifest.channel = true;
        else if (name == "clipboard") manifest.clipboard = true;
        else if (name == "file_clipboard") manifest.file_clipboard = true;
        else if (name == "log") manifest.log = true;
        else if (name == "ui") manifest.ui = true;
        else if (name == "input") manifest.input = true;
        else if (name == "config") manifest.config = true;
        else if (name == "video") manifest.video = true;
        else if (name == "fs") manifest.fs = true;
        else if (name == "pty") manifest.pty = true;
        else if (name == "overlay") manifest.overlay = true;
        else if (name == "sys") manifest.sys = true;
        else throw std::runtime_error("Unknown plugin permission");
    }
    if (manifest.overlay) {
        auto& item = manifest.overlay_defaults;
        item.plugin_id.assign(manifest.id.begin(), manifest.id.end());
#if defined(_WIN32)
        item.title = json.GetNamedString(L"name", winrt::to_hstring(manifest.id));
        if (const auto settings = json.GetNamedObject(L"overlay", nullptr)) {
            item.anchor = ParseOverlayAnchor(winrt::to_string(settings.GetNamedString(L"anchor", L"bottom-right")))
                .value_or(OverlayAnchor::bottom_right);
            item.priority = static_cast<int>(std::clamp(settings.GetNamedNumber(L"priority", 100), -1000.0, 1000.0));
            item.default_visible = settings.GetNamedBoolean(L"default_visible", true);
        }
    }
    const auto filename = winrt::to_string(json.GetNamedString(L"entry"));
#else
        item.title = json.value("name").toString(QString::fromStdString(manifest.id)).toStdWString();
        const auto settings = json.value("overlay").toObject();
        item.anchor = ParseOverlayAnchor(settings.value("anchor").toString().toStdString())
            .value_or(OverlayAnchor::bottom_right);
        item.priority = std::clamp(settings.value("priority").toInt(100), -1000, 1000);
        item.default_visible = settings.value("default_visible").toBool(true);
    }
    const auto filename = json.value("entry").toString().toStdString();
#endif
    if (!filename.ends_with(".lua") || !IsPluginIdentifier(std::string_view(filename).substr(0, filename.size() - 4))) {
        throw std::runtime_error("Plugin entry must be a Lua file in its own directory");
    }
    entry = directory / filename;
    return manifest;
}

constexpr Tick agent_approval_timeout_ms = 15'000;

struct AgentRequest {
    enum class Decision { waiting, approved, denied, revoked, cancelled };
    std::uint32_t id{};
    Tick started{};
    AgentTool tool{};
    Decision decision{Decision::waiting};
    std::string image;
};

// Lua menu requests use the lower half of the sequence space.
constexpr std::uint32_t local_agent_id_bit = 0x80000000u;
constexpr std::size_t agent_header_bytes = 4;
constexpr std::size_t agent_summary_limit = 4096;
constexpr std::size_t agent_image_chunk = 16 * 1024;
constexpr std::string_view png_signature = "\x89PNG\r\n\x1a\n";

bool ValidAgentImage(std::string_view image) {
    if (image.size() < 45 || image.size() > agent_image_limit || !image.starts_with(png_signature) ||
        image.substr(12, 4) != "IHDR" || image.substr(image.size() - 8, 4) != "IEND") return false;
    const auto dimension = [&](std::size_t offset) {
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) value = (value << 8) | static_cast<unsigned char>(image[offset + i]);
        return value;
    };
    return dimension(16) > 0 && dimension(16) <= 1280 && dimension(20) > 0 && dimension(20) <= 720;
}

struct LocalAgentQuery {
    std::uint32_t id{};
    bool sent{};
    AgentTool tool{};
    std::uint32_t chunk{};
    AgentSummary result;
};

std::string AgentHeader(std::uint32_t id) {
    std::string bytes(agent_header_bytes, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(id >> (i * 8));
    return bytes;
}

std::uint32_t AgentHeaderId(std::string_view bytes) {
    std::uint32_t id{};
    for (std::size_t i = 0; i < agent_header_bytes; ++i) {
        id |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    }
    return id;
}

class Workspace {
public:
    explicit Workspace(PluginHost::Notify signal) : signal_(std::move(signal)) {}

    unsigned AddMenu(PluginMenuItem item) {
        static std::atomic_uint next_id{1};
        item.id = next_id.fetch_add(1);
        const auto id = item.id;
        {
            const std::lock_guard lock(mutex);
            menu.push_back(std::move(item));
        }
        Signal();
        return id;
    }

    bool CheckMenu(unsigned id, bool value) {
        const std::lock_guard lock(mutex);
        const auto item = std::ranges::find(menu, id, &PluginMenuItem::id);
        if (item == menu.end() || !item->checked) return false;
        item->checked = value;
        return true;
    }

    void RemoveMenu(unsigned id) {
        {
            const std::lock_guard lock(mutex);
            std::erase_if(menu, [id](const auto& item) { return item.id == id; });
        }
        Signal();
    }

    bool Request(PluginAction action) {
        {
            const std::lock_guard lock(mutex);
            if (actions.size() >= 32) return false;
            actions.push_back(std::move(action));
        }
        Signal();
        return true;
    }

    std::uint32_t NextDialogId() noexcept {
        static std::atomic_uint32_t next_id{1};
        return next_id.fetch_add(1);
    }

    bool PushUiCommand(PluginUiCommand command) {
        {
            const std::lock_guard lock(mutex);
            if (ui_commands.size() >= 256) return false;
            ui_commands.push_back(std::move(command));
        }
        Signal();
        return true;
    }

    PluginMetrics ReadMetrics() const {
        const std::lock_guard lock(mutex);
        return metrics;
    }

    std::optional<std::pair<bool, bool>> ReadLockState() const {
        const std::lock_guard lock(mutex);
        return lock_state;
    }

    void Signal() const noexcept {
        try { if (signal_) signal_(); } catch (...) {}
    }

    mutable std::mutex mutex;
    std::vector<PluginMenuItem> menu;
    std::deque<unsigned> clicks;
    std::vector<PluginAction> actions;
    std::deque<PluginUiCommand> ui_commands;
    std::deque<PluginUiEvent> ui_events;
    PluginMetrics metrics;
    std::optional<AgentRequest> agent_request;
    std::optional<LocalAgentQuery> local_agent_query;
    std::uint32_t cancel_agent_query{};
    bool agent_paused{};
    std::optional<std::pair<bool, bool>> lock_state;
    bool lock_state_dirty{};
    std::atomic_uint lock_key_consumers{};

private:
    PluginHost::Notify signal_;
};

#if defined(_WIN32)
std::optional<std::wstring> Utf8ToWide(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) return std::nullopt;
    if (text.empty()) return std::wstring{};
    if (text.size() > 32'768) return std::nullopt;
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), wide.data(), needed) != needed) {
        return std::nullopt;
    }
    return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

#else
std::optional<std::wstring> Utf8ToWide(std::string_view text) {
    if (text.find('\0') != std::string_view::npos || text.size() > 32768) return std::nullopt;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString output = decoder(QByteArrayView(text.data(), text.size()));
    if (decoder.hasError()) return std::nullopt;
    return output.toStdWString();
}

#endif
#if defined(_WIN32)
std::string Win32ErrorUtf8(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND: return "文件不存在";
    case ERROR_PATH_NOT_FOUND: return "路径不存在";
    case ERROR_ACCESS_DENIED: return "拒绝访问";
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return "目标已存在";
    case ERROR_SHARING_VIOLATION: return "文件正在被其他程序占用";
    case ERROR_DISK_FULL: return "磁盘空间不足";
    case ERROR_INVALID_NAME: return "文件名包含非法字符";
    default: return "系统错误 (" + std::to_string(code) + ")";
    }
}

std::optional<std::filesystem::path> SafeFsPath(std::string_view utf8) {
    const auto wide = Utf8ToWide(utf8);
    if (!wide || wide->empty()) return std::nullopt;
    if (wide->starts_with(L"\\\\") || wide->starts_with(L"//")) return std::nullopt;
    std::filesystem::path path(*wide);
    path = path.lexically_normal();
    if (path.empty() || !path.is_absolute()) return std::nullopt;
    return path;
}

#else
std::optional<std::filesystem::path> SafeFsPath(std::string_view utf8) {
    std::filesystem::path path(utf8);
    if (!path.is_absolute() || !Utf8ToWide(utf8) || utf8.starts_with("//")) return std::nullopt;
    return path.lexically_normal();
}

#endif
bool ValidLeafName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 240 || name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return name.back() != ' ' && name.back() != '.';
}

#if defined(_WIN32)
std::string ParentPathUtf8(const std::filesystem::path& normalized) {
    auto p = normalized;
    if (p.has_relative_path() && p.filename().empty()) p = p.parent_path();
    const auto parent = p.parent_path();
    if (parent.empty() || parent == p) return {};
    return WideToUtf8(parent.native());
}

std::int64_t FileTimeToUnixSeconds(const FILETIME& ft) noexcept {
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t epoch_diff = 116444736000000000ULL;
    if (uli.QuadPart < epoch_diff) return 0;
    return static_cast<std::int64_t>((uli.QuadPart - epoch_diff) / 10000000ULL);
}

lua_Integer FileTimeStamp(const FILETIME& time) noexcept {
    ULARGE_INTEGER stamp{};
    stamp.LowPart = time.dwLowDateTime;
    stamp.HighPart = time.dwHighDateTime;
    return static_cast<lua_Integer>(stamp.QuadPart);
}

bool PlainAncestors(const std::filesystem::path& path) {
    for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        if (!IsPlainPath(parent, true)) return false;
        if (!parent.has_relative_path()) break;
    }
    return true;
}

std::string FileTimeToLocalText(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

#endif
void AppendJsonString(std::string& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            if (c < 0x20) {
                out.append("\\u00");
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xf]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

bool LuaValueToJson(lua_State* state, int index, std::string& out, int depth = 0) {
    if (depth > max_json_depth || out.size() > max_json_bytes) return false;
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        out.append("null");
        return out.size() <= max_json_bytes;
    case LUA_TBOOLEAN:
        out.append(lua_toboolean(state, index) ? "true" : "false");
        return out.size() <= max_json_bytes;
    case LUA_TNUMBER: {
        char buf[64]{};
        if (lua_isinteger(state, index)) {
            const auto value = lua_tointeger(state, index);
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec != std::errc{}) return false;
            out.append(buf, ptr);
        } else {
            const auto value = lua_tonumber(state, index);
            if (!std::isfinite(value)) return false;
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec != std::errc{}) return false;
            out.append(buf, ptr);
        }
        return out.size() <= max_json_bytes;
    }
    case LUA_TSTRING: {
        std::size_t len{};
        const auto* data = lua_tolstring(state, index, &len);
        if (len > max_json_bytes) return false;
        AppendJsonString(out, {data, len});
        return out.size() <= max_json_bytes;
    }
    case LUA_TTABLE: {
        if (!lua_checkstack(state, 4)) return false;
        bool is_array = true;
        lua_Integer max_key = 0;
        std::size_t count = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2)) {
                const auto k = lua_tointeger(state, -2);
                if (k >= 1) {
                    if (k > max_key) max_key = k;
                    ++count;
                } else {
                    is_array = false;
                }
            } else {
                is_array = false;
            }
            lua_pop(state, 1);
        }
        if (count > 0 && is_array && static_cast<std::size_t>(max_key) == count) {
            out.push_back('[');
            for (lua_Integer i = 1; i <= max_key; ++i) {
                if (i > 1) out.push_back(',');
                lua_rawgeti(state, index, i);
                const bool ok = LuaValueToJson(state, -1, out, depth + 1);
                lua_pop(state, 1);
                if (!ok) return false;
            }
            out.push_back(']');
        } else {
            out.push_back('{');
            bool first = true;
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                if (lua_type(state, -2) == LUA_TSTRING) {
                    if (!first) out.push_back(',');
                    first = false;
                    std::size_t klen{};
                    const auto* kdata = lua_tolstring(state, -2, &klen);
                    AppendJsonString(out, {kdata, klen});
                    out.push_back(':');
                    if (!LuaValueToJson(state, -1, out, depth + 1)) {
                        lua_pop(state, 2);
                        return false;
                    }
                }
                lua_pop(state, 1);
            }
            out.push_back('}');
        }
        return out.size() <= max_json_bytes;
    }
    default:
        return false;
    }
}

class JsonParser {
public:
    JsonParser(lua_State* state, std::string_view input) : state_(state), input_(input) {}

    bool Parse() {
        SkipWs();
        if (!ParseValue(0)) return false;
        SkipWs();
        if (pos_ != input_.size()) {
            lua_pop(state_, 1);
            return false;
        }
        return true;
    }

private:
    void SkipWs() noexcept {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }

    static void AppendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7f) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0x10ffff) {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool ParseHex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > input_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            std::uint32_t v{};
            if (c >= '0' && c <= '9') v = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
            out = (out << 4) | v;
        }
        return true;
    }

    bool ParseStringRaw(std::string& out) {
        if (pos_ >= input_.size() || input_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) return false;
            const char esc = input_[pos_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp{};
                if (!ParseHex4(cp)) return false;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') return false;
                    pos_ += 2;
                    std::uint32_t low{};
                    if (!ParseHex4(low) || low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + (((cp - 0xd800) << 10) | (low - 0xdc00));
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    return false;
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ParseNumber() {
        const auto start = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '0') {
            ++pos_;
        } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        } else {
            return false;
        }
        bool is_float = false;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            is_float = true;
            ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') return false;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') return false;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        const auto token = input_.substr(start, pos_ - start);
        if (!is_float) {
            lua_Integer iv{};
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), iv);
            if (ec == std::errc{} && ptr == token.data() + token.size()) {
                lua_pushinteger(state_, iv);
                return true;
            }
        }
        double dv{};
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), dv);
        if (ec != std::errc{} || ptr != token.data() + token.size() || !std::isfinite(dv)) return false;
        lua_pushnumber(state_, dv);
        return true;
    }

    bool ParseValue(int depth) {
        if (depth > max_json_depth || !lua_checkstack(state_, 4) || pos_ >= input_.size()) return false;
        const char c = input_[pos_];
        if (c == 'n') {
            if (input_.substr(pos_, 4) != "null") return false;
            pos_ += 4;
            lua_pushnil(state_);
            return true;
        }
        if (c == 't') {
            if (input_.substr(pos_, 4) != "true") return false;
            pos_ += 4;
            lua_pushboolean(state_, 1);
            return true;
        }
        if (c == 'f') {
            if (input_.substr(pos_, 5) != "false") return false;
            pos_ += 5;
            lua_pushboolean(state_, 0);
            return true;
        }
        if (c == '"') {
            std::string str;
            if (!ParseStringRaw(str)) return false;
            lua_pushlstring(state_, str.empty() ? "" : str.data(), str.size());
            return true;
        }
        if (c == '[') {
            ++pos_;
            lua_newtable(state_);
            SkipWs();
            if (pos_ < input_.size() && input_[pos_] == ']') {
                ++pos_;
                return true;
            }
            lua_Integer index = 1;
            while (true) {
                SkipWs();
                if (!ParseValue(depth + 1)) {
                    lua_pop(state_, 1);
                    return false;
                }
                lua_rawseti(state_, -2, index++);
                SkipWs();
                if (pos_ >= input_.size()) {
                    lua_pop(state_, 1);
                    return false;
                }
                if (input_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                if (input_[pos_] != ',') {
                    lua_pop(state_, 1);
                    return false;
                }
                ++pos_;
            }
        }
        if (c == '{') {
            ++pos_;
            lua_newtable(state_);
            SkipWs();
            if (pos_ < input_.size() && input_[pos_] == '}') {
                ++pos_;
                return true;
            }
            std::string key;
            while (true) {
                SkipWs();
                if (!ParseStringRaw(key)) {
                    lua_pop(state_, 1);
                    return false;
                }
                SkipWs();
                if (pos_ >= input_.size() || input_[pos_] != ':') {
                    lua_pop(state_, 1);
                    return false;
                }
                ++pos_;
                SkipWs();
                lua_pushlstring(state_, key.empty() ? "" : key.data(), key.size());
                if (!ParseValue(depth + 1)) {
                    lua_pop(state_, 2);
                    return false;
                }
                lua_rawset(state_, -3);
                SkipWs();
                if (pos_ >= input_.size()) {
                    lua_pop(state_, 1);
                    return false;
                }
                if (input_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                if (input_[pos_] != ',') {
                    lua_pop(state_, 1);
                    return false;
                }
                ++pos_;
            }
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
        return false;
    }

    lua_State* state_;
    std::string_view input_;
    std::size_t pos_{};
};

std::optional<std::wstring> UiText(std::string_view text, std::size_t limit) {
    if (text.empty() || text.size() > 4 * limit || text.find('\0') != std::string_view::npos) return std::nullopt;
#if defined(_WIN32)
    auto converted = winrt::to_hstring(text);
    if (converted.size() > limit) return std::nullopt;
    return std::wstring(converted);
#else
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString utf16 = decoder(QByteArrayView(text.data(), text.size()));
    if (decoder.hasError() || utf16.size() > static_cast<qsizetype>(limit)) return std::nullopt;
    return std::wstring(utf16.utf16(), utf16.utf16() + utf16.size());
#endif

}

#if defined(_WIN32)
// Windows IMM uses these WM_IME_CONTROL commands for the foreground input context.
constexpr WPARAM ime_get_open = 0x0005;
constexpr WPARAM ime_set_open = 0x0006;

std::optional<bool> ForegroundIme(std::optional<bool> set = {}) noexcept {
    const auto foreground = GetForegroundWindow();
    if (!foreground) return std::nullopt;
    const auto thread = GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO state{sizeof(state)};
    if (!GetGUIThreadInfo(thread, &state) || !state.hwndFocus) return std::nullopt;
    const auto ime = ImmGetDefaultIMEWnd(state.hwndFocus);
    if (!ime) return std::nullopt;
    DWORD_PTR result{};
    if (!SendMessageTimeoutW(ime, WM_IME_CONTROL, set ? ime_set_open : ime_get_open,
            set.value_or(false), SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result)) return std::nullopt;
    if (set) return result == 0 ? set : std::nullopt;
    return result != 0;
}

bool SetLockKey(WORD key, bool target) noexcept {
    if (static_cast<bool>(GetKeyState(key) & 1) == target) return true;
    if (GetAsyncKeyState(key) & 0x8000) return false;
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const auto count = SendInput(2, inputs, sizeof(INPUT));
    if (count == 1) SendInput(1, &inputs[1], sizeof(INPUT));
    return count == 2;
}

UINT NamedVirtualKey(std::string_view name) noexcept {
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) {
        return static_cast<UINT>(name[0]);
    }
    const std::pair<std::string_view, UINT> names[] = {{"Ctrl", VK_LCONTROL}, {"Alt", VK_LMENU}, {"Shift", VK_LSHIFT},
        {"Win", VK_LWIN}, {"Esc", VK_ESCAPE}, {"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Delete", VK_DELETE},
        {"Space", VK_SPACE}, {"Backspace", VK_BACK}, {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP},
        {"Down", VK_DOWN}, {"Home", VK_HOME}, {"End", VK_END}, {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3},
        {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9},
        {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12}};
    for (const auto& [label, key] : names) if (name == label) return key;
    return 0;
}

#else
std::optional<bool> ForegroundIme(std::optional<bool> = {}) noexcept { return std::nullopt; }
std::uint16_t NamedVirtualKey(std::string_view name) noexcept {
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) {
        return static_cast<std::uint16_t>(name.front());
    }
    // InputCommand::shortcut uses the Windows virtual-key encoding on the wire.
    constexpr std::pair<std::string_view, std::uint16_t> keys[] = {{"Ctrl", 0xa2}, {"Alt", 0xa4}, {"Shift", 0xa0},
        {"Win", 0x5b}, {"Esc", 0x1b}, {"Enter", 0x0d}, {"Tab", 0x09}, {"Delete", 0x2e}, {"Space", 0x20},
        {"Backspace", 0x08}, {"Left", 0x25}, {"Right", 0x27}, {"Up", 0x26}, {"Down", 0x28},
        {"Home", 0x24}, {"End", 0x23}, {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73},
        {"F5", 0x74}, {"F6", 0x75}, {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79},
        {"F11", 0x7a}, {"F12", 0x7b}};
    for (const auto& [label, key] : keys) if (name == label) return key;
    return 0;
}

#endif
std::string Base64Encode(std::string_view input) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
            (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back(table[(n >> 6) & 0x3f]);
        out.push_back(table[n & 0x3f]);
        i += 3;
    }
    if (i < input.size()) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = (i + 1 < input.size()) ? bytes[i + 1] : 0;
        const std::uint32_t n = (b0 << 16) | (b1 << 8);
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back(i + 1 < input.size() ? table[(n >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> Base64Decode(std::string_view input) {
    auto decode_char = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve((input.size() * 3) / 4);
    std::uint32_t val = 0;
    int bits = -8;
    for (unsigned char c : input) {
        if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            if (c == '=') break;
            continue;
        }
        const int d = decode_char(c);
        if (d < 0) return std::nullopt;
        val = (val << 6) | static_cast<std::uint32_t>(d);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

class LuaPlugin;

class PluginWake {
public:
#if defined(_WIN32)
    PluginWake() { winrt::check_pointer(event_.get()); }
    void Signal() { SetEvent(event_.get()); }
    void WaitUntilConnected() { WaitForSingleObject(event_.get(), INFINITE); }
    bool Wait(WaitTime delay) {
        const HANDLE event = event_.get();
        return MsgWaitForMultipleObjectsEx(1, &event, delay, QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_FAILED;
    }
    HANDLE Native() const { return event_.get(); }

private:
    winrt::handle event_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
#else
    void Signal() {
        {
            const std::lock_guard lock(mutex_);
            ready_ = true;
        }
        changed_.notify_one();
    }
    void WaitUntilConnected() { Wait(no_timeout); }
    bool Wait(WaitTime delay) {
        std::unique_lock lock(mutex_);
        if (delay == no_timeout) changed_.wait(lock, [this] { return ready_; });
        else changed_.wait_for(lock, std::chrono::milliseconds(delay), [this] { return ready_; });
        ready_ = false;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_{};
#endif
};

class PluginRuntime {
public:
    PluginRuntime(bool host, const PluginHost::SendFrame& send, const std::atomic_bool& connected,
        Workspace& workspace, std::filesystem::path config_root, PluginWake& wake, bool local_overlay,
        PluginOverlays& overlays, bool diagnostics_only)
        : host(host), local_overlay(local_overlay), diagnostics_only(diagnostics_only),
          workspace(workspace), config_root(std::move(config_root)),
          wake(wake), overlays(overlays), send_(send), connected_(connected) {}

    bool Connected() const noexcept { return connected_.load(); }

    bool Send(PluginPacket packet) noexcept {
        if (!Connected()) return false;
        const auto now = TickMilliseconds();
        if (now >= send_period_end_) {
            send_period_end_ = now + 100;
            send_bytes_left_ = 16 * 1024 * 1024;
            send_frames_left_ = 512;
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
    const bool local_overlay;
    const bool diagnostics_only;
    Workspace& workspace;
    const std::filesystem::path config_root;
    PluginWake& wake;
    PluginOverlays& overlays;
    std::optional<ClipboardBridge> clipboard;
    std::optional<FileClipboard> file_clipboard;
    LuaPlugin* file_clipboard_owner{};

private:
    const PluginHost::SendFrame& send_;
    const std::atomic_bool& connected_;
    Tick send_period_end_{};
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

    ~LuaPlugin() {
        if (manifest_.overlay && !runtime_.host) runtime_.overlays.Clear(manifest_.overlay_defaults.plugin_id);
        CaptureLockKeys(false);
        ClearMenu();
        ClearDialogs();
        ClearFiles();
        ClearPtys();
        lua_close(state_);
    }
    LuaPlugin(const LuaPlugin&) = delete;
    LuaPlugin& operator=(const LuaPlugin&) = delete;

    bool Load(std::string_view script) { return Run(Initialize, &script); }
    const Manifest& Info() const noexcept { return manifest_; }

    void ConnectionChanged(bool connected) {
        Call(connected ? callbacks_.connected : callbacks_.disconnected);
        if (!connected) {
            ClearDialogs();
            ClearFiles();
            ClearPtys();
        }
    }

    void Receive(PluginPacket packet) {
        if (manifest_.channel) Call(callbacks_.message, 2, packet.type, packet.payload);
    }

    void ClipboardChanged(std::string_view text) {
        if (manifest_.clipboard) Call(callbacks_.clipboard, 1, text);
    }

    void ClipboardFilesChanged(const std::vector<LocalClipboardFile>& files, std::string_view issue) {
        if (!healthy_ || callbacks_.clipboard_files == LUA_NOREF) return;
        struct Change {
            int ref;
            const std::vector<LocalClipboardFile>& files;
            std::string_view issue;
        } change{callbacks_.clipboard_files, files, issue};
        Run([](lua_State* state) {
            const auto& args = *static_cast<const Change*>(lua_touserdata(state, 1));
            lua_rawgeti(state, LUA_REGISTRYINDEX, args.ref);
            lua_createtable(state, static_cast<int>(args.files.size()), 0);
            for (std::size_t i = 0; i < args.files.size(); ++i) {
                lua_createtable(state, 0, 2);
                PushString(state, args.files[i].path);
                lua_setfield(state, -2, "path");
                PushString(state, args.files[i].name);
                lua_setfield(state, -2, "name");
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            if (args.issue.empty()) lua_pushnil(state);
            else PushString(state, args.issue);
            lua_call(state, 2, 0);
            return 0;
        }, &change);
    }

    void ClipboardError(std::string_view error) {
        Call(callbacks_.clipboard_error, 1, error);
    }

    void ClipboardRead(ClipboardReadRequest request) {
        if (healthy_ && callbacks_.clipboard_read != LUA_NOREF && Run([](lua_State* state) {
            const auto& req = *static_cast<const ClipboardReadRequest*>(lua_touserdata(state, 1));
            lua_rawgeti(state, LUA_REGISTRYINDEX, Self(state).callbacks_.clipboard_read);
            lua_pushinteger(state, req.id);
            lua_pushinteger(state, req.offer);
            lua_pushinteger(state, req.index);
            lua_pushinteger(state, static_cast<lua_Integer>(req.offset));
            lua_pushinteger(state, req.size);
            lua_call(state, 5, 0);
            return 0;
        }, &request)) return;
        runtime_.file_clipboard->Reply(request.id, std::nullopt);
    }

    void LockStateChanged() {
        if (manifest_.input) Call(callbacks_.lock_state);
    }

    void Click(unsigned id) {
        const auto item = std::ranges::find(menu_, id, &MenuCallback::id);
        if (item != menu_.end()) Call(item->ref);
    }

    bool HandleUiEvent(const PluginUiEvent& ev) {
        if (!healthy_ || !manifest_.ui) return false;
        const auto slot = std::ranges::find(dialogs_, ev.dialog_id, &DialogCallback::id);
        if (slot == dialogs_.end() || slot->ref == LUA_NOREF) return false;
        const int ref = slot->ref;
        const bool closing = ev.event == "close" && ev.control.empty();
        if (closing) *slot = {};
        struct UiCall {
            int ref;
            const PluginUiEvent* ev;
        } call{ref, &ev};
        Run([](lua_State* state) {
            const auto& args = *static_cast<const UiCall*>(lua_touserdata(state, 1));
            lua_rawgeti(state, LUA_REGISTRYINDEX, args.ref);
            bool parsed_table = false;
            if (!args.ev->json.empty() && args.ev->json.size() <= max_json_bytes) {
                JsonParser parser(state, args.ev->json);
                if (parser.Parse()) {
                    if (lua_istable(state, -1)) parsed_table = true;
                    else lua_pop(state, 1);
                }
            }
            if (!parsed_table) lua_createtable(state, 0, 4);
            lua_pushinteger(state, static_cast<lua_Integer>(args.ev->dialog_id));
            lua_setfield(state, -2, "dialog");
            PushString(state, args.ev->control);
            lua_setfield(state, -2, "control");
            PushString(state, args.ev->event);
            lua_setfield(state, -2, "event");
            lua_call(state, 1, 0);
            return 0;
        }, &call);
        if (closing) luaL_unref(state_, LUA_REGISTRYINDEX, ref);
        return true;
    }

    void RunTimer(Tick now) {
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

    WaitTime Wait(Tick now) const noexcept {
        auto delay = no_timeout;
        if (!healthy_) return delay;
        for (const auto& timer : timers_) {
            if (timer.ref != LUA_NOREF) delay = std::min(delay, timer.due <= now ? 0UL : static_cast<WaitTime>(timer.due - now));
        }
        return delay;
    }

private:
    struct MenuCallback {
        unsigned id{};
        int ref{LUA_NOREF};
    };

    struct DialogCallback {
        std::uint32_t id{};
        int ref{LUA_NOREF};
    };

#if defined(_WIN32)
    struct FileSlot {
        lua_Integer id{};
        HANDLE handle{INVALID_HANDLE_VALUE};
        bool writable{};
        std::filesystem::path target_path;
        std::filesystem::path temp_path;
        BCRYPT_HASH_HANDLE hash{};
        std::string hash_hex;
    };
#else
    using FileSlot = LinuxPluginFile;
#endif

    struct PtySlot {
        std::uint32_t id{};
        std::unique_ptr<PtySession> session;
    };
    void ClearMenu() noexcept {
        for (auto& item : menu_) {
            if (item.id) {
                try { runtime_.workspace.RemoveMenu(item.id); } catch (...) {}
                item.id = 0;
            }
        }
    }

    void ClearDialogs() noexcept {
        for (auto& dialog : dialogs_) {
            if (dialog.id) {
                try { runtime_.workspace.PushUiCommand({dialog.id, "close", "{}"}); } catch (...) {}
                if (state_ && dialog.ref != LUA_NOREF) luaL_unref(state_, LUA_REGISTRYINDEX, dialog.ref);
                dialog = {};
            }
        }
    }

#if defined(_WIN32)
    void CloseFileSlot(FileSlot& slot) noexcept {
        if (slot.hash) {
            BCryptDestroyHash(slot.hash);
            slot.hash = nullptr;
        }
        if (slot.handle != INVALID_HANDLE_VALUE) {
            CloseHandle(slot.handle);
            slot.handle = INVALID_HANDLE_VALUE;
        }
        if (slot.writable && !slot.temp_path.empty()) {
            DeleteFileW(slot.temp_path.c_str());
        }
        slot = {};
    }

#else
    void CloseFileSlot(FileSlot& slot) noexcept { slot.clear(); }

#endif
    void ClearFiles() noexcept {
        for (auto& slot : files_) {
            if (slot.id) CloseFileSlot(slot);
        }
    }

    void ClearPtys() noexcept {
        for (auto& slot : ptys_) {
            if (slot.session) {
                try { slot.session->Close(); } catch (...) {}
            }
            slot = {};
        }
    }
    void CaptureLockKeys(bool capture) noexcept {
        if (capture == captures_lock_keys_) return;
        captures_lock_keys_ = capture;
        if (capture) ++runtime_.workspace.lock_key_consumers;
        else --runtime_.workspace.lock_key_consumers;
    }

    struct Timer {
        lua_Integer id{};
        Tick due{};
        Tick interval{};
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
        CaptureLockKeys(false);
        ClearMenu();
        ClearDialogs();
        ClearFiles();
        ClearPtys();
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

    static std::uint32_t ClipboardId(lua_State* state) {
        const auto id = luaL_checkinteger(state, 1);
        if (id <= 0 || id > UINT32_MAX) luaL_argerror(state, 1, "invalid clipboard ID");
        return static_cast<std::uint32_t>(id);
    }

    static bool ReadFileDescriptors(lua_State* state, std::vector<ClipboardFile>& files) {
        const auto count = lua_rawlen(state, 2);
        if (!count || count > max_clipboard_files) return false;
        files.reserve(count);
        for (std::size_t i = 1; i <= count; ++i) {
            if (lua_rawgeti(state, 2, static_cast<lua_Integer>(i)) != LUA_TTABLE) return false;
            lua_rawgeti(state, -1, 1);
            lua_rawgeti(state, -2, 2);
            lua_rawgeti(state, -3, 3);
            if (lua_type(state, -3) != LUA_TSTRING || !lua_isinteger(state, -2) || !lua_isboolean(state, -1)) return false;
            std::size_t length{};
            const auto* name = lua_tolstring(state, -3, &length);
            if (!length || length >= 4 * 260) return false;
            const auto size = lua_tointeger(state, -2);
            if (size < 0) return false;
#if defined(_WIN32)
            auto wide = Utf8ToWide({name, length});
            if (!wide) return false;
            files.push_back({std::move(*wide), static_cast<std::uint64_t>(size), lua_toboolean(state, -1) != 0});
#else
            files.push_back({std::string(name, length), static_cast<std::uint64_t>(size), lua_toboolean(state, -1) != 0});
#endif
            lua_pop(state, 4);
        }
        return true;
    }

    static int PublishFiles(lua_State* state) {
        auto& self = Self(state);
        auto& runtime = self.runtime_;
        const auto offer = ClipboardId(state);
        luaL_checktype(state, 2, LUA_TTABLE);
        bool published{};
#if !defined(_WIN32)
        std::string_view error;
#endif
        try {
            std::vector<ClipboardFile> files;
            if (runtime.Connected() && runtime.file_clipboard && ReadFileDescriptors(state, files)) {
#if defined(_WIN32)
                published = runtime.file_clipboard->Publish(offer, files);
#else
                error = runtime.file_clipboard->Publish(offer, files);
                published = error.empty();
#endif
                if (published) {
                    runtime.file_clipboard_owner = &self;
                    runtime.clipboard->CancelWrite();
                }
            }
        } catch (...) {}
        lua_pushboolean(state, published);
#if !defined(_WIN32)
        if (!error.empty()) {
            PushString(state, error);
            return 2;
        }
#endif
        return 1;
    }

    static int ClearPublishedFiles(lua_State* state) {
        auto& self = Self(state);
        const auto offer = ClipboardId(state);
        if (self.runtime_.file_clipboard_owner == &self) self.runtime_.file_clipboard->Clear(offer);
        return 0;
    }

    static int ReplyToClipboardRead(lua_State* state) {
        auto& self = Self(state);
        const auto id = ClipboardId(state);
        std::optional<std::string_view> data;
        if (!lua_isnoneornil(state, 2)) data = CheckString(state, 2);
        const bool accepted = self.runtime_.file_clipboard_owner == &self &&
            self.runtime_.file_clipboard->Reply(id, data);
        lua_pushboolean(state, accepted);
        return 1;
    }

    static int AddMenu(lua_State* state) {
        auto& self = Self(state);
        luaL_checktype(state, 1, LUA_TTABLE);
        luaL_argcheck(state, lua_gettop(state) == 1, 1, "expected one menu declaration");
        MenuField(state, "on_click");
        luaL_checktype(state, -1, LUA_TFUNCTION);
        const auto slot = std::ranges::find(self.menu_, 0u, &MenuCallback::id);
        unsigned id{};
        if (!self.runtime_.host && self.runtime_.Connected() && slot != self.menu_.end()) {
            try {
                if (auto item = ReadMenu(state)) {
                    item->plugin_id = self.manifest_.id;
                    Subscribe(state, 2, slot->ref);
                    id = self.runtime_.workspace.AddMenu(std::move(*item));
                    slot->id = id;
                }
            } catch (...) {}
            if (!id && slot->ref != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX, slot->ref);
                *slot = {};
            }
        }
        lua_settop(state, 1);
        if (id) lua_pushinteger(state, id);
        else lua_pushnil(state);
        return 1;
    }

    static void MenuField(lua_State* state, const char* name) {
        lua_pushstring(state, name);
        lua_rawget(state, 1);
    }

    static std::optional<PluginMenuItem> ReadMenu(lua_State* state) {
        const auto string_field = [state](const char* key) -> std::optional<std::string_view> {
            MenuField(state, key);
            std::optional<std::string_view> result;
            if (lua_type(state, -1) == LUA_TSTRING) {
                size_t size{};
                const auto* text = lua_tolstring(state, -1, &size);
                result = std::string_view(text, size);
            } else if (lua_isnil(state, -1)) {
                result = std::string_view{};
            }
            lua_pop(state, 1);
            return result;
        };
        const auto order_field = [state](const char* key) -> std::optional<int> {
            MenuField(state, key);
            std::optional<int> result;
            if (lua_isnil(state, -1)) result = 50;
            if (lua_isinteger(state, -1)) {
                const auto value = lua_tointeger(state, -1);
                if (value >= -10000 && value <= 10000) result = static_cast<int>(value);
            }
            lua_pop(state, 1);
            return result;
        };
        const auto display_text = [](std::string_view text) -> std::optional<std::wstring> {
            auto value = UiText(text, 64);
            if (!value || std::ranges::any_of(*value, [](wchar_t ch) { return ch < 32 || ch == 127; })) {
                return std::nullopt;
            }
            return value;
        };
        const auto label = string_field("label");
        const auto menu = string_field("menu");
        const auto title = string_field("menu_title");
        const auto section = string_field("section");
        const auto presentation = string_field("presentation");
        const auto menu_order = order_field("menu_order");
        const auto order = order_field("order");
        if (!label || !menu || !title || !section || !presentation || !menu_order || !order ||
            !IsPluginIdentifier(*menu) || (!section->empty() && !IsPluginIdentifier(*section)) ||
            *menu == "plugins" || *menu == "more") return std::nullopt;

        auto label_text = display_text(*label);
        const bool builtin = *menu == "display" || *menu == "audio" || *menu == "input" ||
            *menu == "tools" || *menu == "actions";
        auto title_text = builtin ? std::optional<std::wstring>(std::in_place) : display_text(*title);
        if (!label_text || !title_text) return std::nullopt;
        const bool button = *presentation == "button";
        if (!button && !presentation->empty() && *presentation != "item") return std::nullopt;

        PluginMenuItem result;
        result.label = std::move(*label_text);
        result.menu.assign(*menu);
        result.menu_title = std::move(*title_text);
        result.section.assign(*section);
        result.menu_order = *menu_order;
        result.order = *order;
        result.toolbar_button = button;

        MenuField(state, "checked");
        const bool checked_valid = lua_isnil(state, -1) || lua_isboolean(state, -1);
        if (lua_isboolean(state, -1)) result.checked = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        if (!checked_valid) return std::nullopt;

        MenuField(state, "path");
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            return result;
        }
        if (!lua_istable(state, -1) || button || lua_rawlen(state, -1) > 3) return std::nullopt;
        const auto count = lua_rawlen(state, -1);
        size_t fields{};
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
                static_cast<size_t>(lua_tointeger(state, -2)) > count) return std::nullopt;
            ++fields;
            lua_pop(state, 1);
        }
        if (fields != count) return std::nullopt;
        for (size_t i = 1; i <= count; ++i) {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(i));
            if (lua_type(state, -1) != LUA_TSTRING) return std::nullopt;
            size_t size{};
            const auto* text = lua_tolstring(state, -1, &size);
            auto segment = display_text(std::string_view(text, size));
            lua_pop(state, 1);
            if (!segment) return std::nullopt;
            if (i > 1) result.path += L'\x1f';
            result.path += *segment;
        }
        lua_pop(state, 1);
        return result;
    }

    static int CheckMenu(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        luaL_checktype(state, 2, LUA_TBOOLEAN);
        bool changed{};
        if (id > 0 && id <= UINT_MAX && std::ranges::find(self.menu_, id, &MenuCallback::id) != self.menu_.end()) {
            changed = self.runtime_.workspace.CheckMenu(static_cast<unsigned>(id), lua_toboolean(state, 2) != 0);
        }
        lua_pushboolean(state, changed);
        return 1;
    }

    static int Notice(lua_State* state) {
        auto& runtime = Self(state).runtime_;
        const auto text = CheckString(state, 1);
        bool queued{};
        try {
            if (runtime.Connected() && !runtime.host) {
                if (auto message = UiText(text, 96)) {
                    std::ranges::replace(*message, L'\n', L' ');
                    std::ranges::replace(*message, L'\r', L' ');
                    queued = runtime.workspace.Request({PluginAction::Kind::notice, std::move(*message), {}});
                }
            }
        } catch (...) {}
        lua_pushboolean(state, queued);
        return 1;
    }

    static int OpenDialog(lua_State* state) {
        auto& self = Self(state);
        luaL_checktype(state, 1, LUA_TTABLE);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        const auto slot = std::ranges::find(self.dialogs_, 0u, &DialogCallback::id);
        std::uint32_t id{};
        if (!self.runtime_.host && self.runtime_.Connected() && slot != self.dialogs_.end()) {
            std::string json;
            json.reserve(1024);
            if (LuaValueToJson(state, 1, json)) {
                Subscribe(state, 2, slot->ref);
                try {
                    const auto candidate = self.runtime_.workspace.NextDialogId();
                    if (self.runtime_.workspace.PushUiCommand({candidate, "open", std::move(json)})) {
                        id = candidate;
                        slot->id = id;
                    }
                } catch (...) {}
                if (!id) {
                    luaL_unref(state, LUA_REGISTRYINDEX, slot->ref);
                    *slot = {};
                }
            }
        }
        if (id) lua_pushinteger(state, static_cast<lua_Integer>(id));
        else lua_pushnil(state);
        return 1;
    }

    static int UpdateDialog(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        luaL_checktype(state, 2, LUA_TTABLE);
        bool queued{};
        if (!self.runtime_.host && self.runtime_.Connected() && id > 0 && id <= UINT32_MAX) {
            const auto target = static_cast<std::uint32_t>(id);
            if (std::ranges::find(self.dialogs_, target, &DialogCallback::id) != self.dialogs_.end()) {
                std::string json;
                json.reserve(1024);
                if (LuaValueToJson(state, 2, json)) {
                    try {
                        queued = self.runtime_.workspace.PushUiCommand({target, "update", std::move(json)});
                    } catch (...) {}
                }
            }
        }
        lua_pushboolean(state, queued);
        return 1;
    }

    static int CloseDialog(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        bool closed{};
        if (id > 0 && id <= UINT32_MAX) {
            const auto target = static_cast<std::uint32_t>(id);
            const auto slot = std::ranges::find(self.dialogs_, target, &DialogCallback::id);
            if (slot != self.dialogs_.end()) {
                if (slot->ref != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, slot->ref);
                *slot = {};
                try {
                    closed = self.runtime_.workspace.PushUiCommand({target, "close", "{}"});
                } catch (...) {}
            }
        }
        lua_pushboolean(state, closed);
        return 1;
    }

    static int JsonEncode(lua_State* state) {
        if (lua_gettop(state) < 1) {
            lua_pushnil(state);
            return 1;
        }
        std::string out;
        out.reserve(256);
        if (!LuaValueToJson(state, 1, out)) {
            lua_pushnil(state);
            return 1;
        }
        PushString(state, out);
        return 1;
    }

    static int JsonDecode(lua_State* state) {
        const auto text = CheckString(state, 1);
        if (text.empty() || text.size() > max_json_bytes) {
            lua_pushnil(state);
            return 1;
        }
        JsonParser parser(state, text);
        if (!parser.Parse()) {
            lua_pushnil(state);
            return 1;
        }
        return 1;
    }

    static int FsError(lua_State* state, std::string_view message) {
        lua_pushnil(state);
        PushString(state, message);
        return 2;
    }

    static int FsBoolError(lua_State* state, std::string_view message) {
        lua_pushboolean(state, 0);
        PushString(state, message);
        return 2;
    }

#if defined(_WIN32)
    static int FsRoots(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        wchar_t buffer[512]{};
        const DWORD len = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buffer) - 1), buffer);
        lua_newtable(state);
        lua_Integer index = 1;
        for (const wchar_t* p = buffer; len && p < buffer + len && *p; p += std::wcslen(p) + 1) {
            const UINT kind = GetDriveTypeW(p);
            if (kind == DRIVE_UNKNOWN || kind == DRIVE_NO_ROOT_DIR) continue;
            const auto utf8 = WideToUtf8(p);
            lua_createtable(state, 0, 3);
            PushString(state, utf8);
            lua_setfield(state, -2, "name");
            PushString(state, utf8);
            lua_setfield(state, -2, "path");
            lua_pushstring(state, "drive");
            lua_setfield(state, -2, "kind");
            lua_rawseti(state, -2, index++);
        }
        return 1;
    }

    static int FsHome(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        auto known = [](REFKNOWNFOLDERID id) -> std::string {
            PWSTR raw{};
            if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return {};
            std::wstring wide(raw);
            CoTaskMemFree(raw);
            return WideToUtf8(wide);
        };
        lua_createtable(state, 0, 3);
        if (const auto home = known(FOLDERID_Profile); !home.empty()) {
            PushString(state, home);
            lua_setfield(state, -2, "home");
        }
        if (const auto desktop = known(FOLDERID_Desktop); !desktop.empty()) {
            PushString(state, desktop);
            lua_setfield(state, -2, "desktop");
        }
        if (const auto downloads = known(FOLDERID_Downloads); !downloads.empty()) {
            PushString(state, downloads);
            lua_setfield(state, -2, "downloads");
        }
        return 1;
    }

    static int FsJoin(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        const auto dir = SafeFsPath(CheckString(state, 1));
        const auto name = CheckString(state, 2);
        if (!dir || !ValidLeafName(name)) return FsError(state, "路径或文件名无效");
        const auto wide_name = Utf8ToWide(name);
        if (!wide_name) return FsError(state, "文件名编码无效");
        const auto joined = (*dir / *wide_name).lexically_normal();
        PushString(state, WideToUtf8(joined.native()));
        return 1;
    }

    static int FsStat(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsError(state, "路径无效");
        if (lua_toboolean(state, 2) && !PlainAncestors(*path)) return FsError(state, "路径包含重解析点或无法访问");
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path->c_str(), GetFileExInfoStandard, &data)) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                lua_createtable(state, 0, 4);
                lua_pushboolean(state, 0);
                lua_setfield(state, -2, "exists");
                lua_pushboolean(state, 0);
                lua_setfield(state, -2, "is_dir");
                lua_pushinteger(state, 0);
                lua_setfield(state, -2, "size");
                lua_pushinteger(state, 0);
                lua_setfield(state, -2, "mtime");
                return 1;
            }
            return FsError(state, Win32ErrorUtf8(err));
        }
        ULARGE_INTEGER sz{};
        sz.LowPart = data.nFileSizeLow;
        sz.HighPart = data.nFileSizeHigh;
        lua_createtable(state, 0, 6);
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, "exists");
        lua_pushboolean(state, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        lua_setfield(state, -2, "is_dir");
        lua_pushinteger(state, static_cast<lua_Integer>(sz.QuadPart));
        lua_setfield(state, -2, "size");
        lua_pushinteger(state, static_cast<lua_Integer>(FileTimeToUnixSeconds(data.ftLastWriteTime)));
        lua_setfield(state, -2, "mtime");
        lua_pushinteger(state, FileTimeStamp(data.ftLastWriteTime));
        lua_setfield(state, -2, "stamp");
        lua_pushboolean(state, (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
        lua_setfield(state, -2, "reparse");
        return 1;
    }

    static int FsMkdir(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsBoolError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsBoolError(state, "路径无效");
        if (!CreateDirectoryW(path->c_str(), nullptr)) {
            return FsBoolError(state, Win32ErrorUtf8(GetLastError()));
        }
        lua_pushboolean(state, 1);
        return 1;
    }

    static int FsList(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsError(state, "路径无效");
        const bool strict = lua_toboolean(state, 2) != 0;
        const auto entry_limit = strict ? static_cast<std::size_t>(std::clamp<lua_Integer>(
            luaL_optinteger(state, 3, max_fs_list_entries), 0, max_fs_list_entries)) : max_fs_list_entries;
        if (strict && !PlainAncestors(*path)) return FsError(state, "路径包含重解析点或无法访问");
        const auto attrs = GetFileAttributesW(path->c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return FsError(state, Win32ErrorUtf8(GetLastError()));
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return FsError(state, "目标不是目录");
        if (strict && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return FsError(state, "目录是重解析点");

        const auto pattern = (*path / L"*").native();
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND) return FsError(state, Win32ErrorUtf8(err));
        }

        struct DirEntry {
            std::string name;
            std::wstring lower;
            bool is_dir{};
            std::uint64_t size{};
            std::int64_t mtime{};
            std::string mtime_text;
        };
        std::vector<DirEntry> entries;
        bool truncated = false;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring_view wname(data.cFileName);
                if (wname == L"." || wname == L"..") continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (!strict && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ==
                    (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                if (entries.size() >= entry_limit) {
                    truncated = true;
                    break;
                }
                std::wstring lower(wname);
                for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
                ULARGE_INTEGER sz{};
                sz.LowPart = data.nFileSizeLow;
                sz.HighPart = data.nFileSizeHigh;
                entries.push_back({WideToUtf8(wname), std::move(lower),
                    (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    sz.QuadPart, FileTimeToUnixSeconds(data.ftLastWriteTime),
                    FileTimeToLocalText(data.ftLastWriteTime)});
            } while (FindNextFileW(find, &data));
            const auto error = GetLastError();
            FindClose(find);
            if (strict && !truncated && error != ERROR_NO_MORE_FILES) return FsError(state, Win32ErrorUtf8(error));
        }

        std::ranges::sort(entries, [](const DirEntry& a, const DirEntry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.lower < b.lower;
        });

        lua_createtable(state, 0, 4);
        PushString(state, WideToUtf8(path->native()));
        lua_setfield(state, -2, "path");
        PushString(state, ParentPathUtf8(*path));
        lua_setfield(state, -2, "parent");
        lua_pushboolean(state, truncated);
        lua_setfield(state, -2, "truncated");
        lua_createtable(state, static_cast<int>(entries.size()), 0);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            lua_createtable(state, 0, 5);
            PushString(state, e.name);
            lua_setfield(state, -2, "name");
            lua_pushboolean(state, e.is_dir);
            lua_setfield(state, -2, "is_dir");
            lua_pushinteger(state, static_cast<lua_Integer>(e.size));
            lua_setfield(state, -2, "size");
            lua_pushinteger(state, static_cast<lua_Integer>(e.mtime));
            lua_setfield(state, -2, "mtime");
            PushString(state, e.mtime_text);
            lua_setfield(state, -2, "mtime_text");
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(state, -2, "entries");
        return 1;
    }

    static int FsOpenRead(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsError(state, "路径无效");
        const bool strict = lua_toboolean(state, 2) != 0;
        if (strict && !PlainAncestors(*path)) return FsError(state, "路径包含重解析点或无法访问");
        const auto slot = std::ranges::find(self.files_, 0, &FileSlot::id);
        if (slot == self.files_.end()) return FsError(state, "同时打开的文件过多");
        const auto attrs = GetFileAttributesW(path->c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return FsError(state, Win32ErrorUtf8(GetLastError()));
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return FsError(state, "只能读取普通文件");
        }
        HANDLE file = CreateFileW(path->c_str(), GENERIC_READ, strict ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                (strict ? FILE_FLAG_OPEN_REPARSE_POINT : 0), nullptr);
        if (file == INVALID_HANDLE_VALUE) return FsError(state, Win32ErrorUtf8(GetLastError()));
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) {
            const DWORD err = GetLastError();
            CloseHandle(file);
            return FsError(state, Win32ErrorUtf8(err));
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (strict && (!GetFileInformationByHandle(file, &info) || GetFileType(file) != FILE_TYPE_DISK ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))) {
            CloseHandle(file);
            return FsError(state, "只能读取普通文件");
        }
        slot->id = ++self.next_file_;
        slot->handle = file;
        slot->writable = false;
        slot->target_path = *path;
        slot->temp_path.clear();
        slot->hash_hex.clear();
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &slot->hash, nullptr, 0, nullptr, 0, 0) < 0) {
            slot->hash = nullptr;
        }
        lua_pushinteger(state, slot->id);
        lua_pushinteger(state, static_cast<lua_Integer>(size.QuadPart));
        if (strict) {
            lua_pushinteger(state, FileTimeStamp(info.ftLastWriteTime));
            return 3;
        }
        return 2;
    }

    static int FsSeek(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsBoolError(state, "未连接");
        const auto id = luaL_checkinteger(state, 1);
        LARGE_INTEGER offset{};
        offset.QuadPart = luaL_checkinteger(state, 2);
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        if (offset.QuadPart < 0 || slot == self.files_.end() || !slot->id || slot->writable) {
            return FsBoolError(state, "无效的读取句柄或偏移量");
        }
        if (!SetFilePointerEx(slot->handle, offset, nullptr, FILE_BEGIN)) return FsBoolError(state, Win32ErrorUtf8(GetLastError()));
        if (slot->hash) {
            BCryptDestroyHash(slot->hash);
            slot->hash = nullptr;
        }
        slot->hash_hex.clear();
        lua_pushboolean(state, true);
        return 1;
    }

    static int FsRead(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsError(state, "未连接");
        const auto id = luaL_checkinteger(state, 1);
        const auto requested = luaL_optinteger(state, 2, static_cast<lua_Integer>(32 * 1024));
        if (requested <= 0) return FsError(state, "读取大小无效");
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        if (slot == self.files_.end() || slot->id == 0 || slot->writable || slot->handle == INVALID_HANDLE_VALUE) {
            return FsError(state, "无效的读取句柄");
        }
        const auto chunk = static_cast<DWORD>(std::min<lua_Integer>(requested, static_cast<lua_Integer>(max_fs_read_chunk)));
        std::string buffer(chunk, '\0');
        DWORD read{};
        if (!::ReadFile(slot->handle, buffer.data(), chunk, &read, nullptr)) {
            return FsError(state, Win32ErrorUtf8(GetLastError()));
        }
        if (read > 0 && slot->hash) {
            if (BCryptHashData(slot->hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) < 0) {
                BCryptDestroyHash(slot->hash);
                slot->hash = nullptr;
            }
        }
        lua_pushlstring(state, buffer.data(), read);
        return 1;
    }

    static int FsOpenWrite(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path || path->filename().empty()) return FsError(state, "目标路径无效");
        const auto slot = std::ranges::find(self.files_, 0, &FileSlot::id);
        if (slot == self.files_.end()) return FsError(state, "同时打开的文件过多");
        const auto attrs = GetFileAttributesW(path->c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            ((attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))) {
            return FsError(state, "目标路径不可写入");
        }
        const auto next_id = ++self.next_file_;
        const auto temp_name = L".p2ptmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(next_id);
        const auto temp_path = path->parent_path() / temp_name;
        HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return FsError(state, Win32ErrorUtf8(GetLastError()));
        slot->id = next_id;
        slot->handle = file;
        slot->writable = true;
        slot->target_path = *path;
        slot->temp_path = temp_path;
        slot->hash_hex.clear();
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &slot->hash, nullptr, 0, nullptr, 0, 0) < 0) {
            slot->hash = nullptr;
        }
        lua_pushinteger(state, slot->id);
        return 1;
    }

    static int FsWrite(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsBoolError(state, "未连接");
        const auto id = luaL_checkinteger(state, 1);
        const auto data = CheckString(state, 2);
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        if (slot == self.files_.end() || slot->id == 0 || !slot->writable || slot->handle == INVALID_HANDLE_VALUE) {
            return FsBoolError(state, "无效的写入句柄");
        }
        std::size_t written_total = 0;
        while (written_total < data.size()) {
            DWORD written{};
            const auto to_write = static_cast<DWORD>(std::min<std::size_t>(data.size() - written_total, 64 * 1024));
            if (!WriteFile(slot->handle, data.data() + written_total, to_write, &written, nullptr) || written == 0) {
                return FsBoolError(state, Win32ErrorUtf8(GetLastError()));
            }
            if (slot->hash) {
                if (BCryptHashData(slot->hash,
                        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data() + written_total)), written, 0) < 0) {
                    BCryptDestroyHash(slot->hash);
                    slot->hash = nullptr;
                }
            }
            written_total += written;
        }
        lua_pushboolean(state, 1);
        return 1;
    }

    static int FsCommit(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        if (slot == self.files_.end() || slot->id == 0 || !slot->writable || slot->handle == INVALID_HANDLE_VALUE) {
            return FsBoolError(state, "无效的写入句柄");
        }
        if (slot->hash) {
            BCryptDestroyHash(slot->hash);
            slot->hash = nullptr;
        }
        CloseHandle(slot->handle);
        slot->handle = INVALID_HANDLE_VALUE;
        const auto temp = std::move(slot->temp_path);
        const auto target = std::move(slot->target_path);
        *slot = {};
        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD err = GetLastError();
            DeleteFileW(temp.c_str());
            return FsBoolError(state, Win32ErrorUtf8(err));
        }
        lua_pushboolean(state, 1);
        return 1;
    }

    static int FsHash(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        if (slot == self.files_.end() || slot->id == 0) {
            lua_pushnil(state);
            return 1;
        }
        if (slot->hash_hex.empty() && slot->hash) {
            std::array<UCHAR, 32> digest{};
            const auto status = BCryptFinishHash(slot->hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            BCryptDestroyHash(slot->hash);
            slot->hash = nullptr;
            if (status >= 0) {
                constexpr char hex[] = "0123456789abcdef";
                slot->hash_hex.resize(64);
                for (std::size_t i = 0; i < digest.size(); ++i) {
                    slot->hash_hex[i * 2] = hex[digest[i] >> 4];
                    slot->hash_hex[i * 2 + 1] = hex[digest[i] & 0xf];
                }
            }
        }
        if (slot->hash_hex.empty()) lua_pushnil(state);
        else PushString(state, slot->hash_hex);
        return 1;
    }

    static int FsClose(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        if (slot != self.files_.end() && slot->id != 0) {
            self.CloseFileSlot(*slot);
            lua_pushboolean(state, 1);
        } else {
            lua_pushboolean(state, 0);
        }
        return 1;
    }

#else
#include "plugin_file_api.inc"


#endif
    static int SendKeys(lua_State* state) {
        auto& runtime = Self(state).runtime_;
        luaL_checktype(state, 1, LUA_TTABLE);
        const auto size = lua_rawlen(state, 1);
        std::array<std::uint16_t, shortcut_limit> keys{};
        if (!runtime.host && runtime.Connected() && size > 0 && size <= keys.size()) {
            for (std::size_t i = 0; i < size; ++i) {
                lua_rawgeti(state, 1, static_cast<lua_Integer>(i + 1));
                const auto key = NamedVirtualKey(CheckString(state, -1));
                lua_pop(state, 1);
                if (!key || std::find(keys.begin(), keys.begin() + i, key) != keys.begin() + i) {
                    lua_pushboolean(state, false);
                    return 1;
                }
                keys[i] = static_cast<std::uint16_t>(key);
            }
            bool queued{};
            try {
                InputCommand command{InputCommandKind::shortcut, {keys.begin(), keys.begin() + size}};
                queued = runtime.workspace.Request({PluginAction::Kind::input, {}, std::move(command)});
            } catch (...) {}
            lua_pushboolean(state, queued);
        } else lua_pushboolean(state, false);
        return 1;
    }

    static int SendText(lua_State* state) {
        auto& runtime = Self(state).runtime_;
        const auto value = CheckString(state, 1);
        bool queued{};
        try {
            if (!runtime.host && runtime.Connected()) {
                if (const auto text = UiText(value, input_text_limit)) {
                    InputCommand command{InputCommandKind::text, {text->begin(), text->end()}};
                    queued = runtime.workspace.Request({PluginAction::Kind::input, {}, std::move(command)});
                }
            }
        } catch (...) {}
        lua_pushboolean(state, queued);
        return 1;
    }

    static int InputState(lua_State* state) {
        const auto locks = Self(state).runtime_.workspace.ReadLockState();
#if defined(_WIN32)
        const auto state_value = locks.value_or(std::pair{
            (GetKeyState(VK_CAPITAL) & 1) != 0, (GetKeyState(VK_NUMLOCK) & 1) != 0});
#else
        if (!locks) {
            lua_createtable(state, 0, 0);
            return 1;
        }
        const auto state_value = *locks;
#endif
        lua_createtable(state, 0, 3);
        lua_pushboolean(state, state_value.first);
        lua_setfield(state, -2, "caps_lock");
        lua_pushboolean(state, state_value.second);
        lua_setfield(state, -2, "num_lock");
        if (const auto ime = ForegroundIme()) {
            lua_pushboolean(state, *ime);
            lua_setfield(state, -2, "ime_open");
        }
        return 1;
    }

#if defined(_WIN32)
    static int SetLocalLock(lua_State* state) {
        const auto key = CheckString(state, 1);
        luaL_checktype(state, 2, LUA_TBOOLEAN);
        const auto& runtime = Self(state).runtime_;
        const auto vk = key == "caps_lock" ? VK_CAPITAL : key == "num_lock" ? VK_NUMLOCK : 0;
        lua_pushboolean(state, vk && runtime.host && runtime.Connected() && SetLockKey(static_cast<WORD>(vk), lua_toboolean(state, 2) != 0));
        return 1;
    }

#else
    static int SetLocalLock(lua_State* state) {
        const auto key = CheckString(state, 1);
        luaL_checktype(state, 2, LUA_TBOOLEAN);
        const auto& runtime = Self(state).runtime_;
        lua_pushboolean(state, runtime.host && runtime.Connected() && setHostLockKey(key, lua_toboolean(state, 2) != 0));
        return 1;
    }

#endif
    static int SetIme(lua_State* state) {
        luaL_checktype(state, 1, LUA_TBOOLEAN);
        const auto& runtime = Self(state).runtime_;
        lua_pushboolean(state, runtime.host && runtime.Connected() && ForegroundIme(lua_toboolean(state, 1) != 0).has_value());
        return 1;
    }

    static int UseLockSync(lua_State* state) {
        luaL_checktype(state, 1, LUA_TBOOLEAN);
        auto& self = Self(state);
        const bool allowed = !self.runtime_.host && self.runtime_.Connected();
        self.CaptureLockKeys(allowed && lua_toboolean(state, 1) != 0);
        lua_pushboolean(state, allowed);
        return 1;
    }

    static int Stats(lua_State* state) {
        const auto sample = Self(state).runtime_.workspace.ReadMetrics();
        const char* route = "unknown";
        switch (sample.route) {
        case IcePath::host: route = "lan"; break;
        case IcePath::nat: route = "direct"; break;
        case IcePath::relay: route = "relay"; break;
        case IcePath::unknown: break;
        }
        lua_createtable(state, 0, sample.width ? 11 : 4);
        lua_pushstring(state, route);
        lua_setfield(state, -2, "route");
        if (sample.rtt_ms) { lua_pushnumber(state, *sample.rtt_ms); lua_setfield(state, -2, "rtt_ms"); }
        if (sample.loss) { lua_pushnumber(state, *sample.loss); lua_setfield(state, -2, "loss"); }
        if (sample.download_bps) { lua_pushinteger(state, *sample.download_bps); lua_setfield(state, -2, "download_bps"); }
        if (sample.upload_bps) { lua_pushinteger(state, *sample.upload_bps); lua_setfield(state, -2, "upload_bps"); }
        if (sample.frame_wait_ms) { lua_pushnumber(state, *sample.frame_wait_ms); lua_setfield(state, -2, "frame_wait_ms"); }
        if (sample.width && sample.height) {
            PushString(state, sample.protocol); lua_setfield(state, -2, "protocol");
            lua_pushinteger(state, sample.width); lua_setfield(state, -2, "width");
            lua_pushinteger(state, sample.height); lua_setfield(state, -2, "height");
            lua_pushnumber(state, sample.seconds); lua_setfield(state, -2, "seconds");
            lua_pushinteger(state, static_cast<lua_Integer>(sample.presented)); lua_setfield(state, -2, "presented");
        }
        return 1;
    }

    struct OverlayOptions {
        std::optional<OverlayAnchor> anchor;
        std::optional<int> priority, font_size, opacity;
        std::uint32_t background{overlay_default_color}, foreground{overlay_default_color};
        bool monospace{}, visible{true};
    };

    static std::uint32_t OverlayColor(std::string_view value) {
        if (value.size() != 7 || value.front() != '#') return overlay_default_color;
        std::uint32_t rgb{};
        const auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
        return error == std::errc{} && end == value.data() + value.size() ? rgb : overlay_default_color;
    }

    bool PublishOverlay(std::string_view slot, std::string_view text, const OverlayOptions& options) noexcept {
        if (!IsPluginIdentifier(slot) || text.size() > 1024 || !runtime_.Connected()) return false;
        try {
            auto wide = Utf8ToWide(text);
            if (!wide) return false;
            auto item = manifest_.overlay_defaults;
            item.id = item.plugin_id + L":" + std::wstring(slot.begin(), slot.end());
            item.text = std::move(*wide);
            item.anchor = options.anchor.value_or(item.anchor);
            item.priority = options.priority.value_or(item.priority);
            item.font_size = options.font_size.value_or(item.font_size);
            item.opacity = static_cast<std::uint8_t>(options.opacity.value_or(item.opacity));
            item.text_rgb = options.foreground;
            item.bg_rgb = options.background;
            item.visible = options.visible;
            item.monospace = options.monospace;
            return runtime_.overlays.Set(std::move(item));
        } catch (...) {
            return false;
        }
    }

    static int SetOverlayText(lua_State* state) {
        const auto text = CheckString(state, 1);
        lua_pushboolean(state, Self(state).PublishOverlay("default", text, {}));
        return 1;
    }

    static int SetOverlay(lua_State* state) {
        const auto slot = CheckString(state, 1);
        luaL_checktype(state, 2, LUA_TTABLE);
        const auto field = [state](const char* name) {
            lua_pushstring(state, name);
            lua_rawget(state, 2);
        };
        const auto string = [state, &field](const char* name) {
            field(name);
            std::size_t size{};
            const auto* data = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &size) : nullptr;
            lua_pop(state, 1);
            return data ? std::string_view(data, size) : std::string_view{};
        };
        const auto integer = [state, &field](const char* name, int low, int high) -> std::optional<int> {
            field(name);
            int valid{};
            const auto value = lua_tointegerx(state, -1, &valid);
            lua_pop(state, 1);
            if (!valid) return std::nullopt;
            return static_cast<int>(std::clamp<lua_Integer>(value, low, high));
        };
        const auto boolean = [state, &field](const char* name, bool fallback) {
            field(name);
            const bool value = lua_type(state, -1) == LUA_TBOOLEAN ? lua_toboolean(state, -1) != 0 : fallback;
            lua_pop(state, 1);
            return value;
        };
        const auto text = string("text");
        const OverlayOptions options{ParseOverlayAnchor(string("anchor")), integer("priority", -1000, 1000),
            integer("font_size", 9, 20), integer("opacity", 0, 255), OverlayColor(string("bg_color")),
            OverlayColor(string("text_color")), boolean("monospace", false), boolean("visible", true)};
        lua_pushboolean(state, Self(state).PublishOverlay(slot, text, options));
        return 1;
    }

    static int ClearOverlay(lua_State* state) {
        auto& self = Self(state);
        const auto& plugin_id = self.manifest_.overlay_defaults.plugin_id;
        if (lua_isnoneornil(state, 1)) {
            self.runtime_.overlays.Clear(plugin_id);
        } else {
            const auto slot = CheckString(state, 1);
            if (IsPluginIdentifier(slot)) {
                try {
                    self.runtime_.overlays.Clear(plugin_id, plugin_id + L":" + std::wstring(slot.begin(), slot.end()));
                } catch (...) {}
            }
        }
        return 0;
    }
    static int SetBitrate(lua_State* state) {
        auto& runtime = Self(state).runtime_;
        const auto name = CheckString(state, 1);
        const auto bitrate = name == "automatic" ? VideoBitratePreset::automatic : name == "economy" ? VideoBitratePreset::economy :
            name == "detail" ? VideoBitratePreset::detail : VideoBitratePreset{};
        bool queued{};
        try {
            queued = !runtime.host && runtime.Connected() && bitrate != VideoBitratePreset{} &&
                runtime.workspace.Request({PluginAction::Kind::bitrate, {}, {}, bitrate});
        } catch (...) {}
        lua_pushboolean(state, queued);
        return 1;
    }

    PluginSettings& Settings() {
        if (!settings_) {
            auto path = runtime_.config_root;
            if (!path.empty()) {
                const auto* extension = runtime_.host ? ".host.json" : ".viewer.json";
                path /= manifest_.id + extension;
            }
            settings_.emplace(std::move(path));
        }
        return *settings_;
    }

#if defined(_WIN32)
    bool AllowsAgentDiagnostics(AgentTool tool) {
        if (tool == AgentTool::screen_snapshot && !runtime_.diagnostics_only) return false;
        try {
            const auto access = Settings().Get(tool == AgentTool::screen_snapshot ? L"allow_screen_snapshot" : L"allow_host_access");
            return access && access.ValueType() == JsonValueType::Boolean && access.GetBoolean();
        } catch (...) { return false; }
    }

    static int AgentStatus(lua_State* state, const char* status) {
        lua_pushnil(state);
        lua_pushstring(state, status);
        return 2;
    }

    static int BeginSystemInfo(lua_State* state) {
        auto& self = Self(state);
        auto& runtime = self.runtime_;
        if (!runtime.host || !runtime.Connected()) return AgentStatus(state, "unavailable");
        static std::atomic_uint32_t next_id{1};
        const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
        if (!id) return AgentStatus(state, "unavailable");
        const auto tool = lua_toboolean(state, 1) ? AgentTool::screen_snapshot : AgentTool::system_info;
        const auto allowed = self.AllowsAgentDiagnostics(tool);
        const auto now = TickMilliseconds();
        auto& workspace = runtime.workspace;
        bool paused{};
        bool busy{};
        {
            const std::lock_guard lock(workspace.mutex);
            paused = workspace.agent_paused;
            busy = workspace.agent_request.has_value();
            if (allowed && !paused && !busy) {
                workspace.agent_request.emplace();
                workspace.agent_request->id = id;
                workspace.agent_request->started = now;
                workspace.agent_request->tool = tool;
            }
        }
        if (!allowed || paused) {
            RecordAgentAudit(runtime.config_root, id, paused ? AgentAuditResult::revoked : AgentAuditResult::denied, 0, tool);
            return AgentStatus(state, "denied");
        }
        if (busy) return AgentStatus(state, "busy");
        workspace.Signal();
        lua_pushinteger(state, id);
        return 1;
    }

    static int PollSystemInfo(lua_State* state) {
        auto& self = Self(state);
        auto& runtime = self.runtime_;
        auto& workspace = runtime.workspace;
        const auto id = luaL_checkinteger(state, 1);
        const auto tool = lua_toboolean(state, 2) ? AgentTool::screen_snapshot : AgentTool::system_info;
        const auto allowed = self.AllowsAgentDiagnostics(tool);
        const auto now = TickMilliseconds();
        std::optional<AgentRequest> finished;
        {
            const std::lock_guard lock(workspace.mutex);
            auto& request = workspace.agent_request;
            if (request && request->id == id && request->tool == tool) {
                const bool expired = now - request->started >= agent_approval_timeout_ms;
                if (!allowed || workspace.agent_paused) request->decision = AgentRequest::Decision::revoked;
                if (expired || request->decision != AgentRequest::Decision::waiting) {
                    finished = std::exchange(request, std::nullopt);
                }
            }
        }
        if (!finished) return AgentStatus(state, "pending");
        workspace.Signal();
        const auto elapsed = now - finished->started;
        const auto record = [&](AgentAuditResult result) {
            return RecordAgentAudit(runtime.config_root, finished->id, result, elapsed, tool);
        };
        if (finished->decision == AgentRequest::Decision::cancelled) {
            record(AgentAuditResult::cancelled);
            return AgentStatus(state, "cancelled");
        }
        if (finished->decision == AgentRequest::Decision::revoked) {
            record(AgentAuditResult::revoked);
            return AgentStatus(state, "revoked");
        }
        if (finished->decision == AgentRequest::Decision::denied) {
            record(AgentAuditResult::denied);
            return AgentStatus(state, "denied");
        }
        if (finished->decision != AgentRequest::Decision::approved || elapsed >= agent_approval_timeout_ms) {
            record(AgentAuditResult::expired);
            return AgentStatus(state, "expired");
        }
        if (!runtime.Connected()) {
            record(AgentAuditResult::disconnected);
            return AgentStatus(state, "unavailable");
        }
        if (tool == AgentTool::screen_snapshot ? finished->image.empty() : !PushSystemInfo(state)) {
            record(AgentAuditResult::unavailable);
            return AgentStatus(state, "unavailable");
        }
        if (tool == AgentTool::screen_snapshot) PushString(state, finished->image);
        if (!record(AgentAuditResult::approved)) {
            lua_pop(state, 1);
            return AgentStatus(state, "unavailable");
        }
        return 1;
    }

    static int CancelSystemInfo(lua_State* state) {
        const auto id = luaL_checkinteger(state, 1);
        auto& workspace = Self(state).runtime_.workspace;
        const std::lock_guard lock(workspace.mutex);
        if (auto& request = workspace.agent_request; request && request->id == id) {
            if (request->decision == AgentRequest::Decision::waiting ||
                request->decision == AgentRequest::Decision::approved) {
                request->decision = AgentRequest::Decision::cancelled;
            }
        }
        return 0;
    }

    static bool PushSystemInfo(lua_State* state) {
        using GetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto get_version = std::bit_cast<GetVersion>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (!get_version || get_version(&version) != 0 || !GlobalMemoryStatusEx(&memory)) return false;
        SYSTEM_INFO system{};
        GetNativeSystemInfo(&system);
        const char* architecture = "unknown";
        switch (system.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: architecture = "x64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: architecture = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: architecture = "x86"; break;
        }
        const auto integer = [state](const char* key, lua_Integer value) {
            lua_pushinteger(state, value);
            lua_setfield(state, -2, key);
        };
        const auto text = [state](const char* key, std::wstring_view value) {
            const auto utf8 = winrt::to_string(value);
            lua_pushlstring(state, utf8.data(), utf8.size());
            lua_setfield(state, -2, key);
        };
        constexpr auto mib = 1024ULL * 1024;
        lua_createtable(state, 0, 14);
        integer("schema", 1);
        lua_pushstring(state, architecture);
        lua_setfield(state, -2, "architecture");
        integer("windows_major", version.dwMajorVersion);
        integer("windows_minor", version.dwMinorVersion);
        integer("windows_build", version.dwBuildNumber);
        integer("logical_processors", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        integer("memory_total_mib", static_cast<lua_Integer>(memory.ullTotalPhys / mib));
        integer("memory_available_mib", static_cast<lua_Integer>(memory.ullAvailPhys / mib));
        integer("uptime_seconds", static_cast<lua_Integer>(TickMilliseconds() / 1000));
        wchar_t windows_directory[MAX_PATH]{};
        const auto windows_length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
        wchar_t volume[MAX_PATH]{};
        ULARGE_INTEGER total{}, free{};
        if (windows_length && windows_length < MAX_PATH &&
            GetVolumePathNameW(windows_directory, volume, MAX_PATH) && GetDriveTypeW(volume) == DRIVE_FIXED &&
            GetDiskFreeSpaceExW(volume, nullptr, &total, &free)) {
            integer("system_disk_total_mib", static_cast<lua_Integer>(total.QuadPart / mib));
            integer("system_disk_free_mib", static_cast<lua_Integer>(free.QuadPart / mib));
        }
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD computer_length = static_cast<DWORD>(std::size(computer));
        if (GetComputerNameW(computer, &computer_length)) text("host_name", {computer, computer_length});
        wchar_t user[257]{};
        DWORD user_length = static_cast<DWORD>(std::size(user));
        if (GetUserNameW(user, &user_length) && user_length > 1) text("host_user", {user, user_length - 1});
        winrt::handle token;
        TOKEN_ELEVATION elevation{};
        DWORD returned{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) &&
            GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned)) {
            lua_pushboolean(state, elevation.TokenIsElevated != 0);
            lua_setfield(state, -2, "host_elevated");
        }
        return true;
    }

#else
    static int BeginSystemInfo(lua_State* state) { return FsError(state, "unavailable"); }
    static int PollSystemInfo(lua_State* state) { return FsError(state, "unavailable"); }
    static int CancelSystemInfo(lua_State*) { return 0; }

#endif
#if defined(_WIN32)
    static int ConfigGet(lua_State* state) {
        auto& self = Self(state);
        const auto key = CheckString(state, 1);
        JsonValueType type{JsonValueType::Null};
        double number{};
        bool boolean{};
        try {
            if (IsPluginIdentifier(key)) {
                if (const auto value = self.Settings().Get(winrt::to_hstring(key))) {
                    type = value.ValueType();
                    if (type == JsonValueType::String) self.config_result_ = winrt::to_string(value.GetString());
                    else if (type == JsonValueType::Number) number = value.GetNumber();
                    else if (type == JsonValueType::Boolean) boolean = value.GetBoolean();
                }
            }
        } catch (...) { type = JsonValueType::Null; }
        if (type == JsonValueType::String) PushString(state, self.config_result_);
        else if (type == JsonValueType::Number) lua_pushnumber(state, number);
        else if (type == JsonValueType::Boolean) lua_pushboolean(state, boolean);
        else if (lua_gettop(state) >= 2) lua_pushvalue(state, 2);
        else lua_pushnil(state);
        return 1;
    }

    static int ConfigSet(lua_State* state) {
        auto& self = Self(state);
        const auto key = CheckString(state, 1);
        const auto type = lua_type(state, 2);
        bool saved{};
        try {
            if (IsPluginIdentifier(key) && (type == LUA_TNIL || type == LUA_TBOOLEAN || type == LUA_TNUMBER || type == LUA_TSTRING)) {
                const auto json = winrt::try_get_activation_factory<JsonValue, IJsonValueStatics>();
                JsonValue value{nullptr};
                bool valid = static_cast<bool>(json);
                if (valid && type == LUA_TBOOLEAN) value = json.CreateBooleanValue(lua_toboolean(state, 2) != 0);
                else if (valid && type == LUA_TNUMBER) {
                    const auto number = lua_tonumber(state, 2);
                    valid = std::isfinite(number);
                    if (valid) value = json.CreateNumberValue(number);
                } else if (valid && type == LUA_TSTRING) {
                    const auto text = CheckString(state, 2);
                    valid = text.size() <= 2048;
                    if (valid) value = json.CreateStringValue(winrt::to_hstring(text));
                }
                saved = valid && self.Settings().Set(winrt::to_hstring(key), value);
            }
        } catch (...) {}
        lua_pushboolean(state, saved);
        return 1;
    }

#else
    static int ConfigGet(lua_State* state) {
        auto& self = Self(state);
        const auto key = CheckString(state, 1);
        QJsonValue value;
        if (IsPluginIdentifier(key)) value = self.Settings().value(key);
        if (value.isString()) {
            self.config_result_ = value.toString().toStdString();
            PushString(state, self.config_result_);
        } else if (value.isBool()) lua_pushboolean(state, value.toBool());
        else if (value.isDouble()) lua_pushnumber(state, value.toDouble());
        else if (lua_gettop(state) >= 2) lua_pushvalue(state, 2);
        else lua_pushnil(state);
        return 1;
    }

    static int ConfigSet(lua_State* state) {
        auto& self = Self(state);
        const auto key = CheckString(state, 1);
        const auto type = lua_type(state, 2);
        QJsonValue value = QJsonValue::Undefined;
        if (type == LUA_TNIL) value = QJsonValue::Null;
        else if (type == LUA_TBOOLEAN) value = lua_toboolean(state, 2) != 0;
        else if (type == LUA_TNUMBER && std::isfinite(lua_tonumber(state, 2))) value = lua_tonumber(state, 2);
        else if (type == LUA_TSTRING) {
            const auto text = CheckString(state, 2);
            if (text.size() <= 2048) value = QString::fromUtf8(text.data(), text.size());
        }
        lua_pushboolean(state, IsPluginIdentifier(key) && !value.isUndefined() && self.Settings().set(key, value));
        return 1;
    }

#endif
    static int Sha256(lua_State* state) {
        const auto input = CheckString(state, 1);
        if (input.size() > plugin_frame_limit) return luaL_argerror(state, 1, "data exceeds 64 KiB");
#if defined(_WIN32)
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
#else
        const auto hash = QCryptographicHash::hash(QByteArrayView(input.data(), input.size()), QCryptographicHash::Sha256).toHex();
        lua_pushlstring(state, hash.constData(), hash.size());
#endif
        return 1;
    }

    static int Base64EncodeLua(lua_State* state) {
        const auto input = CheckString(state, 1);
        if (input.size() > plugin_frame_limit) return luaL_argerror(state, 1, "data exceeds 64 KiB");
        const auto encoded = Base64Encode(input);
        PushString(state, encoded);
        return 1;
    }

    static int Base64DecodeLua(lua_State* state) {
        const auto input = CheckString(state, 1);
        const auto decoded = input.size() <= plugin_frame_limit * 2 ? Base64Decode(input) : std::nullopt;
        if (!decoded) {
            lua_pushnil(state);
            lua_pushstring(state, "invalid base64");
            return 2;
        }
        PushString(state, *decoded);
        return 1;
    }

    static int PtyOpen(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.host || !self.runtime_.Connected()) {
            lua_pushnil(state);
            lua_pushstring(state, "pty is only available on connected host");
            return 2;
        }
        const std::string_view shell = lua_isnoneornil(state, 1) ? std::string_view("powershell") : CheckString(state, 1);
        const auto cols = std::clamp<lua_Integer>(luaL_optinteger(state, 2, 100), 20, 400);
        const auto rows = std::clamp<lua_Integer>(luaL_optinteger(state, 3, 30), 5, 200);
        const auto slot = std::ranges::find_if(self.ptys_, [](const auto& item) { return item.id == 0; });
        if (slot == self.ptys_.end()) {
            lua_pushnil(state);
            lua_pushstring(state, "too many open pty sessions");
            return 2;
        }
        const auto pty_id = ++self.next_pty_;
        try {
            auto session = std::make_unique<PtySession>(shell, static_cast<std::uint16_t>(cols),
                static_cast<std::uint16_t>(rows));
            if (!session->Valid()) {
                lua_pushnil(state);
                PushString(state, session->Error());
                return 2;
            }
            *slot = {pty_id, std::move(session)};
        } catch (...) {
            lua_pushnil(state);
            lua_pushliteral(state, "无法初始化终端输入输出");
            return 2;
        }
        lua_pushinteger(state, static_cast<lua_Integer>(pty_id));
        return 1;
    }

    static int PtyWrite(lua_State* state) {
        auto& self = Self(state);
        const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
        const auto data = CheckString(state, 2);
        const auto slot = std::ranges::find_if(self.ptys_, [id](const auto& item) { return item.id == id && item.session; });
        bool queued{};
        try { queued = slot != self.ptys_.end() && slot->session->Write(data); } catch (...) {}
        lua_pushboolean(state, queued);
        return 1;
    }

    static int PtyRead(lua_State* state) {
        auto& self = Self(state);
        const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
        const auto slot = std::ranges::find_if(self.ptys_, [id](const auto& item) { return item.id == id && item.session; });
        if (slot == self.ptys_.end()) return 0;
        PtySession::Output output;
        try { output = slot->session->Read(); }
        catch (...) {
#if defined(_WIN32)
            output.exit_code = ERROR_NOT_ENOUGH_MEMORY;
#else
            output.exit_code = ENOMEM;
#endif
        }
        if (!output.bytes.empty()) {
            PushString(state, output.bytes);
            return 1;
        }
        if (!output.exit_code) return 0;
        lua_pushnil(state);
        lua_pushinteger(state, *output.exit_code);
        return 2;
    }

    static int PtyResize(lua_State* state) {
        auto& self = Self(state);
        const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
        const auto cols = std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 20, 400);
        const auto rows = std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 5, 200);
        const auto slot = std::ranges::find_if(self.ptys_, [id](const auto& item) { return item.id == id && item.session; });
        lua_pushboolean(state, slot != self.ptys_.end() &&
            slot->session->Resize(static_cast<std::uint16_t>(cols), static_cast<std::uint16_t>(rows)));
        return 1;
    }

    static int PtyClose(lua_State* state) {
        auto& self = Self(state);
        const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
        const auto slot = std::ranges::find_if(self.ptys_, [id](const auto& item) { return item.id == id; });
        if (slot == self.ptys_.end()) {
            lua_pushboolean(state, false);
            return 1;
        }
        if (slot->session) slot->session->Close();
        *slot = {};
        lua_pushboolean(state, true);
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
        const auto delay = static_cast<Tick>(ms);
        *slot = {++self.next_timer_, TickMilliseconds() + delay, repeating ? delay : 0, ref};
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
            {"on", OnConnection}, {"stats", Stats}, {nullptr, nullptr}
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
        static const luaL_Reg file_clipboard[] = {
            {"available", [](lua_State* l) {
                const auto& runtime = Self(l).runtime_;
                lua_pushboolean(l, runtime.clipboard && runtime.clipboard->Available() &&
                    runtime.file_clipboard && runtime.file_clipboard->Available());
                return 1;
            }},
            {"on_change", [](lua_State* l) { Subscribe(l, 1, Self(l).callbacks_.clipboard_files); return 0; }},
            {"on_read", [](lua_State* l) { Subscribe(l, 1, Self(l).callbacks_.clipboard_read); return 0; }},
            {"on_error", [](lua_State* l) { Subscribe(l, 1, Self(l).callbacks_.clipboard_error); return 0; }},
            {"publish", PublishFiles}, {"clear", ClearPublishedFiles}, {"reply", ReplyToClipboardRead},
            {nullptr, nullptr}
        };
        static const luaL_Reg crypto[] = {{"sha256", Sha256}, {nullptr, nullptr}};
        static const luaL_Reg timer[] = {
            {"after", [](lua_State* l) { return AddTimer(l, false); }},
            {"every", [](lua_State* l) { return AddTimer(l, true); }},
            {"cancel", CancelTimer},
            {"now_ms", [](lua_State* l) { lua_pushinteger(l, static_cast<lua_Integer>(TickMilliseconds())); return 1; }},
            {nullptr, nullptr}
        };
        static const luaL_Reg json[] = {
            {"encode", JsonEncode}, {"decode", JsonDecode}, {nullptr, nullptr}
        };
        static const luaL_Reg base64[] = {
            {"encode", Base64EncodeLua}, {"decode", Base64DecodeLua}, {nullptr, nullptr}
        };
        static const luaL_Reg pty[] = {
            {"open", PtyOpen}, {"read", PtyRead}, {"write", PtyWrite}, {"resize", PtyResize}, {"close", PtyClose},
            {nullptr, nullptr}
        };
        static const luaL_Reg log[] = {
            {"info", [](lua_State* l) { Log(Self(l).manifest_.id, "info", CheckString(l, 1)); return 0; }},
            {"warn", [](lua_State* l) { Log(Self(l).manifest_.id, "warn", CheckString(l, 1)); return 0; }},
            {"error", [](lua_State* l) { Log(Self(l).manifest_.id, "error", CheckString(l, 1)); return 0; }},
            {nullptr, nullptr}
        };
        static const luaL_Reg ui[] = {
            {"add_menu", AddMenu}, {"set_checked", CheckMenu}, {"notify", Notice},
            {"open_dialog", OpenDialog}, {"update_dialog", UpdateDialog}, {"close_dialog", CloseDialog},
            {nullptr, nullptr}
        };
        static const luaL_Reg overlay[] = {
            {"set_text", SetOverlayText}, {"set", SetOverlay}, {"clear", ClearOverlay}, {nullptr, nullptr}
        };
        static const luaL_Reg input[] = {{"send_keys", SendKeys}, {"send_text", SendText}, {"get_state", InputState},
            {"on_state_change", [](lua_State* l) { Subscribe(l, 1, Self(l).callbacks_.lock_state); return 0; }},
            {"set_lock", SetLocalLock}, {"set_ime", SetIme}, {"use_lock_sync", UseLockSync}, {nullptr, nullptr}};
        static const luaL_Reg config[] = {{"get", ConfigGet}, {"set", ConfigSet}, {nullptr, nullptr}};
        static const luaL_Reg sys[] = {{"begin_info", BeginSystemInfo}, {"poll_info", PollSystemInfo},
            {"cancel_info", CancelSystemInfo}, {nullptr, nullptr}};
        static const luaL_Reg video[] = {{"set_bitrate", SetBitrate}, {nullptr, nullptr}};
        static const luaL_Reg fs[] = {
            {"roots", FsRoots}, {"home", FsHome}, {"list", FsList}, {"stat", FsStat},
            {"mkdir", FsMkdir}, {"join", FsJoin},
            {"open_read", FsOpenRead}, {"read", FsRead}, {"seek", FsSeek},
            {"open_write", FsOpenWrite}, {"write", FsWrite}, {"commit", FsCommit}, {"hash", FsHash}, {"close", FsClose},
            {nullptr, nullptr}
        };
        lua_newtable(state);
        Namespace(state, "session", session);
        Namespace(state, "crypto", crypto);
        Namespace(state, "timer", timer);
        Namespace(state, "json", json);
        Namespace(state, "base64", base64);
        if (manifest_.channel) Namespace(state, "channel", channel);
        if (manifest_.clipboard) Namespace(state, "clipboard", clipboard);
        if (manifest_.file_clipboard) Namespace(state, "file_clipboard", file_clipboard);
        if (manifest_.log) Namespace(state, "log", log);
        if (manifest_.ui) Namespace(state, "ui", ui);
        if (manifest_.input) Namespace(state, "input", input);
        if (manifest_.config) Namespace(state, "config", config);
        if (manifest_.sys && runtime_.host && manifest_.id == "ai-agent") Namespace(state, "sys", sys);
        if (manifest_.video) Namespace(state, "video", video);
        if (manifest_.fs) Namespace(state, "fs", fs);
        if (manifest_.pty) Namespace(state, "pty", pty);
        if (manifest_.overlay && !runtime_.host) Namespace(state, "overlay", overlay);
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
    bool captures_lock_keys_{};
    struct {
        int connected{LUA_NOREF};
        int disconnected{LUA_NOREF};
        int message{LUA_NOREF};
        int clipboard{LUA_NOREF};
        int clipboard_files{LUA_NOREF};
        int clipboard_read{LUA_NOREF};
        int clipboard_error{LUA_NOREF};
        int lock_state{LUA_NOREF};
    } callbacks_;
    std::array<Timer, 32> timers_{};
    std::array<MenuCallback, 8> menu_{};
    std::array<DialogCallback, 4> dialogs_{};
    std::array<FileSlot, 4> files_{};
    std::array<PtySlot, 2> ptys_{};
    std::optional<PluginSettings> settings_;
    std::string config_result_;
    lua_Integer next_timer_{};
    lua_Integer next_file_{};
    std::uint32_t next_pty_{};
};

void LoadPlugins(const std::filesystem::path& root, PluginRuntime& runtime,
    std::vector<std::unique_ptr<LuaPlugin>>& plugins, bool diagnostics_only, bool clipboard_only) {
    if (!IsPlainPath(root, true)) return;
#if defined(_WIN32)
    const bool restricted_host = runtime.host && [] {
        HANDLE opened{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &opened)) return true;
        const winrt::handle token{opened};
        alignas(TOKEN_USER) BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
        DWORD size{};
        return !GetTokenInformation(token.get(), TokenUser, storage, sizeof(storage), &size)
            || IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(storage)->User.Sid, WinLocalSystemSid);
    }();
    // Keep the activation factory scoped to this worker's COM apartment.
    const auto json_factory = winrt::try_get_activation_factory<JsonObject, IJsonObjectStatics>();
    if (!json_factory) throw std::runtime_error("Could not activate plugin manifest parser");
#endif
    for (const auto& directory : std::filesystem::directory_iterator(root)) {
        if (!runtime.Connected() || plugins.size() == max_plugins) break;
        if (clipboard_only && directory.path().filename() != "clipboard-sync" &&
            directory.path().filename() != "file-clipboard") continue;
        if (!IsPlainPath(directory.path(), true)) continue;
        try {
            std::filesystem::path entry;
            auto manifest = ReadManifest(
#if defined(_WIN32)
                json_factory,
#endif
                directory.path(), runtime.host, entry);
            if (!manifest || (clipboard_only && manifest->id != "clipboard-sync" && manifest->id != "file-clipboard")) continue;
            if (diagnostics_only && (manifest->id != "ai-agent" || manifest->input || manifest->video ||
                manifest->fs || manifest->pty || manifest->clipboard || manifest->file_clipboard)) continue;
            const bool local_overlay = manifest->overlay && !manifest->channel;
            if (local_overlay != runtime.local_overlay) continue;
            if (local_overlay && (runtime.host || manifest->clipboard || manifest->file_clipboard
                    || manifest->ui || manifest->input || manifest->video || manifest->fs || manifest->pty)) continue;
#if defined(_WIN32)
            if (restricted_host && (manifest->pty || manifest->fs || manifest->file_clipboard)) continue;
#endif
            const auto script = ReadFile(entry, 128 * 1024);
            if (manifest->clipboard || manifest->file_clipboard) {
                if (!runtime.clipboard) runtime.clipboard.emplace();
                if (manifest->file_clipboard && !runtime.file_clipboard) {
                    runtime.clipboard->WatchFiles();
                    if (runtime.clipboard->Available()) {
#if defined(_WIN32)
                        runtime.file_clipboard.emplace(runtime.wake.Native());
#else
                        runtime.file_clipboard.emplace([&wake = runtime.wake] { wake.Signal(); });
#endif
                    }
                }
                runtime.clipboard->Poll(TickMilliseconds());
            }
            auto plugin = std::make_unique<LuaPlugin>(std::move(*manifest), runtime);
            if (plugin->Load(script)) plugins.push_back(std::move(plugin));
#if defined(_WIN32)
        } catch (const winrt::hresult_error&) {
            Log("loader", "error", "Invalid plugin manifest");
#endif
        } catch (const std::exception& error) {
            Log("loader", "error", error.what());
        }
    }
}

} // namespace

struct PluginHost::Impl {
    Impl(bool host, SendFrame send, std::filesystem::path root, Notify notify, std::filesystem::path config_root,
        bool local_overlay, std::shared_ptr<PluginOverlays> overlays, bool diagnostics_only, bool clipboard_only)
        : host(host), local_overlay(local_overlay), diagnostics_only(diagnostics_only), clipboard_only(clipboard_only), send(std::move(send)), root(std::move(root)),
          config_root(std::move(config_root)), overlays(overlays ? std::move(overlays) : std::make_shared<PluginOverlays>()),
          workspace(std::move(notify)) {
        worker = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    void Stop() {
        connected = false;
        worker.request_stop();
        wake.Signal();
        if (worker.joinable()) worker.join();
    }

    void SendAgentQueries() {
        std::uint32_t cancel{}, request{};
        AgentTool tool{};
        {
            const std::lock_guard lock(workspace.mutex);
            cancel = std::exchange(workspace.cancel_agent_query, 0);
            if (auto& query = workspace.local_agent_query; query && !query->sent) {
                query->sent = true;
                request = query->id;
                tool = query->tool;
            }
        }
        if (cancel) {
            if (const auto frame = EncodePluginPacket({"ai-agent", "cancel_info", AgentHeader(cancel)})) send(*frame);
        }
        if (request) {
            const auto frame = EncodePluginPacket({"ai-agent",
                tool == AgentTool::screen_snapshot ? "request_screen" : "request_info", AgentHeader(request)});
            if (!frame || !send(*frame)) {
                const std::lock_guard lock(workspace.mutex);
                if (auto& query = workspace.local_agent_query; query && query->id == request) {
                    query->result.status = "unavailable";
                }
            }
        }
    }

    bool OnAgentSummary(const PluginPacket& packet) {
        if (host || packet.id != "ai-agent" || packet.payload.size() < agent_header_bytes) return false;
        const auto id = AgentHeaderId(packet.payload);
        if (!(id & local_agent_id_bit)) return false;
        const std::lock_guard lock(workspace.mutex);
        auto& query = workspace.local_agent_query;
        if (!query || query->id != id || !query->sent || !query->result.status.empty()) return true;
        const auto size = packet.payload.size() - agent_header_bytes;
        if (query->tool == AgentTool::system_info && packet.type == "snapshot" && size && size <= agent_summary_limit) {
            query->result = {"ok", std::string(packet.payload.substr(agent_header_bytes)), {}};
        } else if (query->tool == AgentTool::screen_snapshot &&
                (packet.type == "image_chunk" || packet.type == "image_last")) {
            auto& image = query->result.image;
            if (size <= agent_header_bytes || size > agent_header_bytes + agent_image_chunk ||
                    AgentHeaderId(packet.payload.substr(agent_header_bytes)) != query->chunk ||
                    image.size() + size - agent_header_bytes > agent_image_limit) {
                query->result.status = "invalid_snapshot";
                image.clear();
                return true;
            }
            ++query->chunk;
            image.append(packet.payload.substr(2 * agent_header_bytes));
            if (packet.type == "image_last") {
                if (ValidAgentImage(image)) query->result.status = "ok";
                else {
                    query->result.status = "invalid_snapshot";
                    image.clear();
                }
            }
        } else if (!size && (packet.type == "denied" || packet.type == "unavailable" ||
            packet.type == "revoked" || packet.type == "expired" || packet.type == "busy" || packet.type == "cancelled")) {
            query->result.status = packet.type;
        }
        return true;
    }

    void Run(std::stop_token stop) noexcept {
        try {
            while (!stop.stop_requested() && !connected.load()) wake.WaitUntilConnected();
            if (stop.stop_requested()) return;
#if defined(_WIN32)
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
#endif
            PluginRuntime runtime(host, send, connected, workspace, config_root.empty() ? DefaultPluginSettingsDirectory()
                : config_root, wake, local_overlay, *overlays, diagnostics_only);
            std::vector<std::unique_ptr<LuaPlugin>> plugins;
            LoadPlugins(root.empty() ? PluginDirectory() : root, runtime, plugins, diagnostics_only, clipboard_only);
            if (local_overlay && plugins.empty()) return;
            for (const auto& plugin : plugins) {
                if (connected.load()) plugin->ConnectionChanged(true);
            }
            while (!stop.stop_requested() && connected.load()) {
#if defined(_WIN32)
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
#endif
                auto now = TickMilliseconds();
                if (runtime.clipboard && connected.load() && runtime.clipboard->Poll(now)) {
                    for (const auto& plugin : plugins) plugin->ClipboardChanged(runtime.clipboard->Text());
                    for (const auto& plugin : plugins) {
                        plugin->ClipboardFilesChanged(runtime.clipboard->Files(), runtime.clipboard->FileIssue());
                    }
                }
                if (runtime.file_clipboard && connected.load()) {
                    if (const auto error = runtime.file_clipboard->TakeError(); !error.empty() && runtime.file_clipboard_owner) {
                        runtime.file_clipboard_owner->ClipboardError(error);
                    }
                    if (auto request = runtime.file_clipboard->TakeRequest()) {
                        if (runtime.file_clipboard_owner) runtime.file_clipboard_owner->ClipboardRead(*request);
                        else runtime.file_clipboard->Reply(request->id, std::nullopt);
                    }
                }

                std::string frame;
                {
                    const std::lock_guard lock(mutex);
                    if (!frames.empty()) {
                        frame = std::move(frames.front());
                        frames.pop_front();
                    }
                }
                if (!host && !local_overlay && connected.load()) SendAgentQueries();
                if (const auto packet = DecodePluginPacket(frame); packet && connected.load() && !OnAgentSummary(*packet)) {
                    const auto plugin = std::ranges::find_if(plugins, [&](const auto& entry) { return entry->Info().id == packet->id; });
                    if (plugin != plugins.end()) (*plugin)->Receive(*packet);
                }

                unsigned click{};
                std::optional<PluginUiEvent> ui_event;
                bool lock_changed{};
                {
                    const std::lock_guard lock(workspace.mutex);
                    if (!workspace.clicks.empty()) {
                        click = workspace.clicks.front();
                        workspace.clicks.pop_front();
                    }
                    if (!workspace.ui_events.empty()) {
                        ui_event = std::move(workspace.ui_events.front());
                        workspace.ui_events.pop_front();
                    }
                    lock_changed = std::exchange(workspace.lock_state_dirty, false);
                }
                if (click && connected.load()) {
                    for (const auto& plugin : plugins) plugin->Click(click);
                }
                if (ui_event && connected.load()) {
                    for (const auto& plugin : plugins) {
                        if (plugin->HandleUiEvent(*ui_event)) break;
                    }
                }
                if (lock_changed && connected.load()) {
                    for (const auto& plugin : plugins) plugin->LockStateChanged();
                }

                for (const auto& plugin : plugins) {
                    if (connected.load()) plugin->RunTimer(now);
                }
                if (!frame.empty() || click || ui_event || lock_changed) continue;
                now = TickMilliseconds();
                WaitTime wait = runtime.clipboard ? runtime.clipboard->Wait(now) : no_timeout;
                for (const auto& plugin : plugins) wait = std::min(wait, plugin->Wait(now));
                if (!wake.Wait(wait)) break;
            }
            connected = false;
            std::optional<AgentRequest> interrupted;
            {
                const std::lock_guard lock(workspace.mutex);
                interrupted = std::exchange(workspace.agent_request, std::nullopt);
            }
#if defined(_WIN32)
            if (interrupted) {
                RecordAgentAudit(runtime.config_root, interrupted->id, AgentAuditResult::disconnected,
                    TickMilliseconds() - interrupted->started, interrupted->tool);
            }
#endif
            if (runtime.clipboard) runtime.clipboard->CancelWrite();
            if (runtime.file_clipboard) runtime.file_clipboard->Clear();
            for (const auto& plugin : plugins) plugin->ConnectionChanged(false);
        } catch (...) {
            Log("host", "error", "Plugin worker stopped");
        }
        connected = false;
        const std::lock_guard lock(mutex);
        frames.clear();
    }

    const bool host;
    const bool local_overlay;
    const bool diagnostics_only;
    const bool clipboard_only;
    const SendFrame send;
    const std::filesystem::path root;
    const std::filesystem::path config_root;
    std::shared_ptr<PluginOverlays> overlays;
    Workspace workspace;
    PluginWake wake;
    std::atomic_bool connected{};
    std::mutex mutex;
    std::deque<std::string> frames;
    std::jthread worker;
};

PluginHost::PluginHost(bool host, SendFrame send, std::filesystem::path root, Notify notify,
    std::filesystem::path config_root, bool local_overlay, std::shared_ptr<PluginOverlays> overlays, bool diagnostics_only, bool clipboard_only)
    : impl_(std::make_unique<Impl>(host, std::move(send), std::move(root), std::move(notify),
        std::move(config_root), local_overlay, std::move(overlays), diagnostics_only, clipboard_only)) {}
PluginHost::~PluginHost() { Stop(); }

void PluginHost::SetConnected(bool connected) noexcept {
    impl_->connected = connected;
    impl_->wake.Signal();
}

void PluginHost::Receive(std::string_view frame) noexcept {
    if (!impl_->connected.load() || !DecodePluginPacket(frame)) return;
    try {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->connected.load() || impl_->frames.size() >= max_pending_frames) return;
        impl_->frames.emplace_back(frame);
        impl_->wake.Signal();
    } catch (...) {}
}

void PluginHost::UpdateMetrics(PluginMetrics metrics) {
    const std::lock_guard lock(impl_->workspace.mutex);
    impl_->workspace.metrics = std::move(metrics);
}

std::wstring PluginHost::OverlayText() const {
    PluginOverlaySnapshot snapshot;
    impl_->overlays->Read(snapshot);
    const auto item = std::ranges::find(snapshot.items, L"connection-status:default", &PluginOverlayItem::id);
    return item != snapshot.items.end() && item->visible ? item->text : std::wstring{};
}
void PluginHost::UpdateLockState(bool caps, bool num) {
    bool notify{};
    {
        const std::lock_guard lock(impl_->workspace.mutex);
        const std::pair<bool, bool> next{caps, num};
        if (impl_->workspace.lock_state != next) {
            notify = impl_->workspace.lock_state.has_value();
            impl_->workspace.lock_state = next;
            if (notify) impl_->workspace.lock_state_dirty = true;
        }
    }
    if (notify) impl_->wake.Signal();
}

bool PluginHost::CapturesLockKeys() const noexcept {
    return impl_->connected.load() && impl_->workspace.lock_key_consumers.load() != 0;
}

bool PluginHost::HasMenu() const {
    const std::lock_guard lock(impl_->workspace.mutex);
    return impl_->connected.load() && !impl_->workspace.menu.empty();
}

std::vector<PluginMenuItem> PluginHost::Menu() const {
    const std::lock_guard lock(impl_->workspace.mutex);
    if (!impl_->connected.load()) return {};
    return impl_->workspace.menu;
}

void PluginHost::Invoke(unsigned id) {
    {
        auto& workspace = impl_->workspace;
        const std::lock_guard lock(workspace.mutex);
        if (!impl_->connected.load() || workspace.clicks.size() >= 16 ||
            std::ranges::find(workspace.menu, id, &PluginMenuItem::id) == workspace.menu.end()) return;
        workspace.clicks.push_back(id);
    }
    impl_->wake.Signal();
}

std::uint32_t PluginHost::PendingAgentApproval(AgentTool* tool) const {
    const auto& workspace = impl_->workspace;
    const std::lock_guard lock(workspace.mutex);
    const auto& request = workspace.agent_request;
    if (!impl_->host || !impl_->connected.load() || !request || request->decision != AgentRequest::Decision::waiting ||
        TickMilliseconds() - request->started >= agent_approval_timeout_ms) return 0;
    if (tool) *tool = request->tool;
    return request->id;
}

void PluginHost::DecideAgentApproval(std::uint32_t id, bool approved, std::string_view image) {
    auto& workspace = impl_->workspace;
    const std::lock_guard lock(workspace.mutex);
    auto& request = workspace.agent_request;
    if (impl_->host && impl_->connected.load() && request && request->id == id &&
        request->decision == AgentRequest::Decision::waiting &&
        TickMilliseconds() - request->started < agent_approval_timeout_ms) {
        if (approved && request->tool == AgentTool::screen_snapshot && ValidAgentImage(image)) {
            request->image = image;
        }
        request->decision = approved ? AgentRequest::Decision::approved : AgentRequest::Decision::denied;
    }
}

void PluginHost::RevokeAgentApproval(bool pause) {
    auto& workspace = impl_->workspace;
    const std::lock_guard lock(workspace.mutex);
    workspace.agent_paused = pause;
    if (workspace.agent_request) workspace.agent_request->decision = AgentRequest::Decision::revoked;
}

std::uint32_t PluginHost::RequestAgentSummary(AgentTool tool) {
    if (impl_->host || impl_->local_overlay || !impl_->send ||
        (tool == AgentTool::screen_snapshot && !impl_->diagnostics_only)) return 0;
    auto& workspace = impl_->workspace;
    std::uint32_t id{};
    {
        const std::lock_guard lock(workspace.mutex);
        if (!impl_->connected.load() || workspace.local_agent_query) return 0;
        static std::atomic_uint32_t next_id{1};
        id = next_id.fetch_add(1, std::memory_order_relaxed) | local_agent_id_bit;
        workspace.local_agent_query = LocalAgentQuery{id, false, tool, 0, {}};
    }
    impl_->wake.Signal();
    return id;
}

std::optional<AgentSummary> PluginHost::TakeAgentSummary(std::uint32_t id) {
    auto& workspace = impl_->workspace;
    const std::lock_guard lock(workspace.mutex);
    auto& query = workspace.local_agent_query;
    if (!impl_->connected.load() || !query || query->id != id) return AgentSummary{"disconnected", {}, {}};
    if (query->result.status.empty()) return std::nullopt;
    auto result = std::move(query->result);
    query.reset();
    return result;
}

void PluginHost::CancelAgentSummary(std::uint32_t id) {
    auto& workspace = impl_->workspace;
    {
        const std::lock_guard lock(workspace.mutex);
        auto& query = workspace.local_agent_query;
        if (!query || query->id != id) return;
        if (query->sent && query->result.status.empty()) workspace.cancel_agent_query = id;
        query.reset();
    }
    impl_->wake.Signal();
}

std::vector<PluginAction> PluginHost::TakeActions() {
    const std::lock_guard lock(impl_->workspace.mutex);
    auto actions = std::exchange(impl_->workspace.actions, {});
    if (!impl_->connected.load()) actions.clear();
    return actions;
}

std::vector<PluginUiCommand> PluginHost::TakeUiCommands() {
    const std::lock_guard lock(impl_->workspace.mutex);
    std::vector<PluginUiCommand> result;
    result.reserve(impl_->workspace.ui_commands.size());
    while (!impl_->workspace.ui_commands.empty()) {
        result.push_back(std::move(impl_->workspace.ui_commands.front()));
        impl_->workspace.ui_commands.pop_front();
    }
    return result;
}

void PluginHost::SendUiEvent(PluginUiEvent event) {
    if (!event.dialog_id || event.event.empty()) return;
    {
        auto& workspace = impl_->workspace;
        const std::lock_guard lock(workspace.mutex);
        if (!impl_->connected.load()) return;
        if (workspace.ui_events.size() >= 64) {
            if (event.event != "close" || !event.control.empty()) return;
            const auto replaceable = std::ranges::find_if(workspace.ui_events, [](const auto& queued) {
                return queued.event != "close" || !queued.control.empty();
            });
            if (replaceable == workspace.ui_events.end()) return;
            workspace.ui_events.erase(replaceable);
        }
        workspace.ui_events.push_back(std::move(event));
    }
    impl_->wake.Signal();
}

void PluginHost::Stop() { impl_->Stop(); }

} // namespace p2premote
