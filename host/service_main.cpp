#include "service_protocol.h"
#include "service_storage.h"

#include <p2premote/core_api.h>

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace p2premote::service;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

struct HostConfig {
    std::wstring server;
    std::wstring user;
    std::wstring password;
    int monitor_index{};
    bool compatible_http{};
    bool relay_only{};

    ~HostConfig() {
        if (!password.empty()) {
            SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        }
    }
};

std::wstring ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!len || len >= path.size()) return {};
    path.resize(len);
    return path;
}

bool IsManagedRuntime(const std::filesystem::path& directory) {
    const auto root = ServiceDirectory();
    return !root.empty() && directory.parent_path() == root
        && directory.filename().wstring().starts_with(L"runtime-")
        && IsProtectedDirectory(root) && IsProtectedDirectory(directory);
}

std::optional<HostConfig> ParseHostConfig(const std::string& json_utf8) {
    try {
        const auto obj = JsonObject::Parse(winrt::to_hstring(json_utf8));
        HostConfig cfg;
        cfg.server = obj.GetNamedString(L"server", L"").c_str();
        cfg.user = obj.GetNamedString(L"user", L"").c_str();
        cfg.password = obj.GetNamedString(L"password", L"").c_str();
        cfg.monitor_index = static_cast<int>(obj.GetNamedNumber(L"monitor_index", 0));
        cfg.compatible_http = obj.GetNamedBoolean(L"compatible_http", false);
        cfg.relay_only = obj.GetNamedBoolean(L"relay_only", false);
        if (cfg.server.empty() || cfg.user.empty() || cfg.password.empty()) return std::nullopt;
        return cfg;
    } catch (...) {
        return std::nullopt;
    }
}

struct MonitorInfoEntry {
    HMONITOR handle{};
    bool primary{};
};

BOOL CALLBACK EnumMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    auto* list = reinterpret_cast<std::vector<MonitorInfoEntry>*>(context);
    list->push_back({monitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

HMONITOR SelectMonitor(int index) {
    std::vector<MonitorInfoEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>(&monitors));
    std::stable_sort(monitors.begin(), monitors.end(), [](const auto& a, const auto& b) {
        return a.primary && !b.primary;
    });
    if (monitors.empty()) {
        return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    if (index >= 0 && static_cast<std::size_t>(index) < monitors.size()) {
        return monitors[static_cast<std::size_t>(index)].handle;
    }
    return monitors.front().handle;
}

// ============================================================================
// Mode 1: --host-worker (runs as SYSTEM inside active console Session N)
// ============================================================================

struct WorkerSharedStatus {
    std::mutex mutex;
    int state{P2P_IDLE};
    bool desktop_control{};
    int monitor_index{};
    std::wstring user;
    std::wstring device_id;
    std::wstring status{L"服务已启动，正在初始化…"};
};

std::string FormatStatusJson(WorkerSharedStatus& shared) {
    const std::lock_guard lock(shared.mutex);
    JsonObject obj;
    obj.SetNamedValue(L"running", JsonValue::CreateBooleanValue(true));
    obj.SetNamedValue(L"state", JsonValue::CreateNumberValue(shared.state));
    obj.SetNamedValue(L"desktop_control", JsonValue::CreateBooleanValue(shared.desktop_control));
    obj.SetNamedValue(L"monitor_index", JsonValue::CreateNumberValue(shared.monitor_index));
    obj.SetNamedValue(L"user", JsonValue::CreateStringValue(shared.user));
    obj.SetNamedValue(L"device_id", JsonValue::CreateStringValue(shared.device_id));
    obj.SetNamedValue(L"status", JsonValue::CreateStringValue(shared.status));
    return winrt::to_string(obj.Stringify());
}

void SetWorkerStatus(WorkerSharedStatus& shared, int state, const std::wstring& status,
    bool desktop_control = false, std::optional<std::wstring> device_id = std::nullopt,
    std::optional<std::wstring> user = std::nullopt, std::optional<int> monitor_index = std::nullopt) {
    const std::lock_guard lock(shared.mutex);
    shared.state = state;
    shared.desktop_control = desktop_control;
    shared.status = status;
    if (device_id) shared.device_id = *device_id;
    if (user) shared.user = *user;
    if (monitor_index) shared.monitor_index = *monitor_index;
}

bool WaitLoginComplete(p2p_account* account, HANDLE stop_event, std::wstring& error) {
    HANDLE handles[]{stop_event, p2p_account_event(account)};
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, 200);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) return false;
        const auto rc = p2p_account_complete(account);
        if (rc == P2P_PENDING) continue;
        if (rc == P2P_OK) return true;
        error = p2p_last_error() ? p2p_last_error() : L"账号登录失败";
        return false;
    }
}

