#pragma once

#include <windows.h>

#include <string>
#include <utility>

namespace p2premote {

// Attaches the calling thread to the currently active Windows input desktop
// (Default, Winlogon for UAC/Lock Screen, or Screensaver) when it differs from
// the thread's current desktop. Returns true if the desktop switched.
inline bool SyncThreadToInputDesktop(HDESK& attached, std::wstring& current_name) noexcept {
    HDESK input = OpenInputDesktop(0, FALSE,
        DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL |
        DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | GENERIC_WRITE);
    if (!input) {
        input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
    }
    if (!input) return false;

    wchar_t name_buf[256]{};
    DWORD needed{};
    std::wstring next_name;
    if (GetUserObjectInformationW(input, UOI_NAME, name_buf, sizeof(name_buf), &needed)) {
        next_name = name_buf;
    }

    if (!next_name.empty() && next_name == current_name) {
        CloseDesktop(input);
        return false;
    }

    if (SetThreadDesktop(input)) {
        if (attached) CloseDesktop(attached);
        attached = input;
        current_name = std::move(next_name);
        return true;
    }

    CloseDesktop(input);
    return false;
}

inline bool IsSecureDesktopName(const std::wstring& name) noexcept {
    return !name.empty() && _wcsicmp(name.c_str(), L"Default") != 0;
}

inline bool SendSecureAttentionSequence() noexcept {
    using SendSasFn = VOID(WINAPI*)(BOOL as_user);
    static const SendSasFn send_sas = []() -> SendSasFn {
        if (HMODULE mod = LoadLibraryW(L"sas.dll")) {
            return reinterpret_cast<SendSasFn>(GetProcAddress(mod, "SendSAS"));
        }
        return nullptr;
    }();
    if (!send_sas) return false;
    send_sas(FALSE);
    return true;
}

} // namespace p2premote
