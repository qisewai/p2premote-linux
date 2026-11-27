#include "desktop_client.h"

#include <iostream>

using namespace p2premote::service;

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--own-lease") {
        winrt::handle lease{CreateMutexW(nullptr, TRUE, argv[2])};
        if (!lease) return 1;
        Sleep(8000);
        return 0;
    }
    std::wstring image(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!length || length >= image.size()) return 1;
    image.resize(length);
    const auto hash = ImageHash(image);
    const auto name = L"Local\\P2PRemote.TestLease." + std::to_wstring(GetCurrentProcessId());
    auto command = L"\"" + image + L"\" --own-lease " + name;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
        &startup, &child)) return 1;
    const winrt::handle process{child.hProcess}, thread{child.hThread};
    auto bound = OpenVerifiedClientProcess(child.dwProcessId, image, hash);
    winrt::handle lease;
    for (int i = 0; i < 200 && !lease; ++i) {
        lease.attach(OpenMutexW(SYNCHRONIZE, FALSE, name.c_str()));
        if (!lease) Sleep(10);
    }
    const bool live = bound && lease && WaitForSingleObject(bound.get(), 0) == WAIT_TIMEOUT &&
        WaitForSingleObject(lease.get(), 0) == WAIT_TIMEOUT &&
        !OpenVerifiedClientProcess(child.dwProcessId, image, L"unregistered");
    if (!TerminateProcess(process.get(), 0)) return 1;
    if (!live || WaitForSingleObject(bound.get(), 2000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(lease.get(), 2000) != WAIT_ABANDONED) {
        std::cerr << "Registered process or foreground lease did not follow child lifetime\n";
        return 1;
    }
    return 0;
}
