#pragma once

#include <windows.h>
#include <winrt/base.h>

#include <string>

namespace p2premote::service {

struct DesktopClient {
    HWND window{};
    DWORD pid{};
    winrt::handle process;

    bool alive() const;
    bool requestsDiagnostics() const;
};

std::wstring ImageHash(const std::wstring& path);
winrt::handle OpenVerifiedClientProcess(DWORD pid, const std::wstring& path, const std::wstring& hash);
DesktopClient FindDesktopClient(const std::wstring& path, const std::wstring& hash);

} // namespace p2premote::service
