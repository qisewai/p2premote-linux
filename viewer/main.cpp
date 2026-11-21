#include "stream_window.h"

#include <winrt/base.h>
#include <utility>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    int code = 1;
    try {
        auto options = ReadStreamOptions(false);
        code = RunStreamWindow(instance, show, false, std::move(options));
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"P2P Remote Viewer", MB_ICONERROR);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"P2P Remote Viewer", MB_ICONERROR);
    }
    winrt::uninit_apartment();
    return code;
}
