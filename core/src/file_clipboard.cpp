#include "file_clipboard.h"

#include <ole2.h>
#include <shlobj.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace p2premote {
namespace {

constexpr unsigned read_window = 256 * 1024;
constexpr DWORD read_timeout_ms = 15'000;
constexpr DWORD clipboard_retry_ms = 25;
constexpr auto max_position = std::numeric_limits<std::int64_t>::max();

struct NameLess {
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
            static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

bool ValidComponent(std::wstring_view name) noexcept {
    if (name.empty() || name.back() == L'.' || name.back() == L' ') return false;
    if (std::ranges::any_of(name, [](wchar_t ch) {
        return ch < L' ' || std::wstring_view(L"<>:\"/|?*").find(ch) != std::wstring_view::npos;
    })) return false;
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    const auto is = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
            b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    for (const auto reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
        if (is(stem, reserved)) return false;
    }
    return stem.size() != 4 || (!is(stem.substr(0, 3), L"COM") && !is(stem.substr(0, 3), L"LPT")) ||
        std::wstring_view(L"0123456789\u00b9\u00b2\u00b3").find(stem.back()) == std::wstring_view::npos;
}

struct Offer {
    std::uint32_t id{};
    DWORD sequence{};
    HWND owner{};
    ULONGLONG until{};
    std::vector<FILEDESCRIPTORW> descriptors;
};

std::vector<FILEDESCRIPTORW> Describe(std::span<const ClipboardFile> files) {
    if (files.empty() || files.size() > max_clipboard_files) return {};
    std::vector<FILEDESCRIPTORW> descriptors;
    std::map<std::wstring_view, bool, NameLess> names;
    descriptors.reserve(files.size());
    for (const auto& file : files) {
        std::wstring_view name(file.name);
        if (name.empty() || name.size() >= MAX_PATH || file.size > max_position) return {};
        const auto delimiter = name.rfind(L'\\');
        const auto leaf = name.substr(delimiter == name.npos ? 0 : delimiter + 1);
        if (!ValidComponent(leaf) || !names.emplace(name, file.directory).second) return {};
        if (delimiter != name.npos) {
            const auto parent = names.find(name.substr(0, delimiter));
            if (parent == names.end() || !parent->second) return {};
        }
        auto& data = descriptors.emplace_back();
        data.dwFlags = static_cast<DWORD>(FD_UNICODE) | FD_ATTRIBUTES | FD_PROGRESSUI;
        data.dwFileAttributes = file.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!file.directory) {
            data.dwFlags |= FD_FILESIZE;
            data.nFileSizeHigh = static_cast<DWORD>(file.size >> 32);
            data.nFileSizeLow = static_cast<DWORD>(file.size);
        }
        std::ranges::copy(name, data.cFileName);
    }
    return descriptors;
}

const auto descriptor_format = [] { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)); };
const auto contents_format = [] { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)); };
const auto effect_format = [] { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)); };

bool ClearOwnedClipboard() noexcept {
    const auto owner = GetClipboardOwner();
    if (!owner || GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId()) return true;
    if (!OpenClipboard(nullptr)) return false;
    const bool cleared = GetClipboardOwner() != owner || EmptyClipboard();
    CloseClipboard();
    return cleared;
}

} // namespace

struct FileClipboard::State {
    explicit State(HANDLE wake) : plugin_wake(wake) {}

    struct Range {
        ClipboardReadRequest request;
        std::string data;
        bool delivered{};
        bool failed{};
    };

    bool Active(std::uint32_t offer) {
        const std::lock_guard lock(mutex);
        return available && active == offer;
    }

    std::optional<std::string> Read(std::uint32_t offer, unsigned index, std::uint64_t offset, unsigned size) {
        std::unique_lock lock(mutex);
        if (!available || active != offer || range) return std::nullopt;
        Range next{{++next_read, offer, index, offset, size}, {}};
        if (!next.request.id) next.request.id = ++next_read;
        next.data.reserve(size);
        range.emplace(std::move(next));
        SetEvent(plugin_wake);
        auto deadline = GetTickCount64() + read_timeout_ms;
        std::size_t received{};
        const auto event = response.get();
        while (available && active == offer && !range->failed && range->data.size() < size) {
            const auto now = GetTickCount64();
            if (received != range->data.size()) {
                deadline = now + read_timeout_ms;
                received = range->data.size();
            }
            if (now >= deadline) {
                ReportError("transfer_timeout");
                break;
            }
            lock.unlock();
            // EmptyClipboard can send WM_DESTROYCLIPBOARD synchronously to
            // OLE's window, including from our text clipboard worker.
            const auto wait = MsgWaitForMultipleObjectsEx(1, &event, static_cast<DWORD>(deadline - now),
                QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 + 1) {
                MSG message{};
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            }
            lock.lock();
            if (wait == WAIT_FAILED) break;
        }
        std::optional<std::string> result;
        if (available && active == offer && !range->failed && range->data.size() == size) {
            result = std::move(range->data);
        }
        range.reset();
        return result;
    }

