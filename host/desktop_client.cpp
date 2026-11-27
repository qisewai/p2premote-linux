#include "desktop_client.h"
#include "service_protocol.h"

#include <wincrypt.h>
#include <array>
#include <utility>

namespace p2premote::service {

std::wstring ImageHash(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const winrt::handle source{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 256 * 1024 * 1024) return {};
    const winrt::handle map{CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!map) return {};
    auto* data = static_cast<const BYTE*>(MapViewOfFile(map.get(), FILE_MAP_READ, 0, 0, 0));
    if (!data) return {};
    std::array<BYTE, 32> digest{};
    DWORD count = static_cast<DWORD>(digest.size());
    const bool ok = CryptHashCertificate2(L"SHA256", 0, nullptr, data, size.LowPart, digest.data(), &count);
    UnmapViewOfFile(data);
    if (!ok || count != digest.size()) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

bool DesktopClient::alive() const {
    DWORD owner{};
    return process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT &&
        GetWindowThreadProcessId(window, &owner) && owner == pid && GetPropW(window, kClientWindow) == window;
}

bool DesktopClient::requestsDiagnostics() const {
    return alive() && GetPropW(window, kDiagnosticWindow) == window;
}

winrt::handle OpenVerifiedClientProcess(DWORD pid, const std::wstring& path, const std::wstring& hash) {
    winrt::handle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!process || !QueryFullProcessImageNameW(process.get(), 0, image.data(), &length)) return {};
    image.resize(length);
    if (hash.empty() || _wcsicmp(image.c_str(), path.c_str()) || ImageHash(image) != hash) return {};
    return process;
}

DesktopClient FindDesktopClient(const std::wstring& path, const std::wstring& hash) {
    struct Search { const std::wstring& path; const std::wstring& hash; DesktopClient found; } search{path, hash, {}};
    if (path.empty() || hash.empty()) return {};
    EnumWindows([](HWND window, LPARAM arg) -> BOOL {
        if (GetPropW(window, kClientWindow) != window) return TRUE;
        auto& search = *reinterpret_cast<Search*>(arg);
        DWORD pid{}, session{};
        if (!GetWindowThreadProcessId(window, &pid) || !ProcessIdToSessionId(pid, &session) ||
            session != WTSGetActiveConsoleSessionId()) return TRUE;
        auto process = OpenVerifiedClientProcess(pid, search.path, search.hash);
        if (!process) return TRUE;
        search.found = {window, pid, std::move(process)};
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return std::move(search.found);
}

} // namespace p2premote::service
