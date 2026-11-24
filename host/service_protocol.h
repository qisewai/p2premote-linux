#pragma once

#include <windows.h>
#include <winrt/base.h>

#include <optional>
#include <string>

namespace p2premote::service {

inline constexpr wchar_t kServiceName[] = L"P2PRemoteService";
inline constexpr wchar_t kServiceDisplayName[] = L"P2P Remote Host Service";
inline constexpr wchar_t kServiceDescription[] = L"P2P Remote 无人值守后台服务";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\P2PRemoteServiceStatus";
inline constexpr DWORD kPipeClientAccess = FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE;

inline DWORD RemainingPipeTime(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}

inline bool CompletePipeIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout, DWORD& size) {
    HANDLE handles[]{operation.hEvent, stop};
    if (WaitForMultipleObjects(stop ? 2 : 1, handles, FALSE, timeout) == WAIT_OBJECT_0) {
        return GetOverlappedResult(pipe, &operation, &size, FALSE);
    }
    CancelIoEx(pipe, &operation);
    // Keep the OVERLAPPED and buffer alive until cancellation completes.
    GetOverlappedResult(pipe, &operation, &size, TRUE);
    return false;
}

inline bool TransferPipeBytes(HANDLE pipe, HANDLE event, bool writing, void* buffer, DWORD length,
    DWORD& size, ULONGLONG deadline, HANDLE stop = nullptr) {
    if (!RemainingPipeTime(deadline)) return false;
    ResetEvent(event);
    OVERLAPPED operation{};
    operation.hEvent = event;
    const BOOL immediate = writing ? WriteFile(pipe, buffer, length, &size, &operation)
                                   : ReadFile(pipe, buffer, length, &size, &operation);
    return immediate || (GetLastError() == ERROR_IO_PENDING
        && CompletePipeIo(pipe, operation, stop, RemainingPipeTime(deadline), size));
}

inline std::optional<std::string> ReadServiceStatus(DWORD timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    if (!WaitNamedPipeW(kPipeName, timeout_ms)) return std::nullopt;
    HANDLE opened = CreateFileW(kPipeName, kPipeClientAccess, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return std::nullopt;
    const winrt::handle pipe{opened};
    const winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return std::nullopt;

    std::string response;
    char buffer[1024];
    DWORD size{};
    while (response.size() < 16 * 1024
            && TransferPipeBytes(pipe.get(), event.get(), false, buffer, sizeof(buffer), size, deadline) && size) {
        response.append(buffer, size);
        if (const auto end = response.find('\n'); end != std::string::npos) {
            char received = 1;
            TransferPipeBytes(pipe.get(), event.get(), true, &received, sizeof(received), size, deadline);
            response.resize(end);
            return response;
        }
    }
    return std::nullopt;
}

} // namespace p2premote::service
