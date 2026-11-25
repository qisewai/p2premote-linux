#include "live_client.h"
#include "snapshot.h"

#include <fcntl.h>
#include <io.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace winrt::Windows::Data::Json;

constexpr std::size_t message_limit = 64 * 1024;
constexpr double max_integer = 9007199254740991.0;
constexpr wchar_t protocol_version[] = L"2025-11-25";

JsonObject Object(std::initializer_list<std::pair<std::wstring_view, IJsonValue>> fields = {}) {
    JsonObject result;
    for (const auto& [key, value] : fields) result.SetNamedValue(key, value);
    return result;
}

JsonArray Array(const IJsonValue& value) {
    JsonArray result;
    result.Append(value);
    return result;
}

IJsonValue Text(std::wstring_view value) { return JsonValue::CreateStringValue(value); }
IJsonValue Number(double value) { return JsonValue::CreateNumberValue(value); }
IJsonValue Flag(bool value) { return JsonValue::CreateBooleanValue(value); }

bool IsInteger(const IJsonValue& value) {
    if (!value || value.ValueType() != JsonValueType::Number) return false;
    const auto number = value.GetNumber();
    return std::abs(number) <= max_integer && std::trunc(number) == number;
}

JsonObject Result(const IJsonValue& id, const JsonObject& result) {
    return Object({{L"jsonrpc", Text(L"2.0")}, {L"id", id}, {L"result", result}});
}

JsonObject Error(const IJsonValue& id, int code, std::wstring_view message) {
    return Object({{L"jsonrpc", Text(L"2.0")}, {L"id", id ? id : JsonValue::CreateNullValue()},
        {L"error", Object({{L"code", Number(code)}, {L"message", Text(message)}})}});
}

JsonObject Tool(std::wstring_view name, std::wstring_view description, bool needs_device) {
    const auto properties = needs_device
        ? Object({{L"device_id", Object({{L"type", Text(L"string")}, {L"minLength", Number(1)},
            {L"maxLength", Number(128)}})}}) : Object();
    const auto schema = Object({{L"type", Text(L"object")}, {L"properties", properties},
        {L"additionalProperties", Flag(false)}});
    if (needs_device) schema.SetNamedValue(L"required", Array(Text(L"device_id")));
    return Object({{L"name", Text(name)}, {L"description", Text(description)}, {L"inputSchema", schema},
        {L"annotations", Object({{L"readOnlyHint", Flag(true)}, {L"openWorldHint", Flag(false)}})}});
}

JsonObject ToolResult(const JsonObject& data, std::wstring_view error = {}) {
    auto result = Object({{L"content", Array(Object({{L"type", Text(L"text")},
        {L"text", data ? Text(data.Stringify()) : Text(error)}}))}, {L"isError", Flag(!data)}});
    if (data) result.SetNamedValue(L"structuredContent", data);
    return result;
}

void Send(const JsonObject& response) {
    if (!response) return;
    const auto line = winrt::to_string(response.Stringify());
    static std::mutex output;
    const std::lock_guard lock(output);
    std::cout << line << '\n' << std::flush;
}

class Server {
public:
    Server(JsonObject snapshot, std::wstring live_token)
        : snapshot_(std::move(snapshot)), live_token_(std::move(live_token)) {}

    void Finish() {
        if (job_.joinable()) job_.join();
    }

    JsonObject Dispatch(std::string_view line) {
        JsonObject request{nullptr};
        try {
            if (!JsonObject::TryParse(winrt::to_hstring(line), request)) {
                return Error(nullptr, -32700, L"Parse error");
            }
        } catch (...) {
            return Error(nullptr, -32700, L"Parse error");
        }
        IJsonValue id{nullptr};
        try {
            const auto value = request.GetNamedValue(L"id", nullptr);
            if (value && (value.ValueType() == JsonValueType::String || IsInteger(value))) id = value;
            if (request.GetNamedString(L"jsonrpc", L"") != L"2.0" ||
                !request.GetNamedValue(L"method", nullptr) ||
                request.GetNamedValue(L"method").ValueType() != JsonValueType::String || (value && !id)) {
                return Error(id, -32600, L"Invalid Request");
            }
            const auto method = request.GetNamedString(L"method");
            if (!id) {
                if (initializing_ && method == L"notifications/initialized") initialized_ = true;
                if (method == L"notifications/cancelled" && job_.joinable()) {
                    const auto params = request.GetNamedObject(L"params", nullptr);
                    const auto target = params ? params.GetNamedValue(L"requestId", nullptr) : nullptr;
                    if (target && target.Stringify() == job_id_) job_.request_stop();
                }
                return nullptr;
            }
            if (method == L"ping") return Result(id, Object());
            const auto params = request.GetNamedObject(L"params", nullptr);
            if (method == L"initialize") {
                if (initializing_ || !params || params.GetNamedString(L"protocolVersion", L"").empty()) {
                    return Error(id, -32602, L"Invalid initialize request");
                }
                initializing_ = true;
                return Result(id, Object({{L"protocolVersion", Text(protocol_version)},
                    {L"capabilities", Object({{L"tools", Object()}})},
                    {L"serverInfo", Object({{L"name", Text(L"p2premote-diagnostics")},
                        {L"version", Text(L"1.0.0")}})}}));
            }
            if (!initialized_) return Error(id, -32002, L"Server not initialized");
            if (method == L"tools/list") {
                const bool live = !live_token_.empty();
                auto tools = Array(Tool(L"system_info", live
                    ? L"Request the Windows system summary for the device shared in Qt. Requires approval on the remote PC."
                    : L"Read the Windows system summary from a user-exported snapshot.", live));
                if (live) tools.Append(Tool(L"list_devices",
                    L"List account devices and identify the session shared in Qt.", false));
                if (live) AppendLiveTools(tools);
                return Result(id, Object({{L"tools", tools}}));
            }
            if (method == L"tools/call") return CallTool(id, params);
            return Error(id, -32601, L"Method not found");
        } catch (...) {
            return Error(id, -32602, L"Invalid parameters");
        }
    }

private:
    void AppendLiveTools(JsonArray& tools) {
        using namespace p2premote::agent;
        try {
            const auto sharing = QueryLocalAgent(live_token_, LiveOperation::devices);
            if (sharing.GetNamedBoolean(L"screen_snapshot", false)) {
                tools.Append(Tool(L"screen_snapshot",
                    L"Request one PNG of the selected remote monitor. Requires separate approval on the remote PC.", true));
            }
            const auto listed = sharing.GetNamedArray(L"reports", nullptr);
            if (!listed || listed.Size() > 32) return;
            for (const auto& item : listed) {
                const auto name = item.GetObject().GetNamedString(L"name");
                if (IsReportTool(name)) {
                    tools.Append(Tool(name,
                        L"Read the selected fields from a Windows system summary. Requires approval on the remote PC.", true));
                }
            }
        } catch (...) {}
    }