int RunHostWorker(const std::filesystem::path& directory, const std::wstring& stop_name) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    WorkerSharedStatus shared;

    if (stop_name.empty()) return 1;
    const winrt::handle stop{OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, stop_name.c_str())};
    if (!stop) return 1;
    const auto stop_event = stop.get();

    // The status pipe does not accept service-management commands.
    std::jthread pipe_thread([&](std::stop_token token) {
        const std::stop_callback shutdown(token, [stop_event] { SetEvent(stop_event); });
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        PSECURITY_DESCRIPTOR pipe_sd{};
        // Interactive clients may read and acknowledge status. They cannot create a server instance.
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100003;;;IU)", SDDL_REVISION_1, &pipe_sd, nullptr)) return;
        const LocalAllocation free_sd(pipe_sd, LocalFree);
        SECURITY_ATTRIBUTES pipe_sa{sizeof(pipe_sa), pipe_sd, FALSE};
        const winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event) return;
        winrt::handle pipe;
        while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
            if (!pipe) {
                const HANDLE opened = CreateNamedPipeW(kPipeName,
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1, 4096, 4096, 1000, &pipe_sa);
                if (opened == INVALID_HANDLE_VALUE) {
                    WaitForSingleObject(stop_event, 200);
                    continue;
                }
                pipe.attach(opened);
            }
            ResetEvent(event.get());
            OVERLAPPED connect{};
            connect.hEvent = event.get();
            bool connected = ConnectNamedPipe(pipe.get(), &connect) != FALSE;
            if (!connected) {
                const DWORD error = GetLastError();
                DWORD ignored{};
                connected = error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING
                    && CompletePipeIo(pipe.get(), connect, stop_event, INFINITE, ignored));
            }
            if (!connected) {
                pipe.close();
                continue;
            }
            auto reply = FormatStatusJson(shared) + "\n";
            const auto deadline = GetTickCount64() + 750;
            DWORD size{};
            if (TransferPipeBytes(pipe.get(), event.get(), true, reply.data(),
                    static_cast<DWORD>(reply.size()), size, deadline, stop_event) && size == reply.size()) {
                // Disconnect discards unread data. Wait for the client to consume the response.
                char received{};
                TransferPipeBytes(pipe.get(), event.get(), false, &received, sizeof(received), size, deadline, stop_event);
            }
            DisconnectNamedPipe(pipe.get());
        }
    });

    const auto sleep_interruptible = [&](DWORD ms) {
        WaitForSingleObject(stop_event, ms);
    };

    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        auto raw = ReadEncryptedConfig(directory / L"host.dat");
        if (!raw) {
            SetWorkerStatus(shared, P2P_IDLE, L"服务运行中 · 等待绑定账号", false, L"", L"");
            sleep_interruptible(2000);
            continue;
        }
        auto cfg = ParseHostConfig(*raw);
        SecureZeroMemory(raw->data(), raw->size());
        if (!cfg) {
            SetWorkerStatus(shared, P2P_IDLE, L"服务配置无效，请在客户端重新启用服务", false, L"", L"");
            sleep_interruptible(3000);
            continue;
        }

        SetWorkerStatus(shared, P2P_WAITING, L"系统服务正在登录账号…", false, L"", cfg->user, cfg->monitor_index);
        std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account(p2p_account_create(), p2p_account_destroy);
        if (!account) {
            SetWorkerStatus(shared, P2P_STOPPED, L"核心账号模块初始化失败");
            sleep_interruptible(3000);
            continue;
        }

        if (p2p_account_login(account.get(), cfg->server.c_str(), cfg->user.c_str(), cfg->password.c_str(),
                0, cfg->compatible_http ? 1 : 0, cfg->relay_only ? 1 : 0) != P2P_OK) {
            SetWorkerStatus(shared, P2P_STOPPED, p2p_last_error() ? p2p_last_error() : L"账号登录请求失败");
            sleep_interruptible(5000);
            continue;
        }

        std::wstring login_err;
        if (!WaitLoginComplete(account.get(), stop_event, login_err)) {
            if (!login_err.empty()) {
                SetWorkerStatus(shared, P2P_STOPPED, L"系统服务登录失败：" + login_err);
                sleep_interruptible(5000);
            }
            continue;
        }

        const std::wstring dev_id = p2p_account_device_id(account.get()) ? p2p_account_device_id(account.get()) : L"";
        SetWorkerStatus(shared, P2P_WAITING, L"系统服务已就绪 · 等待远程连接 (SYSTEM)", false, dev_id, cfg->user, cfg->monitor_index);

        while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
            const p2p_connection_options_v1 settings{L"", L"", "", 0, 1};
            std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream(
                p2p_stream_create(account.get(), 1, nullptr, nullptr, &settings), p2p_stream_destroy);
            if (!stream) {
                SetWorkerStatus(shared, P2P_STOPPED, p2p_last_error() ? p2p_last_error() : L"无法创建主机接收会话");
                sleep_interruptible(3000);
                break;
            }
            HMONITOR monitor = SelectMonitor(cfg->monitor_index);
            if (p2p_stream_start(stream.get(), monitor, 60) != P2P_OK) {
                SetWorkerStatus(shared, P2P_STOPPED, p2p_last_error() ? p2p_last_error() : L"无法启动主机接收");
                sleep_interruptible(3000);
                break;
            }

            bool input_ready = false;
            int last_state = -1;
            std::wstring last_status;
            ULONGLONG next_tick = GetTickCount64() + 500;
            ULONGLONG next_cursor = GetTickCount64() + 50;

            auto publish = [&]() -> bool {
                p2p_stream_snapshot_v1 snap{};
                if (p2p_stream_snapshot(stream.get(), &snap) != P2P_OK) return false;
                input_ready = snap.input_ready != 0;
                const std::wstring st = snap.status ? std::wstring(snap.status) : L"";
                if (snap.state != last_state || st != last_status) {
                    last_state = snap.state;
                    last_status = st;
                    SetWorkerStatus(shared, snap.state, L"[系统服务] " + st, snap.desktop_control != 0);
                }
                return snap.state != P2P_STOPPED;
            };

            if (!publish()) {
                sleep_interruptible(2000);
                continue;
            }

            while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
                HANDLE wait_handles[3]{stop_event};
                uint32_t kinds[3]{};
                DWORD count = 1;
                if (const auto event = p2p_stream_wait_handle(stream.get(), P2P_WAIT_SESSION)) {
                    wait_handles[count] = event;
                    kinds[count++] = P2P_WAIT_SESSION;
                }
                if (const auto capture = p2p_stream_wait_handle(stream.get(), P2P_WAIT_CAPTURE)) {
                    wait_handles[count] = capture;
                    kinds[count++] = P2P_WAIT_CAPTURE;
                }

                auto now = GetTickCount64();
                ULONGLONG due = next_tick;
                if (input_ready) due = std::min(due, next_cursor);
                const DWORD timeout = due > now ? static_cast<DWORD>(std::min<ULONGLONG>(due - now, 200)) : 0;

                const DWORD waited = MsgWaitForMultipleObjectsEx(count, wait_handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (waited == WAIT_OBJECT_0) {
                    p2p_stream_stop(stream.get(), L"系统服务已停止");
                    break;
                }
                if (waited == WAIT_FAILED) break;
                if (waited >= WAIT_OBJECT_0 + 1 && waited < WAIT_OBJECT_0 + count) {
                    const auto kind = kinds[waited - WAIT_OBJECT_0];
                    if (p2p_stream_dispatch(stream.get(), kind) != P2P_OK) break;
                }

                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                now = GetTickCount64();
                if (now >= next_tick) {
                    next_tick = now + 500;
                    if (p2p_stream_tick(stream.get()) != P2P_OK) break;
                }
                if (input_ready && now >= next_cursor) {
                    next_cursor = now + 50;
                    if (p2p_stream_poll_cursor(stream.get()) != P2P_OK) break;
                }

                if (!publish()) break;
            }

            if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
            // Stream ended (e.g. viewer disconnected or transient drop); re-listen shortly.
            sleep_interruptible(1500);
        }
    }

    if (pipe_thread.joinable()) pipe_thread.join();
    return 0;
}