    // Called with mutex held. Only fixed error codes cross the worker boundary.
    void ReportError(std::string_view code) {
        error = code;
        SetEvent(plugin_wake);
    }

    const HANDLE plugin_wake;
    winrt::handle changed{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    winrt::handle response{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    mutable std::mutex mutex;
    std::condition_variable startup;
    bool started{};
    bool available{};
    std::uint32_t active{};
    std::uint32_t next_read{};
    std::shared_ptr<const Offer> pending;
    std::optional<Range> range;
    std::string_view error;
};

namespace {

class RemoteFileStream : public winrt::implements<RemoteFileStream, IStream, ISequentialStream, winrt::non_agile> {
public:
    RemoteFileStream(std::shared_ptr<FileClipboard::State> state, std::uint32_t offer, unsigned index,
        std::uint64_t size, std::uint64_t position = 0) noexcept
        : state_(std::move(state)), offer_(offer), index_(index), size_(size), position_(position) {}

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG count, ULONG* read) noexcept override {
        if (read) *read = 0;
        if (!count) return S_OK;
        if (!buffer) return STG_E_INVALIDPOINTER;
        if (!state_->Active(offer_)) return STG_E_REVERTED;
        ULONG done{};
        try {
            while (done < count && position_ < size_) {
                if (position_ < cache_start_ || position_ - cache_start_ >= cache_.size()) {
                    const auto length = static_cast<unsigned>(std::min<std::uint64_t>(read_window, size_ - position_));
                    auto data = state_->Read(offer_, index_, position_, length);
                    if (!data) return STG_E_READFAULT;
                    cache_start_ = position_;
                    cache_ = std::move(*data);
                }
                const auto offset = static_cast<std::size_t>(position_ - cache_start_);
                const auto length = static_cast<ULONG>(std::min<std::size_t>(count - done, cache_.size() - offset));
                std::memcpy(static_cast<char*>(buffer) + done, cache_.data() + offset, length);
                done += length;
                position_ += length;
                if (read) *read = done;
            }
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return done == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) noexcept override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER offset, DWORD origin, ULARGE_INTEGER* position) noexcept override {
        std::int64_t base{};
        switch (origin) {
        case STREAM_SEEK_SET: break;
        case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
        case STREAM_SEEK_END: base = static_cast<std::int64_t>(size_); break;
        default: return STG_E_INVALIDFUNCTION;
        }
        if (offset.QuadPart < -base || offset.QuadPart > max_position - base) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<std::uint64_t>(base + offset.QuadPart);
        if (position) position->QuadPart = position_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) noexcept override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read,
        ULARGE_INTEGER* written) noexcept override {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        if (!target) return STG_E_INVALIDPOINTER;
        std::array<char, 32 * 1024> buffer;
        while (count.QuadPart) {
            ULONG chunk{};
            const auto status = Read(buffer.data(), static_cast<ULONG>(std::min<std::uint64_t>(count.QuadPart, buffer.size())), &chunk);
            if (FAILED(status)) return status;
            if (!chunk) break;
            if (read) read->QuadPart += chunk;
            ULONG stored{};
            const auto write = target->Write(buffer.data(), chunk, &stored);
            if (written) written->QuadPart += stored;
            if (FAILED(write)) return write;
            if (stored != chunk) return STG_E_MEDIUMFULL;
            count.QuadPart -= chunk;
            if (status == S_FALSE) break;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) noexcept override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Revert() noexcept override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) noexcept override {
        if (!stat) return STG_E_INVALIDPOINTER;
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = size_;
        stat->grfMode = STGM_READ;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream** copy) noexcept override {
        if (!copy) return STG_E_INVALIDPOINTER;
        *copy = nullptr;
        try {
            *copy = winrt::make<RemoteFileStream>(state_, offer_, index_, size_, position_).detach();
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

private:
    const std::shared_ptr<FileClipboard::State> state_;
    const std::uint32_t offer_;
    const unsigned index_;
    const std::uint64_t size_;
    std::uint64_t position_{};
    std::uint64_t cache_start_{};
    std::string cache_;
};

class FileDataObject : public winrt::implements<FileDataObject, IDataObject, IDataObjectAsyncCapability, winrt::non_agile> {
public:
    FileDataObject(std::shared_ptr<FileClipboard::State> state, std::shared_ptr<const Offer> offer) noexcept
        : state_(std::move(state)), offer_(std::move(offer)) {}

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) noexcept override {
        if (!medium) return E_POINTER;
        *medium = {};
        const auto supported = QueryGetData(format);
        if (FAILED(supported)) return supported;
        if (format->cfFormat == contents_format()) {
            if (!state_->Active(offer_->id)) return STG_E_REVERTED;
            const auto index = format->lindex < 0 ? 0U : static_cast<unsigned>(format->lindex);
            const auto& data = offer_->descriptors[index];
            const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            try {
                medium->pstm = winrt::make<RemoteFileStream>(state_, offer_->id, index + 1, size).detach();
                medium->tymed = TYMED_ISTREAM;
                return S_OK;
            } catch (...) {
                return E_OUTOFMEMORY;
            }
        }
        const auto descriptors = format->cfFormat == descriptor_format();
        const auto bytes = descriptors ? offsetof(FILEGROUPDESCRIPTORW, fgd) +
            offer_->descriptors.size() * sizeof(FILEDESCRIPTORW) : sizeof(DWORD);
        const auto block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!block) return E_OUTOFMEMORY;
        auto* data = GlobalLock(block);
        if (!data) {
            GlobalFree(block);
            return E_OUTOFMEMORY;
        }
        if (descriptors) {
            const auto count = static_cast<UINT>(offer_->descriptors.size());
            std::memcpy(data, &count, sizeof(count));
            std::memcpy(static_cast<char*>(data) + offsetof(FILEGROUPDESCRIPTORW, fgd),
                offer_->descriptors.data(), count * sizeof(FILEDESCRIPTORW));
        } else {
            *static_cast<DWORD*>(data) = DROPEFFECT_COPY;
        }
        GlobalUnlock(block);
        medium->hGlobal = block;
        medium->tymed = TYMED_HGLOBAL;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) noexcept override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) noexcept override {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->ptd) return DV_E_DVTARGETDEVICE;
        if (format->cfFormat == contents_format()) {
            if (format->lindex < -1 || format->lindex >= static_cast<LONG>(offer_->descriptors.size())) return DV_E_LINDEX;
            return format->tymed & TYMED_ISTREAM ? S_OK : DV_E_TYMED;
        }
        if (format->cfFormat != descriptor_format() && format->cfFormat != effect_format() &&
            format->cfFormat != VirtualFileClipboardMarker()) return DV_E_FORMATETC;
        if (format->lindex != -1) return DV_E_LINDEX;
        return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) noexcept override {
        if (!output) return E_POINTER;
        *output = {};
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) noexcept override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        const FORMATETC formats[] = {
            {descriptor_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {contents_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
            {effect_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {static_cast<CLIPFORMAT>(VirtualFileClipboardMarker()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}
        };
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(std::size(formats)), formats, output);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) noexcept override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) noexcept override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) noexcept override { return OLE_E_ADVISENOTSUPPORTED; }

    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL async) noexcept override { async_ = async; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* async) noexcept override {
        if (!async) return E_POINTER;
        *async = async_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) noexcept override { copying_ = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* copying) noexcept override {
        if (!copying) return E_POINTER;
        *copying = copying_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) noexcept override { copying_ = FALSE; return S_OK; }

