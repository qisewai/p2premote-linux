#include "pty_session.h"

#include <windows.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace p2premote {
namespace {

constexpr std::size_t chunk_size = 8192;
constexpr std::size_t queue_limit = 256 * 1024;

std::wstring ShellPath(std::string_view shell) {
    if (shell != "powershell" && shell != "cmd") return {};
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring path(system, length);
    path += shell == "powershell" ? L"\\WindowsPowerShell\\v1.0\\powershell.exe" : L"\\cmd.exe";
    return path;
}

COORD GridSize(std::uint16_t cols, std::uint16_t rows) noexcept {
    return {static_cast<SHORT>(std::clamp<std::uint16_t>(cols, 20, 400)),
        static_cast<SHORT>(std::clamp<std::uint16_t>(rows, 5, 200))};
}

void CancelAndJoin(std::jthread& thread) noexcept {
    if (!thread.joinable()) return;
    const auto handle = thread.native_handle();
    while (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) {
        // CancelSynchronousIo can race the worker's next ReadFile / WriteFile.
        CancelSynchronousIo(handle);
        if (WaitForSingleObject(handle, 20) != WAIT_TIMEOUT) break;
    }
    thread.join();
}

} // namespace

struct PtySession::Impl {
    ~Impl() { Close(); }

    void Start(std::string_view shell, std::uint16_t cols, std::uint16_t rows) {
        const auto exe = ShellPath(shell);
        if (exe.empty()) {
            error = "无法确定系统终端的路径";
            return;
        }
        auto command = L"\"" + exe + L"\"";
        if (shell == "powershell") command += L" -NoLogo";

        winrt::handle pty_in;
        winrt::handle pty_out;
        if (!CreatePipe(pty_in.put(), input.put(), nullptr, 0) ||
            !CreatePipe(output.put(), pty_out.put(), nullptr, 0)) {
            error = "无法创建终端管道";
            return;
        }
        if (FAILED(CreatePseudoConsole(GridSize(cols, rows), pty_in.get(), pty_out.get(), 0, &console))) {
            error = "无法创建 Windows ConPTY 伪终端";
            return;
        }
        pty_in.close();
        pty_out.close();
        reader = std::jthread([this] { ReadLoop(); });

        SIZE_T attr_size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        if (!attr_size) {
            error = "无法确定伪终端进程属性大小";
            return;
        }
        std::vector<BYTE> attr_storage(attr_size);
        auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
        if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
            error = "无法初始化伪终端进程属性";
            return;
        }
        struct Attributes {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~Attributes() { DeleteProcThreadAttributeList(list); }
        } attributes{attr_list};
        if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                console, sizeof(console), nullptr, nullptr)) {
            error = "无法绑定伪终端进程属性";
            return;
        }

        job.attach(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            error = "无法创建终端进程组";
            return;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = attr_list;
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                nullptr, nullptr, &startup.StartupInfo, &child)) {
            error = "无法启动终端进程";
            return;
        }
        process.attach(child.hProcess);
        const winrt::handle primary_thread{child.hThread};
        if (!AssignProcessToJobObject(job.get(), process.get()) ||
            ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) {
            TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
            error = "无法启动终端进程组";
            return;
        }
        writer = std::jthread([this] { WriteLoop(); });
        valid = true;
    }

    void ReadLoop() noexcept {
        try {
            char buffer[chunk_size];
            while (!stop_io) {
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return closing || queued_output <= queue_limit - chunk_size; });
                }
                DWORD read{};
                if (stop_io || !ReadFile(output.get(), buffer, sizeof(buffer), &read, nullptr) || !read) break;
                const std::lock_guard lock(mutex);
                if (!closing) {
                    output_queue.emplace_back(buffer, read);
                    queued_output += read;
                }
            }
        } catch (...) {}
        const std::lock_guard lock(mutex);
        output_ended = true;
    }

    void WriteLoop() noexcept {
        while (!stop_io) {
            std::string data;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return closing || !input_queue.empty(); });
                if (closing) break;
                data = std::move(input_queue.front());
                input_queue.pop_front();
                queued_input -= data.size();
            }
            std::size_t done{};
            while (!stop_io && done < data.size()) {
                DWORD written{};
                if (!WriteFile(input.get(), data.data() + done, static_cast<DWORD>(data.size() - done),
                        &written, nullptr) || !written) break;
                done += written;
            }
            if (done != data.size()) break;
        }
        const std::lock_guard lock(mutex);
        input_ended = true;
    }

    bool Write(std::string_view data) {
        if (!valid || data.empty() || data.size() > chunk_size) return false;
        {
            const std::lock_guard lock(mutex);
            if (closing || input_ended || data.size() > queue_limit - queued_input) return false;
            input_queue.emplace_back(data);
            queued_input += data.size();
        }
        ready.notify_all();
        return true;
    }

    Output Read() {
        if (!valid) return {};
        const bool exited = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
        if (exited && console) {
            // ConPTY may keep its pipe open after the shell exits. Shutdown on
            // a separate thread while the reader still delivers the last bytes.
            closer = std::jthread([handle = console] { ClosePseudoConsole(handle); });
            console = nullptr;
        }
        const std::lock_guard lock(mutex);
        if (!output_queue.empty()) {
            auto bytes = std::move(output_queue.front());
            output_queue.pop_front();
            queued_output -= bytes.size();
            ready.notify_all();
            return {std::move(bytes), std::nullopt};
        }
        if (!output_ended) return {};
        DWORD code = ERROR_BROKEN_PIPE;
        if (exited) GetExitCodeProcess(process.get(), &code);
        return {{}, code};
    }

    bool Resize(std::uint16_t cols, std::uint16_t rows) {
        return valid && console && SUCCEEDED(ResizePseudoConsole(console, GridSize(cols, rows)));
    }

    void Close() noexcept {
        bool reader_exited{};
        {
            const std::lock_guard lock(mutex);
            if (closing) return;
            closing = true;
            reader_exited = output_ended;
        }
        ready.notify_all();
        job.close();
        CancelAndJoin(writer);
        input.close();

        // ClosePseudoConsole can block until the output is drained on older
        // Windows. Keep the reader running and discarding bytes until it returns.
        if (!reader.joinable() || reader_exited) output.close();
        if (console) ClosePseudoConsole(std::exchange(console, nullptr));
        if (closer.joinable()) closer.join();

        stop_io = true;
        CancelAndJoin(reader);
        output.close();
        process.close();
    }

    std::string error;
    bool valid{};
    bool closing{};
    bool output_ended{};
    bool input_ended{};
    std::atomic_bool stop_io{};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> input_queue;
    std::deque<std::string> output_queue;
    std::size_t queued_input{};
    std::size_t queued_output{};
    HPCON console{};
    winrt::handle input;
    winrt::handle output;
    winrt::handle job;
    winrt::handle process;
    std::jthread reader;
    std::jthread writer;
    std::jthread closer;
};

PtySession::PtySession(std::string_view shell, std::uint16_t cols, std::uint16_t rows)
    : impl_(std::make_unique<Impl>()) { impl_->Start(shell, cols, rows); }
PtySession::~PtySession() = default;
bool PtySession::Valid() const noexcept { return impl_->valid; }
const std::string& PtySession::Error() const noexcept { return impl_->error; }
bool PtySession::Write(std::string_view data) { return impl_->Write(data); }
PtySession::Output PtySession::Read() { return impl_->Read(); }
bool PtySession::Resize(std::uint16_t cols, std::uint16_t rows) { return impl_->Resize(cols, rows); }
void PtySession::Close() { impl_->Close(); }

} // namespace p2premote