// ============================================================================
// Mode 2: --service (Session 0 SCM LocalSystem supervisor)
// ============================================================================

SERVICE_STATUS_HANDLE g_status_handle{};
SERVICE_STATUS g_service_status{};
HANDLE g_svc_stop_event{};
HANDLE g_svc_session_event{};

void ReportServiceState(DWORD state, DWORD win32_exit = NO_ERROR, DWORD wait_hint = 0) {
    static DWORD checkpoint = 1;
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32_exit;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE)
        : 0;
    g_service_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_status_handle, &g_service_status);
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD event_type, LPVOID, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportServiceState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_svc_stop_event) SetEvent(g_svc_stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (event_type == WTS_CONSOLE_CONNECT || event_type == WTS_CONSOLE_DISCONNECT ||
            event_type == WTS_SESSION_LOGON || event_type == WTS_SESSION_LOGOFF ||
            event_type == WTS_SESSION_LOCK || event_type == WTS_SESSION_UNLOCK) {
            if (g_svc_session_event) SetEvent(g_svc_session_event);
        }
        return NO_ERROR;
    default:
        return NO_ERROR;
    }
}

bool SpawnWorkerInSession(DWORD session_id, HANDLE& out_process, HANDLE& out_job, winrt::handle& out_stop) {
    out_process = nullptr;
    out_job = nullptr;
    const auto suffix = UniqueSuffix();
    if (suffix.empty()) return false;
    const auto stop_name = L"Global\\P2PRemoteHostStop-" + suffix;
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) return false;
    const LocalAllocation free_descriptor(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    winrt::handle stop{CreateEventW(&attributes, TRUE, FALSE, stop_name.c_str())};
    if (!stop || GetLastError() == ERROR_ALREADY_EXISTS) return false;

    HANDLE own_token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &own_token)) return false;

    HANDLE dup_token{};
    const BOOL dup_ok = DuplicateTokenEx(own_token, MAXIMUM_ALLOWED, nullptr,
        SecurityIdentification, TokenPrimary, &dup_token);
    CloseHandle(own_token);
    if (!dup_ok) return false;

    if (!SetTokenInformation(dup_token, TokenSessionId, &session_id, sizeof(session_id))) {
        CloseHandle(dup_token);
        return false;
    }

    // Best-effort UIAccess flag so the worker has full UI privilege even across restricted apps.
    DWORD ui_access = 1;
    SetTokenInformation(dup_token, TokenUIAccess, &ui_access, sizeof(ui_access));

    LPVOID env{};
    CreateEnvironmentBlock(&env, dup_token, FALSE);

    const std::wstring exe = ExecutablePath();
    std::wstring cmdline = L"\"" + exe + L"\" --host-worker --stop-event " + stop_name;
    const std::wstring workdir = std::filesystem::path(exe).parent_path().wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessAsUserW(
        dup_token,
        exe.c_str(),
        cmdline.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        env,
        workdir.c_str(),
        &si,
        &pi);

    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(dup_token);
    if (!created) return false;

    winrt::handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jinfo{};
    jinfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const winrt::handle thread{pi.hThread};
    winrt::handle process{pi.hProcess};
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jinfo, sizeof(jinfo))
            || !AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
        TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
        return false;
    }

    out_process = process.detach();
    out_job = job.detach();
    out_stop = std::move(stop);
    return true;
}