private:
    const std::shared_ptr<FileClipboard::State> state_;
    const std::shared_ptr<const Offer> offer_;
    BOOL async_{TRUE};
    BOOL copying_{};
};

} // namespace

UINT VirtualFileClipboardMarker() noexcept {
    static const auto format = RegisterClipboardFormatW(L"P2PRemote.VirtualFileClipboard");
    return format;
}

FileClipboard::FileClipboard(HANDLE plugin_wake) : state_(std::make_shared<State>(plugin_wake)) {
    if (!state_->changed || !state_->response || !plugin_wake) return;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
    std::unique_lock lock(state_->mutex);
    state_->startup.wait(lock, [&] { return state_->started; });
}

FileClipboard::~FileClipboard() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->available = false;
        state_->pending.reset();
    }
    worker_.request_stop();
    if (state_->response) SetEvent(state_->response.get());
    if (state_->changed) SetEvent(state_->changed.get());
    if (worker_.joinable()) worker_.join();
}

bool FileClipboard::Available() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->available;
}

bool FileClipboard::Publish(std::uint32_t offer, std::span<const ClipboardFile> files) {
    if (!offer) return false;
    const auto sequence = GetClipboardSequenceNumber();
    const auto owner = GetClipboardOwner();
    auto descriptors = Describe(files);
    if (descriptors.empty()) return false;
    const auto command = std::make_shared<Offer>(Offer{offer, sequence, owner, GetTickCount64() + 500, std::move(descriptors)});
    const std::lock_guard lock(state_->mutex);
    if (!state_->available) return false;
    state_->active = offer;
    state_->error = {};
    state_->pending = command;
    SetEvent(state_->response.get());
    SetEvent(state_->changed.get());
    return true;
}

