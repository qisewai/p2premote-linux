#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "live_client.h"
#include "snapshot.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace p2premote::agent {
namespace {

constexpr DWORD deadline_ms = 25'000;
constexpr std::size_t response_limit = 768 * 1024;
using namespace winrt::Windows::Data::Json;

DWORD Remaining(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    return deadline <= now ? 0 : static_cast<DWORD>(deadline - now);
}

DWORD Transfer(HANDLE pipe, bool write, void* buffer, DWORD size, ULONGLONG deadline, HANDLE cancel) {
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
        throw std::runtime_error("The local diagnostic request was cancelled.");
    }
    winrt::handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Unable to create a diagnostic I/O event.");
    OVERLAPPED pending{};
    pending.hEvent = event.get();
    const BOOL started = write ? WriteFile(pipe, buffer, size, nullptr, &pending)
                               : ReadFile(pipe, buffer, size, nullptr, &pending);
    if (!started && GetLastError() != ERROR_IO_PENDING) {
        throw std::runtime_error("The local diagnostic connection closed.");
    }
    const std::array handles{event.get(), cancel};
    const auto signal = started ? WAIT_OBJECT_0 : WaitForMultipleObjects(
        cancel ? 2 : 1, handles.data(), FALSE, Remaining(deadline));
    if (signal != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &pending);
        DWORD ignored{};
        GetOverlappedResult(pipe, &pending, &ignored, TRUE);
        throw std::runtime_error(signal == WAIT_OBJECT_0 + 1 ? "The local diagnostic request was cancelled."
                                                            : "The local diagnostic request timed out.");
    }
    DWORD done{};
    if (!GetOverlappedResult(pipe, &pending, &done, FALSE)) {
        throw std::runtime_error("The local diagnostic connection closed.");
    }
    return done;
}

} // namespace

bool IsLiveToken(std::wstring_view token) noexcept {
    return token.size() == 32 && std::ranges::all_of(token, [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
    });
}

bool IsReportTool(std::wstring_view name) noexcept {
    return name.starts_with(L"plugin_") && name.size() <= 64 && std::ranges::all_of(name, [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
    });
}

JsonObject QueryLocalAgent(std::wstring_view token, LiveOperation op, std::wstring_view device, std::stop_token stop,
    std::wstring_view report) {
    if (!IsLiveToken(token) || (op != LiveOperation::devices && (device.empty() || device.size() > 128)) ||
        (op == LiveOperation::report && !IsReportTool(report))) {
        throw std::invalid_argument("Invalid local sharing code or device ID.");
    }
    const auto location = L"\\\\.\\pipe\\p2premote-diagnostics-" + std::wstring(token);
    const auto deadline = GetTickCount64() + (op == LiveOperation::devices ? 5'000 : deadline_ms);
    const winrt::handle cancel(stop.stop_possible() ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr);
    if (stop.stop_possible() && !cancel) throw std::runtime_error("Unable to create a diagnostic cancellation event.");
    const std::stop_callback on_stop(stop, [&] { SetEvent(cancel.get()); });
    if (stop.stop_requested()) throw std::runtime_error("The local diagnostic request was cancelled.");
    const auto open = [&] {
        return CreateFileW(location.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    };
    auto handle = open();
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(location.c_str(), 1500)) handle = open();
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Local diagnostics sharing is unavailable or has ended.");
    }
    const winrt::handle pipe(handle);

    JsonObject request;
    const auto name = op == LiveOperation::devices ? L"list_devices"
        : op == LiveOperation::screen_snapshot ? L"screen_snapshot"
        : op == LiveOperation::report ? L"system_report" : L"system_info";
    request.SetNamedValue(L"op", JsonValue::CreateStringValue(name));
    if (op != LiveOperation::devices) request.SetNamedValue(L"device_id", JsonValue::CreateStringValue(device));
    if (op == LiveOperation::report) request.SetNamedValue(L"report", JsonValue::CreateStringValue(report));
    auto wire = winrt::to_string(request.Stringify()) + '\n';
    if (Transfer(pipe.get(), true, wire.data(), static_cast<DWORD>(wire.size()), deadline, cancel.get()) != wire.size()) {
        throw std::runtime_error("Could not send the local diagnostic request.");
    }

    std::array<char, 4096> chunk{};
    std::string response;
    for (;;) {
        const auto accessed = Transfer(pipe.get(), false, chunk.data(), static_cast<DWORD>(chunk.size()), deadline, cancel.get());
        if (!accessed || response.size() + accessed > response_limit) {
            throw std::runtime_error("Invalid local diagnostic response.");
        }
        const auto received = response.size();
        response.append(chunk.data(), accessed);
        if (const auto end = response.find('\n', received); end != std::string::npos) {
            response.resize(end);
            break;
        }
    }
    JsonObject result{nullptr};
    if (!JsonObject::TryParse(winrt::to_hstring(response), result)) {
        throw std::runtime_error("Invalid local diagnostic response.");
    }
    const auto status = result.GetNamedString(L"status", L"");
    if (status == L"denied" || status == L"expired" || status == L"revoked" || status == L"cancelled") {
        throw std::runtime_error("The remote diagnostic approval was declined, expired, or cancelled.");
    }
    if (status == L"not_shared") throw std::runtime_error("Only the session selected in Qt can be queried.");
    if (status == L"requires_diagnostics") throw std::runtime_error("Screen snapshots require a diagnostics-only session.");
    if (status == L"busy") throw std::runtime_error("A diagnostic request is already pending.");
    if (status != L"ok") throw std::runtime_error("The live diagnostics result is unavailable.");
    if (op == LiveOperation::devices) return result;
    if (op == LiveOperation::report) {
        const auto fields = result.GetNamedObject(L"snapshot");
        if (fields.Size() > 8) throw std::runtime_error("Invalid diagnostic report.");
        return fields;
    }
    if (op == LiveOperation::screen_snapshot) {
        const auto png = result.GetNamedString(L"image_png", L"");
        constexpr std::size_t image_limit_base64 = ((512 * 1024 + 2) / 3) * 4;
        if (png.empty() || png.size() > image_limit_base64 || png.size() % 4 || !png.starts_with(L"iVBORw0KGgo")) {
            throw std::runtime_error("Invalid PNG in diagnostic response.");
        }
        JsonObject image;
        image.SetNamedValue(L"image_png", JsonValue::CreateStringValue(png));
        return image;
    }
    return ParseDiagnosticSnapshot(winrt::to_string(result.GetNamedObject(L"snapshot").Stringify()));
}

} // namespace p2premote::agent