void StopWorkerProcess(HANDLE& process, HANDLE& job, winrt::handle& stop) {
    if (stop) SetEvent(stop.get());
    if (process) {
        if (WaitForSingleObject(process, 3000) == WAIT_TIMEOUT) {
            if (TerminateProcess(process, 0)) WaitForSingleObject(process, 1000);
        }
        CloseHandle(process);
        process = nullptr;
    }
    if (job) {
        CloseHandle(job);
        job = nullptr;
    }
    stop.close();
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
    if (!g_status_handle) return;

    ReportServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);
    g_svc_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_svc_session_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_svc_stop_event || !g_svc_session_event) {
        if (g_svc_stop_event) CloseHandle(g_svc_stop_event);
        if (g_svc_session_event) CloseHandle(g_svc_session_event);
        ReportServiceState(SERVICE_STOPPED, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    ReportServiceState(SERVICE_RUNNING);

    HANDLE worker_proc{};
    HANDLE worker_job{};
    winrt::handle worker_stop;
    DWORD worker_session = 0xFFFFFFFF;

    while (WaitForSingleObject(g_svc_stop_event, 0) != WAIT_OBJECT_0) {
        const DWORD active_session = WTSGetActiveConsoleSessionId();
        if (active_session == 0xFFFFFFFF) {
            if (worker_proc) {
                StopWorkerProcess(worker_proc, worker_job, worker_stop);
                worker_session = 0xFFFFFFFF;
            }
            HANDLE waits[2] = {g_svc_stop_event, g_svc_session_event};
            WaitForMultipleObjects(2, waits, FALSE, 1000);
            continue;
        }

        if (worker_proc && worker_session != active_session) {
            StopWorkerProcess(worker_proc, worker_job, worker_stop);
            worker_session = 0xFFFFFFFF;
        }

        if (!worker_proc) {
            if (SpawnWorkerInSession(active_session, worker_proc, worker_job, worker_stop)) {
                worker_session = active_session;
            } else {
                HANDLE waits[2] = {g_svc_stop_event, g_svc_session_event};
                WaitForMultipleObjects(2, waits, FALSE, 2000);
                continue;
            }
        }

        HANDLE waits[3] = {g_svc_stop_event, g_svc_session_event, worker_proc};
        const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 2) {
            StopWorkerProcess(worker_proc, worker_job, worker_stop);
            worker_session = 0xFFFFFFFF;
            WaitForSingleObject(g_svc_stop_event, 2000);
        }
    }

    StopWorkerProcess(worker_proc, worker_job, worker_stop);
    CloseHandle(g_svc_session_event);
    CloseHandle(g_svc_stop_event);
    ReportServiceState(SERVICE_STOPPED);
}