    JsonObject CallTool(const IJsonValue& id, const JsonObject& params) {
        using namespace p2premote::agent;
        const auto name = params ? params.GetNamedString(L"name", L"") : winrt::hstring{};
        const bool live = !live_token_.empty();
        const bool devices = live && name == L"list_devices";
        const bool screen = live && name == L"screen_snapshot";
        const bool report = live && IsReportTool(name);
        if (name != L"system_info" && !devices && !screen && !report) return Error(id, -32602, L"Unknown tool");
        const auto arguments = params.GetNamedObject(L"arguments", nullptr);
        winrt::hstring device;
        if (live && !devices) {
            if (arguments && arguments.Size() == 1) device = arguments.GetNamedString(L"device_id", L"");
            if (device.empty() || device.size() > 128) return Error(id, -32602, L"Expected a device_id");
        } else if (arguments && arguments.Size()) {
            return Error(id, -32602, L"This tool takes no arguments");
        }
        if (!live) return Result(id, ToolResult(snapshot_));
        if (devices) return LiveResult(id, LiveOperation::devices, L"", {});
        if (running_.load()) return Result(id, ToolResult(nullptr, L"A diagnostic request is already pending."));
        Finish();
        job_id_ = id.Stringify();
        running_ = true;
        const auto op = report ? LiveOperation::report : screen ? LiveOperation::screen_snapshot : LiveOperation::system_info;
        try {
            job_ = std::jthread([this, id, device, op, name](std::stop_token stop) {
                try {
                    winrt::init_apartment();
                    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
                    const auto response = LiveResult(id, op, device, stop, name);
                    if (!stop.stop_requested()) Send(response);
                } catch (...) {}
                running_ = false;
            });
        } catch (...) {
            running_ = false;
            return Result(id, ToolResult(nullptr, L"Could not start the live diagnostic request."));
        }
        return nullptr;
    }

    JsonObject LiveResult(const IJsonValue& id, p2premote::agent::LiveOperation op, std::wstring_view device,
        std::stop_token stop, std::wstring_view report = {}) {
        try {
            const auto data = p2premote::agent::QueryLocalAgent(live_token_, op, device, stop, report);
            if (op == p2premote::agent::LiveOperation::screen_snapshot) {
                return Result(id, Object({{L"content", Array(Object({{L"type", Text(L"image")},
                    {L"mimeType", Text(L"image/png")}, {L"data", data.GetNamedValue(L"image_png")}}))},
                    {L"isError", Flag(false)}}));
            }
            return Result(id, ToolResult(data));
        } catch (const std::exception& error) {
            return Result(id, ToolResult(nullptr, winrt::to_hstring(error.what())));
        } catch (...) {
            return Result(id, ToolResult(nullptr, L"The live diagnostic request failed."));
        }
    }

    JsonObject snapshot_;
    std::wstring live_token_;
    bool initializing_{};
    bool initialized_{};
    winrt::hstring job_id_;
    std::atomic_bool running_{};
    std::jthread job_;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace p2premote::agent;
    const std::wstring_view mode = argc == 3 ? argv[1] : L"";
    const bool live = mode == L"--live" && IsLiveToken(argv[2]);
    if (mode != L"--snapshot" && !live) {
        std::cerr << "Usage: p2premote_mcp.exe --live <sharing-code>\n"
                     "       p2premote_mcp.exe --snapshot <absolute-path-to-exported-json>\n";
        return 2;
    }
    try {
        winrt::init_apartment();
        Server server(live ? JsonObject{nullptr} : LoadDiagnosticSnapshot(argv[2]), live ? argv[2] : L"");
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        std::array<char, message_limit + 1> line{};
        for (;;) {
            std::cin.getline(line.data(), static_cast<std::streamsize>(line.size()));
            if (std::cin.bad()) return 1;
            if (std::cin.fail() && !std::cin.eof()) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                Send(Error(nullptr, -32600, L"Request too large"));
                continue;
            }
            if (std::cin.eof()) break;
            const auto size = static_cast<std::size_t>(std::cin.gcount() - 1);
            Send(server.Dispatch(std::string_view(line.data(), size)));
        }
        server.Finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    } catch (...) {
        std::cerr << "Unable to start the diagnostics server.\n";
    }
    return 1;
}
