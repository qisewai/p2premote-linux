#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace p2premote {

class PtySession {
public:
    struct Output {
        std::string bytes;
        std::optional<std::uint32_t> exit_code;
    };

    PtySession(std::string_view shell, std::uint16_t cols, std::uint16_t rows);
    ~PtySession();
    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    bool Valid() const noexcept;
    const std::string& Error() const noexcept;
    bool Write(std::string_view data);
    Output Read();
    bool Resize(std::uint16_t cols, std::uint16_t rows);
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