// ============================================================================
// Mode 3 & 4: --install / --uninstall (elevated management)
// ============================================================================

constexpr const wchar_t* runtime_files[]{L"p2premote_service.exe", L"p2premote_core.dll", L"p2premote_minirtc.dll"};
constexpr const wchar_t* service_plugins[]{L"clipboard-sync", L"shortcut-macros", L"keyboard-sync", L"adaptive-quality"};

bool RemoveRuntime(const std::filesystem::path& directory) {
    if (!IsManagedRuntime(directory)) return false;
    if (!DeleteFileW((directory / L"host.dat").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    const auto plugins = directory / L"plugins";
    for (const auto* id : service_plugins) {
        const auto plugin = plugins / id;
        DeleteFileW((plugin / L"main.lua").c_str());
        DeleteFileW((plugin / L"plugin.json").c_str());
        RemoveDirectoryW(plugin.c_str());
    }
    RemoveDirectoryW(plugins.c_str());
    for (const auto* name : runtime_files) DeleteFileW((directory / name).c_str());
    return RemoveDirectoryW(directory.c_str()) != FALSE;
}

bool CopyPackageFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    const HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (input == INVALID_HANDLE_VALUE) return false;
    const winrt::handle from{input};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(from.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))
            || (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;
    // Create the destination ourselves so it inherits the protected directory's ACL.
    const HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    const winrt::handle to{output};
    char buffer[64 * 1024];
    DWORD read{}, written{};
    while (ReadFile(from.get(), buffer, sizeof(buffer), &read, nullptr)) {
        if (!read) return FlushFileBuffers(to.get()) != FALSE;
        if (!WriteFile(to.get(), buffer, read, &written, nullptr) || written != read) break;
    }
    return false;
}

bool DeployRuntime(const std::filesystem::path& directory, const std::string& config) {
    const auto source = std::filesystem::path(ExecutablePath()).parent_path();
    if (source.empty() || !EnsureProtectedDirectory(directory)) return false;
    for (const auto* name : runtime_files) {
        if (!CopyPackageFile(source / name, directory / name)) return false;
    }
    const auto plugins = directory / L"plugins";
    if (!CreateDirectoryW(plugins.c_str(), nullptr)) return false;
    for (const auto* id : service_plugins) {
        const auto plugin = plugins / id;
        if (!CreateDirectoryW(plugin.c_str(), nullptr)) return false;
        for (const auto* name : {L"main.lua", L"plugin.json"}) {
            if (!CopyPackageFile(source / L"plugins" / id / name, plugin / name)) return false;
        }
    }
    return WriteEncryptedConfig(directory / L"host.dat", config, false);
}

using ServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, decltype(&CloseServiceHandle)>;

DWORD ServiceState(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes{};
    return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
        sizeof(status), &bytes) ? status.dwCurrentState : 0;
}

