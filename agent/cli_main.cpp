#include "live_client.h"
#include "snapshot.h"

#include <iostream>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    using namespace p2premote::agent;
    constexpr const char* usage =
        "Usage: p2p-cli.exe devices --live <sharing-code>\n"
        "       p2p-cli.exe system-info --live <sharing-code> <device-id>\n"
        "       p2p-cli.exe screen-snapshot --live <sharing-code> <device-id>\n"
        "       p2p-cli.exe system-info --snapshot <absolute-path-to-exported-json>\n";
    const std::wstring_view command = argc > 1 ? argv[1] : L"";
    const std::wstring_view mode = argc > 2 ? argv[2] : L"";
    if (argc == 2 && command == L"--help") {
        std::cout << usage;
        return 0;
    }
    const bool snapshot = argc == 4 && command == L"system-info" && mode == L"--snapshot";
    const bool devices = argc == 4 && command == L"devices" && mode == L"--live";
    const bool diagnostic = argc == 5 && mode == L"--live" &&
        (command == L"system-info" || command == L"screen-snapshot");
    if (!snapshot && !devices && !diagnostic) {
        std::cerr << usage;
        return 2;
    }
    try {
        winrt::init_apartment();
        const auto op = devices ? LiveOperation::devices : command == L"screen-snapshot"
            ? LiveOperation::screen_snapshot : LiveOperation::system_info;
        const auto result = snapshot ? LoadDiagnosticSnapshot(argv[3]) : QueryLocalAgent(argv[3], op,
            diagnostic ? argv[4] : L"");
        std::cout << winrt::to_string(result.Stringify()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    } catch (...) {
        std::cerr << "Unable to read diagnostics.\n";
    }
    return 1;
}
