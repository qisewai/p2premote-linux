#include "single_instance.h"

#include <QDeadlineTimer>
#include <QThread>
#include <QWidget>

#include <windows.h>
#include <sddl.h>
#include <winrt/base.h>

namespace {

UINT activationMessage() {
    static const UINT message = RegisterWindowMessageW(L"P2PRemote.Desktop.Activate");
    return message;
}

std::wstring instanceName() {
    winrt::handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) return {};

    // TOKEN_USER points into the same buffer at the user SID.
    alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD size{};
    if (!GetTokenInformation(token.get(), TokenUser, buffer, sizeof(buffer), &size)) return {};
    wchar_t* sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer)->User.Sid, &sid)) return {};
    const std::wstring name = std::wstring(L"P2PRemote.Desktop.") + sid;
    LocalFree(sid);
    return name;
}

bool activateExistingWindow(const wchar_t* name) {
    struct Search {
        const wchar_t* name;
        HWND window{};
    } search{name};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        if (!GetPropW(window, search.name)) return TRUE;
        search.window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));

    DWORD pid{};
    if (!search.window || !GetWindowThreadProcessId(search.window, &pid)) return false;
    AllowSetForegroundWindow(pid);
    DWORD_PTR accepted{};
    return SendMessageTimeoutW(search.window, activationMessage(), 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 250, &accepted) && accepted;
}

} // namespace

SingleInstance::SingleInstance() : name_(instanceName()) {}

SingleInstance::~SingleInstance() {
    if (mutex_) CloseHandle(mutex_);
}

SingleInstance::Result SingleInstance::start(bool elevated_restart) {
    if (name_.empty() || !activationMessage()) return Result::failed;

    // Local objects are scoped to the Windows session. The SID separates users in the same session.
    const auto mutex_name = L"Local\\" + name_;
    const QDeadlineTimer deadline(elevated_restart ? 15'000 : 5'000);
    do {
        const auto mutex = CreateMutexExW(nullptr, mutex_name.c_str(), 0, SYNCHRONIZE);
        if (!mutex) return Result::failed;
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            mutex_ = mutex;
            return Result::primary;
        }
        CloseHandle(mutex);

        // A second launch may arrive before the first window exists or while it is closing.
        if (!elevated_restart && activateExistingWindow(name_.c_str())) return Result::activated;
        QThread::msleep(50);
    } while (!deadline.hasExpired());
    return Result::failed;
}

bool SingleInstance::registerWindow(QWidget& widget) {
    const auto window = reinterpret_cast<HWND>(widget.winId());
    // Only the parameterless activation message is accepted from lower-integrity clients.
    ChangeWindowMessageFilterEx(window, activationMessage(), MSGFLT_ALLOW, nullptr);
    if (!SetPropW(window, name_.c_str(), window)) return false;
    window_ = window;
    return true;
}

void SingleInstance::unregisterWindow() {
    if (window_) RemovePropW(static_cast<HWND>(window_), name_.c_str());
    window_ = nullptr;
}

bool SingleInstance::isActivationMessage(unsigned message) {
    return message != 0 && message == activationMessage();
}