bool WaitForServiceState(SC_HANDLE service, DWORD desired) {
    const auto deadline = GetTickCount64() + 8'000;
    do {
        const auto state = ServiceState(service);
        if (state == desired) return true;
        if (!state || (desired == SERVICE_RUNNING && state == SERVICE_STOPPED)) return false;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    return false;
}

bool StopRegisteredService(SC_HANDLE service) {
    const auto state = ServiceState(service);
    if (state == SERVICE_STOPPED) return true;
    SERVICE_STATUS status{};
    if (state != SERVICE_STOP_PENDING && !ControlService(service, SERVICE_CONTROL_STOP, &status)
            && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return false;
    return WaitForServiceState(service, SERVICE_STOPPED);
}

std::filesystem::path RegisteredRuntime(SC_HANDLE service) {
    DWORD needed{};
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (!needed || needed > 64 * 1024) return {};
    std::vector<BYTE> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, needed, &needed)) return {};
    int count{};
    LPWSTR* arguments = CommandLineToArgvW(config->lpBinaryPathName, &count);
    const LocalAllocation free_arguments(arguments, LocalFree);
    if (!arguments || count != 2 || std::wstring_view(arguments[1]) != L"--service") return {};
    return std::filesystem::path(arguments[0]).parent_path();
}

int InstallAndStartService(const std::wstring& staging_path) {
    if (staging_path.empty()) return 2;
    auto config = ReadEncryptedConfig(staging_path);
    if (!config || config->empty()) return 2;
    struct ClearPlaintext {
        std::string& value;
        ~ClearPlaintext() { SecureZeroMemory(value.data(), value.size()); }
    } clear{*config};
    if (!ParseHostConfig(*config)) return 2;

    const auto root = ServiceDirectory();
    if (root.empty() || !EnsureProtectedDirectory(root)) return 3;
    const HANDLE lock = CreateFileW((root / L"install.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return 3;
    const winrt::handle install_lock{lock};
    const ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE),
        CloseServiceHandle};
    if (!manager) return 4;
    ServiceHandle service{OpenServiceW(manager.get(), kServiceName, SERVICE_ALL_ACCESS), CloseServiceHandle};
    if (!service && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) return 4;

    const auto suffix = UniqueSuffix();
    if (suffix.empty()) return 3;
    const auto directory = root / (L"runtime-" + suffix);
    if (!DeployRuntime(directory, *config)) {
        RemoveRuntime(directory);
        return 3;
    }
    const auto command = L"\"" + (directory / runtime_files[0]).wstring() + L"\" --service";
    std::filesystem::path previous;
    if (service) {
        previous = RegisteredRuntime(service.get());
        if (!StopRegisteredService(service.get()) ||
                !ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                    SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                    L"LocalSystem", nullptr, kServiceDisplayName)) {
            RemoveRuntime(directory);
            return 5;
        }
    } else {
        service.reset(CreateServiceW(manager.get(), kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(),
            nullptr, nullptr, nullptr, L"LocalSystem", nullptr));
        if (!service) {
            RemoveRuntime(directory);
            return 5;
        }
    }
    if (!previous.empty() && previous != directory) RemoveRuntime(previous);

    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(kServiceDescription)};
    SC_ACTION actions[]{{SC_ACTION_RESTART, 3000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}};
    SERVICE_FAILURE_ACTIONSW recovery{60, nullptr, nullptr, static_cast<DWORD>(std::size(actions)), actions};
    if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description)
            || !ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &recovery)) return 6;
    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return 7;
    if (!WaitForServiceState(service.get(), SERVICE_RUNNING)) return 7;
    if (!ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) return 8;
    return 0;
}

int UninstallService() {
    const ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT), CloseServiceHandle};
    if (!manager) return 4;
    const ServiceHandle service{OpenServiceW(manager.get(), kServiceName,
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | DELETE), CloseServiceHandle};
    if (!service) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : 4;
    const auto runtime = RegisteredRuntime(service.get());
    if (!StopRegisteredService(service.get()) || !DeleteService(service.get())) return 9;
    if (IsManagedRuntime(runtime) && !RemoveRuntime(runtime)) return 10;
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc{};
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::wstring config_file;
    std::wstring stop_name;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--service" || arg == L"--host-worker" || arg == L"--install" || arg == L"--uninstall") {
            mode = arg;
        } else if (arg == L"--config-file" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == L"--stop-event" && i + 1 < argc) {
            stop_name = argv[++i];
        }
    }
    if (argv) LocalFree(argv);

    if (mode == L"--service" || mode == L"--host-worker") {
        const auto directory = std::filesystem::path(ExecutablePath()).parent_path();
        if (!IsManagedRuntime(directory)
                || !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) return 1;
        if (mode == L"--host-worker") return RunHostWorker(directory, stop_name);
    }
    if (mode == L"--service") {
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(kServiceName), ServiceMain},
            {nullptr, nullptr}
        };
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }
    if (mode == L"--install") {
        return InstallAndStartService(config_file);
    }
    if (mode == L"--uninstall") {
        return UninstallService();
    }
    return 0;
}
