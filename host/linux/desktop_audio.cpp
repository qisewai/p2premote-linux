#include "desktop_audio.h"

#include <pulse/pulseaudio.h>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace p2premote::linux_host {
namespace {

using namespace std::chrono_literals;
constexpr pa_sample_spec kSampleSpec{PA_SAMPLE_S16LE, 48'000, 2};
constexpr std::size_t kFrameBytes = 480 * 2 * sizeof(std::int16_t); // 10 ms of stereo PCM
using PcmFrame = std::array<char, kFrameBytes>;
class AudioProcess {
public:
    AudioProcess() = default;
    ~AudioProcess() { Stop(); }
    AudioProcess(const AudioProcess&) = delete;
    AudioProcess& operator=(const AudioProcess&) = delete;

    void Start(const DesktopTarget& session, std::string executable) {
        if (executable.empty()) return;
        auto runtime = "XDG_RUNTIME_DIR=" + session.runtime_dir;
        char* env[]{runtime.data(), nullptr};
        std::string capture_arg = "--capture-desktop-audio";
        char* user_args[]{executable.data(), capture_arg.data(), nullptr};
        char** args = user_args;
        const char* program = executable.c_str();
        std::array<std::string, 8> drop_privileges;
        std::array<char*, 9> privileged_args{};
        if (geteuid() == 0) {
            std::array<char, 16384> buffer{};
            struct passwd account{};
            struct passwd* found{};
            if (getpwuid_r(session.uid, &account, buffer.data(), buffer.size(), &found) != 0 || !found) return;
            // Interpret the user's audio protocol only after permanently dropping root.
            drop_privileges = {"/usr/bin/setpriv", "--reuid=" + std::to_string(session.uid),
                "--regid=" + std::to_string(account.pw_gid), "--clear-groups", "--no-new-privs", "--",
                executable, capture_arg};
            std::transform(drop_privileges.begin(), drop_privileges.end(), privileged_args.begin(),
                [](std::string& arg) { return arg.data(); });
            args = privileged_args.data();
            program = args[0];
        } else if (geteuid() != session.uid) {
            return;
        }

        int pipe[2];
        if (pipe2(pipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
        if (pipe[0] <= STDERR_FILENO || pipe[1] <= STDERR_FILENO) {
            close(pipe[0]);
            close(pipe[1]);
            return;
        }
        fcntl(pipe[1], F_SETPIPE_SZ, 4096);
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) == 0) {
            if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
                posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO) == 0 &&
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1) == 0 &&
                posix_spawn(&pid_, program, &actions, nullptr, args, env) == 0) {
                fd_ = pipe[0];
            }
            posix_spawn_file_actions_destroy(&actions);
        }
        close(pipe[1]);
        if (fd_ < 0) close(pipe[0]);
    }

    void Stop() {
        if (fd_ < 0) return;
        close(fd_);
        pid_t exited;
        do {
            exited = waitpid(pid_, nullptr, WNOHANG);
        } while (exited < 0 && errno == EINTR);
        if (exited == 0) {
            // The desktop user owns this subprocess. Don't wait for a cooperative shutdown.
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
        fd_ = -1;
    }

    int fd() const { return fd_; }

private:
    pid_t pid_{};
    int fd_{-1};
};

class PulseMonitor {
public:
    int Run() {
        if (!context_) return 1;
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] != '/') return 1;
        const auto socket = std::string("unix:") + runtime + "/pulse/native";
        if (pa_context_connect(context_.get(), socket.c_str(), PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) return 1;
        while (pa_context_get_state(context_.get()) != PA_CONTEXT_READY) {
            if (!Iterate()) return 1;
        }
        pa_context_set_subscribe_callback(context_.get(),
            [](pa_context*, pa_subscription_event_type_t, std::uint32_t, void* userdata) {
                static_cast<PulseMonitor*>(userdata)->refresh_ = true;
            }, this);
        constexpr auto mask = static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK);
        if (!Wait(pa_context_subscribe(context_.get(), mask, nullptr, nullptr))) return 1;

        while (!finished_) {
            while (refresh_) {
                if (!RefreshSource()) return 1;
            }
            if (!Iterate() || (stream_ && !PA_STREAM_IS_GOOD(pa_stream_get_state(stream_.get())))) return 1;
        }
        return 0;
    }

