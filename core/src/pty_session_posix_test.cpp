#include "pty_session.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main() {
    p2premote::PtySession pty("sh", 80, 24);
    if (!pty.Valid()) {
        std::cerr << "PtySession failed to start: " << pty.Error() << "\n";
        return 1;
    }
    if (!pty.Resize(100, 30)) {
        std::cerr << "PtySession Resize failed\n";
        return 1;
    }
    if (!pty.Write("echo P2PREMOTE_PTY_OK; exit 0\n")) {
        std::cerr << "PtySession Write failed\n";
        return 1;
    }

    std::string collected;
    std::optional<std::uint32_t> exit_code;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto out = pty.Read();
        if (!out.bytes.empty()) collected += out.bytes;
        if (out.exit_code) {
            exit_code = out.exit_code;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    pty.Close();

    if (collected.find("P2PREMOTE_PTY_OK") == std::string::npos) {
        std::cerr << "Expected marker missing from PTY output: " << collected << "\n";
        return 1;
    }
    if (!exit_code || *exit_code != 0) {
        std::cerr << "Unexpected PTY exit code\n";
        return 1;
    }
    std::cout << "pty_session_posix_test passed\n";
    return 0;
}