void FileClipboard::Clear(std::uint32_t offer) {
    const std::lock_guard lock(state_->mutex);
    if (!state_->active || (offer && state_->active != offer)) return;
    state_->active = 0;
    state_->pending.reset();
    SetEvent(state_->response.get());
    SetEvent(state_->changed.get());
}

std::string_view FileClipboard::TakeError() {
    const std::lock_guard lock(state_->mutex);
    return std::exchange(state_->error, std::string_view{});
}

std::optional<ClipboardReadRequest> FileClipboard::TakeRequest() {
    const std::lock_guard lock(state_->mutex);
    if (!state_->range || state_->range->delivered || state_->range->request.offer != state_->active) return std::nullopt;
    state_->range->delivered = true;
    return state_->range->request;
}

bool FileClipboard::Reply(std::uint32_t id, std::optional<std::string_view> data) {
    const std::lock_guard lock(state_->mutex);
    const auto& range = state_->range;
    if (!range || range->request.id != id || range->request.offer != state_->active || range->failed ||
        range->data.size() == range->request.size) return false;
    if (!data || data->empty() || data->size() > range->request.size - range->data.size()) {
        state_->range->failed = true;
        SetEvent(state_->response.get());
        return !data;
    }
    state_->range->data.append(*data);
    SetEvent(state_->response.get());
    return true;
}

void FileClipboard::Run(std::stop_token stop) noexcept {
    const auto initialized = OleInitialize(nullptr);
    const bool available = SUCCEEDED(initialized) && descriptor_format() && contents_format() &&
        effect_format() && VirtualFileClipboardMarker();
    {
        const std::lock_guard lock(state_->mutex);
        state_->available = available;
        state_->started = true;
    }
    state_->startup.notify_one();
    if (FAILED(initialized)) return;

    winrt::com_ptr<IDataObject> current;
    std::uint32_t current_offer{};
    while (available && !stop.stop_requested()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        std::shared_ptr<const Offer> pending;
        std::uint32_t active{};
        {
            const std::lock_guard lock(state_->mutex);
            pending = state_->pending;
            active = state_->active;
        }
        auto wait = INFINITE;
        if (pending) {
            HRESULT result = E_ABORT;
            const auto now = GetTickCount64();
            auto error = now >= pending->until ? "clipboard_busy" : "clipboard_changed";
            if (now < pending->until && GetClipboardSequenceNumber() == pending->sequence &&
                GetClipboardOwner() == pending->owner) {
                error = "publish_failed";
                try {
                    auto object = winrt::make<FileDataObject>(state_, pending);
                    result = OleSetClipboard(object.get());
                    if (SUCCEEDED(result)) {
                        current = std::move(object);
                        current_offer = pending->id;
                    }
                } catch (...) {}
            }
            if (result == CLIPBRD_E_CANT_OPEN) {
                wait = clipboard_retry_ms;
            } else {
                const std::lock_guard lock(state_->mutex);
                if (state_->pending == pending) {
                    state_->pending.reset();
                    if (FAILED(result)) {
                        state_->active = 0;
                        state_->ReportError(error);
                    }
                }
                continue;
            }
        } else if (current && (active != current_offer || OleIsCurrentClipboard(current.get()) == S_FALSE)) {
            if (ClearOwnedClipboard()) {
                current = nullptr;
            } else {
                wait = clipboard_retry_ms;
            }
        }
        const auto event = state_->changed.get();
        if (MsgWaitForMultipleObjectsEx(1, &event, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) break;
    }
    if (current) ClearOwnedClipboard();
    current = nullptr;
    OleUninitialize();
    const std::lock_guard lock(state_->mutex);
    state_->available = false;
}

} // namespace p2premote
