#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace p2premote {

inline constexpr std::size_t max_clipboard_files = 512;

struct ClipboardFile {
    std::wstring name;
    std::uint64_t size{};
    bool directory{};
};

struct ClipboardReadRequest {
    std::uint32_t id{};
    std::uint32_t offer{};
    unsigned index{};
    std::uint64_t offset{};
    unsigned size{};
};

UINT VirtualFileClipboardMarker() noexcept;

class FileClipboard {
public:
    explicit FileClipboard(HANDLE plugin_wake);
    ~FileClipboard();
    FileClipboard(const FileClipboard&) = delete;
    FileClipboard& operator=(const FileClipboard&) = delete;

    bool Available() const noexcept;
    bool Publish(std::uint32_t offer, std::span<const ClipboardFile> files);
    void Clear(std::uint32_t offer = 0);
    std::string_view TakeError();
    std::optional<ClipboardReadRequest> TakeRequest();
    bool Reply(std::uint32_t id, std::optional<std::string_view> data);

    struct State;

private:
    void Run(std::stop_token stop) noexcept;
    const std::shared_ptr<State> state_;
    std::jthread worker_;
};

} // namespace p2premote
