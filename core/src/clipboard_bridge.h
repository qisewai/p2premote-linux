#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace p2premote {

struct LocalClipboardFile {
    std::string path;
    std::string name;
};

class ClipboardBridge {
public:
    ClipboardBridge();
    ~ClipboardBridge();
    ClipboardBridge(const ClipboardBridge&) = delete;
    ClipboardBridge& operator=(const ClipboardBridge&) = delete;

    // These methods and the message window belong to the plugin worker.
    bool Available() const noexcept { return window_ != nullptr; }
    std::string_view Text() const noexcept { return text_; }
    const std::vector<LocalClipboardFile>& Files() const noexcept { return files_; }
    std::string_view FileIssue() const noexcept { return file_issue_; }
    void WatchFiles() noexcept { watch_files_ = true; }
    bool SetText(std::string_view text);
    bool Poll(ULONGLONG now);
    DWORD Wait(ULONGLONG now) const noexcept;
    void CancelWrite() noexcept;

private:
    enum class WriteResult { written, retry, aborted };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    bool Read();
    WriteResult Write();

    HWND window_{};
    bool initialized_{};
    bool watch_files_{};
    bool dirty_{true};
    DWORD sequence_{};
    ULONGLONG read_at_{};
    ULONGLONG write_at_{};
    ULONGLONG write_until_{};
    DWORD write_sequence_{};
    std::wstring pending_;
    std::string pending_utf8_;
    std::string text_;
    std::vector<LocalClipboardFile> files_;
    std::string_view file_issue_;
};

} // namespace p2premote
