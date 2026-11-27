#include "pty_session.h"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace p2premote {
namespace {

constexpr std::size_t chunk_size = 8192;
constexpr std::size_t queue_limit = 256 * 1024;

bool IsExecutableFile(const char* path) noexcept {
    return path && *path == '/' && access(path, X_OK) == 0;
}

std::string ResolveShellPath(std::string_view requested) {
    if (requested == "bash" && IsExecutableFile("/bin/bash")) return "/bin/bash";
    if (requested == "zsh" && IsExecutableFile("/bin/zsh")) return "/bin/zsh";
    if (requested == "zsh" && IsExecutableFile("/usr/bin/zsh")) return "/usr/bin/zsh";
    if (requested == "sh" && IsExecutableFile("/bin/sh")) return "/bin/sh";

    // Default (including Windows viewer default "powershell"/"cmd"): use user's login shell.
    if (const char* env_shell = std::getenv("SHELL"); IsExecutableFile(env_shell)) {
        return env_shell;
    }
    for (const char* candidate : {"/bin/bash", "/bin/zsh", "/usr/bin/zsh", "/bin/sh"}) {
        if (IsExecutableFile(candidate)) return candidate;
    }
    return {};
}

winsize MakeWinsize(std::uint16_t cols, std::uint16_t rows) noexcept {
    winsize ws{};
    ws.ws_col = std::clamp<std::uint16_t>(cols, 20, 400);
    ws.ws_row = std::clamp<std::uint16_t>(rows, 5, 200);
    return ws;
}

bool SetNonBlockingCloexec(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    const int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) return false;
    return true;
}

} // namespace

struct PtySession::Impl {
    ~Impl() { Close(); }

