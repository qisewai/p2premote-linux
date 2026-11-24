#include "clipboard_bridge.h"
#include "file_clipboard.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

namespace p2premote {
namespace {

constexpr wchar_t clipboard_class[] = L"P2PRemote.PluginClipboard";
constexpr std::size_t text_limit = 60 * 1024;
constexpr DWORD retry_ms = 25;
// Multiple StreamSessions in one process share the same Win32 clipboard.
std::atomic<DWORD> remote_sequence{};

struct ClipboardLock {
    ~ClipboardLock() { CloseClipboard(); }
};

struct GlobalMemoryLock {
    HGLOBAL handle;
    ~GlobalMemoryLock() { GlobalUnlock(handle); }
};

std::string UnicodeClipboardText() {
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return {};
    const auto capacity = std::min(GlobalSize(handle) / sizeof(wchar_t), text_limit + 1);
    if (!capacity) return {};
    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!text) return {};
    const GlobalMemoryLock lock{handle};
    const auto end = std::find(text, text + capacity, L'\0');
    if (end == text || end == text + capacity) return {};
    const auto length = static_cast<int>(end - text);
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0 || static_cast<std::size_t>(size) > text_limit) return {};
    std::string utf8(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, utf8.data(), size, nullptr, nullptr) != size) return {};
    return utf8;
}

std::vector<LocalClipboardFile> DroppedFiles() {
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) return {};
    const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    if (count > max_clipboard_files) return {};
    std::vector<LocalClipboardFile> files;
    files.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        wchar_t path[MAX_PATH]{};
        const auto length = DragQueryFileW(drop, index, nullptr, 0);
        if (!length || length >= std::size(path) || DragQueryFileW(drop, index, path, MAX_PATH) != length) return {};
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, length, nullptr, 0, nullptr, nullptr);
        if (!size) return {};
        std::string utf8(size, '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, length, utf8.data(), size, nullptr, nullptr) != size) return {};
        const auto last_slash = utf8.find_last_of("\\/");
        auto name = utf8.substr(last_slash == std::string::npos ? 0 : last_slash + 1);
        if (!name.empty()) files.push_back({std::move(utf8), std::move(name)});
    }
    return files;
}

} // namespace

ClipboardBridge::ClipboardBridge() {
    const auto module = GetModuleHandleW(nullptr);
    static const auto registered = [module] {
        WNDCLASSW registration{};
        registration.lpfnWndProc = WindowProc;
        registration.hInstance = module;
        registration.lpszClassName = clipboard_class;
        return RegisterClassW(&registration) != 0;
    }();
    if (!registered) return;
    window_ = CreateWindowExW(0, clipboard_class, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, this);
    if (window_ && !AddClipboardFormatListener(window_)) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

ClipboardBridge::~ClipboardBridge() {
    if (window_) {
        RemoveClipboardFormatListener(window_);
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK ClipboardBridge::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (message == WM_CLIPBOARDUPDATE) {
        if (auto* bridge = reinterpret_cast<ClipboardBridge*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
            bridge->dirty_ = true;
            bridge->read_at_ = 0;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool ClipboardBridge::SetText(std::string_view text) {
    if (!window_ || text.empty() || text.size() > text_limit || text.find('\0') != std::string_view::npos) return false;
    const auto length = static_cast<int>(text.size());
    const auto chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!chars) return false;
    std::wstring unicode(chars, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, unicode.data(), chars) != chars) return false;
    pending_ = std::move(unicode);
    pending_utf8_.assign(text);
    write_sequence_ = GetClipboardSequenceNumber();
    const auto now = GetTickCount64();
    write_at_ = now;
    write_until_ = now + 500;
    const auto result = Write();
    if (result == WriteResult::written) {
        CancelWrite();
        return true;
    }
    if (result == WriteResult::aborted) {
        CancelWrite();
        return false;
    }
    write_at_ = now + retry_ms;
    return false;
}

void ClipboardBridge::CancelWrite() noexcept {
    pending_.clear();
    pending_utf8_.clear();
}

ClipboardBridge::WriteResult ClipboardBridge::Write() {
    if (!OpenClipboard(window_)) return WriteResult::retry;
    const ClipboardLock clipboard;
    // A local copy made while a clipboard write was waiting takes priority.
    if (GetClipboardSequenceNumber() != write_sequence_) return WriteResult::aborted;
    const auto size = (pending_.size() + 1) * sizeof(wchar_t);
    const auto buffer = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!buffer) return WriteResult::aborted;
    if (auto* output = GlobalLock(buffer)) {
        std::memcpy(output, pending_.c_str(), size);
        GlobalUnlock(buffer);
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, buffer)) {
            const auto sequence = GetClipboardSequenceNumber();
            remote_sequence.store(sequence);
            text_ = pending_utf8_;
            files_.clear();
            sequence_ = sequence;
            initialized_ = true;
            dirty_ = false;
            return WriteResult::written;
        }
    }
    GlobalFree(buffer);
    return WriteResult::aborted;
}

bool ClipboardBridge::Read() {
    if (!OpenClipboard(window_)) return false;
    const ClipboardLock clipboard;
    const auto before = GetClipboardSequenceNumber();
    if (initialized_ && before && before == sequence_) {
        dirty_ = false;
        return false;
    }
    const auto marker = watch_files_ ? VirtualFileClipboardMarker() : 0;
    const bool virtual_files = marker && IsClipboardFormatAvailable(marker);
    const bool dropped_files = watch_files_ && IsClipboardFormatAvailable(CF_HDROP);
    text_ = !virtual_files && !dropped_files && IsClipboardFormatAvailable(CF_UNICODETEXT) ?
        UnicodeClipboardText() : std::string{};
    files_ = !virtual_files && dropped_files ? DroppedFiles() : std::vector<LocalClipboardFile>{};
    sequence_ = GetClipboardSequenceNumber();
    dirty_ = false;
    if (!std::exchange(initialized_, true)) return false;
    return !virtual_files && (sequence_ == 0 || sequence_ != remote_sequence.load());
}

bool ClipboardBridge::Poll(ULONGLONG now) {
    if (!window_) return false;
    if (!pending_.empty() && write_at_ <= now) {
        if (now >= write_until_ || Write() != WriteResult::retry) CancelWrite();
        else write_at_ = now + retry_ms;
    }
    if (!dirty_ || now < read_at_) return false;
    const auto changed = Read();
    if (dirty_) read_at_ = now + retry_ms;
    return changed;
}

DWORD ClipboardBridge::Wait(ULONGLONG now) const noexcept {
    if (!window_) return INFINITE;
    const auto delay = [now](ULONGLONG value) { return value <= now ? 0 : static_cast<DWORD>(value - now); };
    return std::min(dirty_ ? delay(read_at_) : INFINITE, pending_.empty() ? INFINITE : delay(write_at_));
}

} // namespace p2premote