private:
    bool Iterate() const {
        return pa_mainloop_iterate(loop_.get(), 1, nullptr) >= 0 && PA_CONTEXT_IS_GOOD(pa_context_get_state(context_.get()));
    }

    bool Wait(pa_operation* operation) const {
        if (!operation) return false;
        const std::unique_ptr<pa_operation, decltype(&pa_operation_unref)> owned(operation, pa_operation_unref);
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
            if (finished_ || !Iterate()) {
                pa_operation_cancel(operation);
                return false;
            }
        }
        return pa_operation_get_state(operation) == PA_OPERATION_DONE;
    }

    bool RefreshSource() {
        refresh_ = false;
        default_sink_.clear();
        if (!Wait(pa_context_get_server_info(context_.get(), [](pa_context*, const pa_server_info* info, void* userdata) {
                if (info && info->default_sink_name) static_cast<PulseMonitor*>(userdata)->default_sink_ = info->default_sink_name;
            }, this))) return false;

        std::string monitor;
        if (!default_sink_.empty() &&
            !Wait(pa_context_get_sink_info_by_name(context_.get(), default_sink_.c_str(),
                [](pa_context*, const pa_sink_info* sink, int end, void* userdata) {
                    if (!end && sink && sink->monitor_source_name) {
                        *static_cast<std::string*>(userdata) = sink->monitor_source_name;
                    }
                }, &monitor))) return false;

        if (monitor == source_) return true;
        if (stream_) pa_stream_disconnect(stream_.get());
        stream_.reset();
        accumulated_ = 0;
        source_ = std::move(monitor);
        if (source_.empty()) return true;
        stream_.reset(pa_stream_new(context_.get(), "P2P Remote system audio", &kSampleSpec, nullptr));
        if (!stream_) return false;
        pa_stream_set_read_callback(stream_.get(), [](pa_stream* stream, size_t, void* userdata) {
            static_cast<PulseMonitor*>(userdata)->Read(stream);
        }, this);
        constexpr std::uint32_t unspecified = static_cast<std::uint32_t>(-1);
        constexpr pa_buffer_attr attributes{kFrameBytes * 4, unspecified, unspecified, unspecified, kFrameBytes};
        constexpr auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
        return pa_stream_connect_record(stream_.get(), source_.c_str(), &attributes, flags) == 0;
    }

    void Read(pa_stream* stream) {
        const char* device = pa_stream_get_device_name(stream);
        const bool capture = !refresh_ && device && source_ == device;
        if (!capture) accumulated_ = 0;
        while (!finished_) {
            const void* data{};
            std::size_t bytes{};
            if (pa_stream_peek(stream, &data, &bytes) < 0 || bytes == 0) break;
            if (capture) {
                // Discard stale samples if the server delivers a backlog after suspension.
                const auto count = std::min(bytes, kFrameBytes * 4);
                if (count != bytes) accumulated_ = 0;
                const auto* source = data ? static_cast<const char*>(data) + bytes - count : nullptr;
                WritePcm(source, count);
            }
            if (pa_stream_drop(stream) < 0) break;
        }
    }

    void WritePcm(const char* source, std::size_t count) {
        for (std::size_t offset = 0; offset < count && !finished_;) {
            const auto take = std::min(kFrameBytes - accumulated_, count - offset);
            if (source) std::memcpy(frame_.data() + accumulated_, source + offset, take);
            else std::memset(frame_.data() + accumulated_, 0, take);
            accumulated_ += take;
            offset += take;
            if (accumulated_ == kFrameBytes) {
                const ssize_t written = write(STDOUT_FILENO, frame_.data(), frame_.size());
                if (written < 0 && errno != EAGAIN && errno != EINTR) finished_ = true;
                accumulated_ = 0;
            }
        }
    }

    std::unique_ptr<pa_mainloop, decltype(&pa_mainloop_free)> loop_{pa_mainloop_new(), pa_mainloop_free};
    std::unique_ptr<pa_context, decltype(&pa_context_unref)> context_{
        loop_ ? pa_context_new(pa_mainloop_get_api(loop_.get()), "P2P Remote") : nullptr, pa_context_unref};
    std::unique_ptr<pa_stream, decltype(&pa_stream_unref)> stream_{nullptr, pa_stream_unref};
    std::string default_sink_;
    std::string source_;
    PcmFrame frame_{};
    std::size_t accumulated_{};
    bool refresh_{true};
    bool finished_{};
};

class PulseDesktopAudio final : public AudioBackend {
public:
    void Capture(std::stop_token stop, const ActiveTarget& state, const Send& send) override;
};

void PulseDesktopAudio::Capture(std::stop_token stop, const ActiveTarget& state, const Send& send) {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error).string();
    AudioProcess process;
    std::optional<DesktopAudioState> current_state;
    std::optional<DesktopTarget> active;
    std::chrono::steady_clock::time_point check_at{};
    std::chrono::steady_clock::time_point retry_at{};
    std::array<char, kFrameBytes> frame{};
    while (!stop.stop_requested()) {
        const auto wanted = state();
        const auto now = std::chrono::steady_clock::now();
        if (wanted != current_state || now >= check_at) {
            active = wanted ? ActiveUserDesktop() : std::nullopt;
            check_at = now + 200ms;
        }
        const bool accessible = wanted && active && active->valid() && wanted->target.SameLogin(*active);
        if (wanted != current_state || !accessible) {
            process.Stop();
            current_state = wanted;
            retry_at = {};
        }
        if (process.fd() < 0) {
            if (accessible && now >= retry_at) {
                retry_at = now + 1s;
                process.Start(wanted->target, executable);
            }
            if (process.fd() < 0) {
                std::this_thread::sleep_for(50ms);
                continue;
            }
        }
        pollfd descriptor{process.fd(), POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (!ready) continue;
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            process.Stop();
            retry_at = std::chrono::steady_clock::now() + 1s;
            continue;
        }
        // Each process writes one whole PCM frame atomically to its nonblocking pipe.
        if (read(process.fd(), frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()) && state() == wanted) {
            send(*wanted, frame.data(), frame.size());
        }
    }
}

} // namespace

std::unique_ptr<AudioBackend> CreateDesktopAudio() {
    return std::make_unique<PulseDesktopAudio>();
}

int RunDesktopAudioCapture() {
    const pid_t parent = getppid();
    if (geteuid() == 0 || parent == 1 || prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) return 1;
    return PulseMonitor{}.Run();
}

} // namespace p2premote::linux_host