    void Start(std::string_view shell, std::uint16_t cols, std::uint16_t rows) {
        const auto shell_path = ResolveShellPath(shell);
        if (shell_path.empty()) {
            error = "无法找到可用的系统 Shell";
            return;
        }
        if (pipe(wake_pipe) != 0) {
            error = "无法创建终端唤醒管道";
            return;
        }
        SetNonBlockingCloexec(wake_pipe[0]);
        SetNonBlockingCloexec(wake_pipe[1]);

        winsize ws = MakeWinsize(cols, rows);
        int fd = -1;
        const pid_t child = forkpty(&fd, nullptr, nullptr, &ws);
        if (child < 0) {
            error = "无法创建 POSIX 伪终端";
            CloseWakePipe();
            return;
        }
        if (child == 0) {
            // Child process: clean environment and exec login shell.
            signal(SIGPIPE, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            setenv("TERM", "xterm-256color", 1);
            setenv("COLORTERM", "truecolor", 1);
            if (const char* home = std::getenv("HOME"); home && *home) {
                if (chdir(home) != 0) { /* stay in inherited cwd */ }
            }
            const char* slash = std::strrchr(shell_path.c_str(), '/');
            const std::string argv0 = "-" + std::string(slash ? slash + 1 : shell_path.c_str());
            execl(shell_path.c_str(), argv0.c_str(), nullptr);
            execl(shell_path.c_str(), shell_path.c_str(), nullptr);
            _exit(127);
        }

        master_fd = fd;
        child_pid = child;
        SetNonBlockingCloexec(master_fd);

        reader = std::jthread([this] { ReadLoop(); });
        writer = std::jthread([this] { WriteLoop(); });
        valid = true;
    }

    void WakeIo() noexcept {
        if (wake_pipe[1] >= 0) {
            const char b = 1;
            [[maybe_unused]] const auto r = write(wake_pipe[1], &b, 1);
        }
    }

    void CloseWakePipe() noexcept {
        if (wake_pipe[0] >= 0) { close(wake_pipe[0]); wake_pipe[0] = -1; }
        if (wake_pipe[1] >= 0) { close(wake_pipe[1]); wake_pipe[1] = -1; }
    }

    void ReadLoop() noexcept {
        try {
            char buffer[chunk_size];
            while (!stop_io.load(std::memory_order_acquire)) {
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return closing || queued_output <= queue_limit - chunk_size; });
                    if (closing) break;
                }
                pollfd fds[2]{
                    {master_fd, POLLIN, 0},
                    {wake_pipe[0], POLLIN, 0},
                };
                const int pr = poll(fds, 2, 200);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (pr == 0) continue;
                if (fds[1].revents & POLLIN) {
                    if (stop_io.load(std::memory_order_acquire)) break;
                }
                if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                    const ssize_t n = read(master_fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        const std::lock_guard lock(mutex);
                        if (!closing) {
                            output_queue.emplace_back(buffer, static_cast<std::size_t>(n));
                            queued_output += static_cast<std::size_t>(n);
                        }
                    } else if (n == 0) {
                        break;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        break;
                    }
                }
            }
        } catch (...) {}
        const std::lock_guard lock(mutex);
        output_ended = true;
    }

    void WriteLoop() noexcept {
        while (!stop_io.load(std::memory_order_acquire)) {
            std::string data;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return closing || !input_queue.empty(); });
                if (closing) break;
                data = std::move(input_queue.front());
                input_queue.pop_front();
                queued_input -= data.size();
            }
            std::size_t done = 0;
            while (!stop_io.load(std::memory_order_acquire) && done < data.size()) {
                pollfd fds[2]{
                    {master_fd, POLLOUT, 0},
                    {wake_pipe[0], POLLIN, 0},
                };
                const int pr = poll(fds, 2, 200);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (pr == 0) continue;
                if (fds[1].revents & POLLIN && stop_io.load(std::memory_order_acquire)) break;
                if (fds[0].revents & (POLLERR | POLLHUP)) break;
                if (fds[0].revents & POLLOUT) {
                    const ssize_t n = write(master_fd, data.data() + done, data.size() - done);
                    if (n > 0) {
                        done += static_cast<std::size_t>(n);
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        break;
                    }
                }
            }
            if (done != data.size()) break;
        }
        const std::lock_guard lock(mutex);
        input_ended = true;
    }

    void PollExitLocked() noexcept {
        if (exited || child_pid <= 0) return;
        int status = 0;
        const pid_t r = waitpid(child_pid, &status, WNOHANG);
        if (r == child_pid) {
            exited = true;
            if (WIFEXITED(status)) exit_code = static_cast<std::uint32_t>(WEXITSTATUS(status));
            else if (WIFSIGNALED(status)) exit_code = static_cast<std::uint32_t>(128 + WTERMSIG(status));
            else exit_code = 1;
        }
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
        const std::lock_guard lock(mutex);
        PollExitLocked();
        if (!output_queue.empty()) {
            auto bytes = std::move(output_queue.front());
            output_queue.pop_front();
            queued_output -= bytes.size();
            ready.notify_all();
            return {std::move(bytes), std::nullopt};
        }
        if (output_ended || exited) {
            return {{}, exit_code};
        }
        return {};
    }

    bool Resize(std::uint16_t cols, std::uint16_t rows) noexcept {
        if (!valid || master_fd < 0) return false;
        winsize ws = MakeWinsize(cols, rows);
        return ioctl(master_fd, TIOCSWINSZ, &ws) == 0;
    }

    void Close() noexcept {
        if (closed.exchange(true, std::memory_order_acq_rel)) return;
        valid = false;
        stop_io.store(true, std::memory_order_release);
        {
            const std::lock_guard lock(mutex);
            closing = true;
        }
        ready.notify_all();
        WakeIo();

        if (child_pid > 0 && !exited) {
            kill(-child_pid, SIGHUP);
            kill(child_pid, SIGHUP);
        }
        if (writer.joinable()) writer.join();
        if (reader.joinable()) reader.join();

        if (master_fd >= 0) {
            close(master_fd);
            master_fd = -1;
        }
        if (child_pid > 0 && !exited) {
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                if (waitpid(child_pid, &status, WNOHANG) == child_pid) {
                    exited = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!exited) {
                kill(-child_pid, SIGKILL);
                kill(child_pid, SIGKILL);
                int status = 0;
                while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
                exited = true;
            }
        }
        child_pid = -1;
        CloseWakePipe();
    }

    std::string error;
    int master_fd{-1};
    int wake_pipe[2]{-1, -1};
    pid_t child_pid{-1};
    std::atomic_bool stop_io{false};
    std::atomic_bool closed{false};
    bool valid{};
    std::mutex mutex;
    std::condition_variable ready;
    bool closing{};
    bool exited{};
    bool output_ended{};
    bool input_ended{};
    std::uint32_t exit_code{};
    std::deque<std::string> output_queue;
    std::deque<std::string> input_queue;
    std::size_t queued_output{};
    std::size_t queued_input{};
    std::jthread reader;
    std::jthread writer;
};

PtySession::PtySession(std::string_view shell, std::uint16_t cols, std::uint16_t rows)
    : impl_(std::make_unique<Impl>()) {
    impl_->Start(shell, cols, rows);
}

PtySession::~PtySession() = default;
bool PtySession::Valid() const noexcept { return impl_ && impl_->valid; }
const std::string& PtySession::Error() const noexcept { return impl_->error; }
bool PtySession::Write(std::string_view data) { return impl_ && impl_->Write(data); }
PtySession::Output PtySession::Read() { return impl_ ? impl_->Read() : Output{}; }
bool PtySession::Resize(std::uint16_t cols, std::uint16_t rows) { return impl_ && impl_->Resize(cols, rows); }
void PtySession::Close() { if (impl_) impl_->Close(); }

} // namespace p2premote
