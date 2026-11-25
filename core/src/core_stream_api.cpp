#include "core_api_internal.h"
#include "direct_signal.h"
#include "plugin_settings.h"
#include "viewer_input.h"
#include "qpc.h"

#include <commctrl.h>
#include <objbase.h>
#include <deque>
#include <memory>
#include <winrt/Windows.Foundation.Collections.h>

#include <p2premote/plugin_protocol.h>

using namespace p2premote;
using namespace p2premote::abi;

struct p2p_stream {
    p2p_stream(bool host, ConnectionOptions connection, HWND parent)
        : host(host), diagnostics(connection.diagnostics && !host), poll_cursor(host || !connection.video_cursor),
          session(host, std::move(connection)), input(session) {
        if (host || diagnostics) return;
        if (!parent || GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId()) {
            throw std::invalid_argument("视频窗口必须由当前线程创建");
        }
        move_timer.attach(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE));
        winrt::check_pointer(move_timer.get());
        surface = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        winrt::check_pointer(surface);
        if (!SetWindowSubclass(surface, SurfaceProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            DestroyWindow(surface);
            throw std::runtime_error("无法初始化视频窗口");
        }
        input.Attach(surface);
        Resize(true);
    }

    ~p2p_stream() {
        input.Attach(nullptr);
        Guard([this] { session.Stop(L"已关闭"); });
        if (surface) {
            RemoveWindowSubclass(surface, SurfaceProc, 1);
            DestroyWindow(surface);
        }
    }

    void Resize(bool drawable) {
        if (!surface) return;
        RECT client{};
        GetClientRect(GetParent(surface), &client);
        if ((!drawable || client.right <= 0 || client.bottom <= 0) && GetFocus() == surface) {
            SetFocus(GetAncestor(surface, GA_ROOT));
        }
        SetWindowPos(surface, nullptr, 0, 0, client.right, client.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
        session.Resize(drawable && client.right > 0 && client.bottom > 0);
    }

    void SyncState() {
        if (host) return;
        input.RefreshReady();
        input.RefreshLocalCursor();
        const auto state = session.State();
        if (state != surface_state) {
            if (state == StreamState::waiting || state == StreamState::reconnecting) input.Reset();
            surface_state = state;
            if (surface) InvalidateRect(surface, nullptr, FALSE);
        }
    }

    void SyncMoveTimer() {
        if (!move_timer) return;
        const auto delay = input.PendingMoveTimeoutMs();
        if (delay == INFINITE) {
            if (move_timer_armed) {
                CancelWaitableTimer(move_timer.get());
                move_timer_armed = false;
            }
            return;
        }
        const auto deadline = QpcNow() + QpcFrequency() * delay / 1000;
        if (move_timer_armed && move_timer_deadline <= deadline) return;
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(delay) * 10'000 - 1;
        winrt::check_bool(SetWaitableTimer(move_timer.get(), &due, 0, nullptr, nullptr, FALSE));
        move_timer_armed = true;
        move_timer_deadline = deadline;
    }

    void PresentReadyFrame() {
        if (host || pumping_video) return;
        if (const auto present = session.PresentEvent();
            present && WaitForSingleObject(present, 0) == WAIT_OBJECT_0) {
            pumping_video = true;
            try {
                session.Present();
            } catch (...) {
                pumping_video = false;
                throw;
            }
            pumping_video = false;
        }
    }

    static LRESULT CALLBACK SurfaceProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR, DWORD_PTR context) {
        auto& self = *reinterpret_cast<p2p_stream*>(context);
        if (message == WM_NCDESTROY) {
            self.surface = nullptr;
            self.input.Attach(nullptr);
            RemoveWindowSubclass(window, SurfaceProc, 1);
        } else if (message == WM_NCHITTEST) {
            // STATIC normally lets mouse input pass through to the Qt parent.
            return DefWindowProcW(window, message, wparam, lparam);
        } else if (message == WM_ERASEBKGND) {
            return 1;
        } else if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            unsigned width{}, height{};
            self.session.VideoDimensions(width, height);
            if (self.session.State() != StreamState::streaming || !width || !height) {
                FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            EndPaint(window, &paint);
            return 0;
        } else if ((message == WM_KEYDOWN || message == WM_KEYUP) && wparam == VK_F11) {
            if (message == WM_KEYDOWN && !(lparam & (1L << 30))) {
                PostMessageW(GetParent(window), P2P_VIDEO_FULLSCREEN_MESSAGE, 0, 0);
            }
            return 0;
        } else {
            try {
                if (const auto result = self.input.OnSurfaceMessage(window, message, wparam, lparam)) {
                    self.SyncMoveTimer();
                    // Keep 60 fps frames presenting even while Qt drains high-rate move/wheel bursts.
                    if (message == WM_MOUSEMOVE || message == WM_MOUSEWHEEL) self.PresentReadyFrame();
                    return *result;
                }
            } catch (...) {
                ReportException();
                Guard([&] {
                    self.session.Stop(p2p_last_error());
                    self.SyncState();
                });
                return 0;
            }
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    Apartment apartment{winrt::apartment_type::single_threaded};
    bool host;
    bool diagnostics;
    bool poll_cursor;
    StreamSession session;
    ViewerInput input;
    HWND surface{};
    StreamState surface_state{StreamState::idle};
    winrt::handle move_timer;
    bool move_timer_armed{};
    std::int64_t move_timer_deadline{};
    bool pumping_video{};
    std::wstring status, notice, overlay;
    PluginOverlaySnapshot overlays;
    StreamStatistics stats;
    std::vector<PluginMenuItem> menu;
    std::deque<PluginUiCommand> ui_commands;
    PluginUiCommand current_ui_command;
    AgentSummary current_agent_summary;
};

p2p_stream* p2p_stream_create(const p2p_account* account, int32_t host,
    const wchar_t* remote_device, void* video_parent, const p2p_connection_options_v1* options) {
    p2p_stream* stream{};
    Guard([&] {
        auto connection = GetConnection(account);
        if (host && !p2p_account_device_id(account)) {
            throw std::invalid_argument("请先将本机添加到账号设备");
        }
        connection.diagnostics = options && options->diagnostics;
        if (!host && (!remote_device || !*remote_device || (!video_parent && !connection.diagnostics))) {
            throw std::invalid_argument("请选择目标设备并创建视频窗口");
        }
        connection.auto_control = host && (!options || options->auto_control);
        if (!host) connection.remote_device = remote_device;
        if (options) {
            if (options->ice_trace_directory && *options->ice_trace_directory) {
                connection.ice_trace_dir = std::filesystem::absolute(options->ice_trace_directory);
                std::filesystem::create_directories(connection.ice_trace_dir);
            }
            if (options->latency_trace_directory && *options->latency_trace_directory) {
                connection.latency_trace_dir = std::filesystem::absolute(options->latency_trace_directory);
                std::filesystem::create_directories(connection.latency_trace_dir);
            }
            ApplyIceOverrides(connection, options->stun_url ? options->stun_url : "", options->stun_only != 0);
        }
        stream = new p2p_stream(host != 0, std::move(connection), static_cast<HWND>(video_parent));
    });
    return stream;
}

p2p_stream* p2p_stream_create_lan(int32_t host, void* video_parent, const p2p_lan_options_v1* options) {
    p2p_stream* stream{};
    Guard([&] {
        const auto& opt = Required(options);
        if (!opt.access_code || std::wcslen(opt.access_code) < 6 || std::wcslen(opt.access_code) > 32) {
            throw std::invalid_argument("局域网被控码长度需为 6 至 32 位");
        }
        if (!host && (!opt.host_address || !*opt.host_address || !video_parent)) {
            throw std::invalid_argument("请输入目标 IP 地址并创建视频窗口");
        }
        ConnectionOptions connection;
        connection.lan_direct = true;
        connection.lan_host = opt.host_address ? opt.host_address : L"";
        connection.lan_port = opt.port ? opt.port : kLanDefaultPort;
        connection.lan_access_code = opt.access_code;
        connection.auto_control = host && (opt.auto_control != 0);
        if (opt.ice_trace_directory && *opt.ice_trace_directory) {
            connection.ice_trace_dir = std::filesystem::absolute(opt.ice_trace_directory);
            std::filesystem::create_directories(connection.ice_trace_dir);
        }
        if (opt.latency_trace_directory && *opt.latency_trace_directory) {
            connection.latency_trace_dir = std::filesystem::absolute(opt.latency_trace_directory);
            std::filesystem::create_directories(connection.latency_trace_dir);
        }
        stream = new p2p_stream(host != 0, std::move(connection), static_cast<HWND>(video_parent));
    });
    return stream;
}

struct p2p_lan_scanner {
    LanDiscoveryScanner scanner{kLanDiscoveryPort};
    std::vector<LanPeer> peers;
};

p2p_lan_scanner* p2p_lan_scanner_create(void) {
    p2p_lan_scanner* result{};
    Guard([&] { result = new p2p_lan_scanner(); });
    return result;
}

void p2p_lan_scanner_destroy(p2p_lan_scanner* scanner) { delete scanner; }

int32_t p2p_lan_scanner_probe(p2p_lan_scanner* scanner) {
    return Guard([&] { Required(scanner).scanner.Probe(); });
}

size_t p2p_lan_scanner_read(p2p_lan_scanner* scanner) {
    size_t size{};
    Guard([&] {
        auto& state = Required(scanner);
        state.peers = state.scanner.Snapshot();
        size = state.peers.size();
    });
    return size;
}

int32_t p2p_lan_scanner_peer(const p2p_lan_scanner* scanner, size_t index, p2p_lan_peer_v1* peer) {
    return Guard([&] {
        const auto& entry = Required(scanner).peers.at(index);
        Required(peer) = {entry.id.c_str(), entry.name.c_str(), entry.address.c_str(), entry.port, entry.busy ? 1 : 0};
    });
}

const wchar_t* p2p_lan_local_addresses(void) {
    thread_local std::wstring addresses;
    addresses.clear();
    Guard([&] { addresses = FormatLocalLanAddresses(); });
    return addresses.c_str();
}

void p2p_stream_destroy(p2p_stream* stream) { delete stream; }

int32_t p2p_stream_start(p2p_stream* stream, void* monitor, uint32_t fps) {
    return Guard([&] {
        auto& state = Required(stream);
        if ((state.host && !monitor) || (!state.host && !state.diagnostics && !state.surface) ||
            (fps != 30 && fps != 60)) {
            throw std::invalid_argument("无效的串流窗口、显示器或帧率");
        }
        state.session.Start(static_cast<HMONITOR>(monitor), state.surface, static_cast<StreamFrameRate>(fps));
        state.SyncState();
    });
}

int32_t p2p_stream_stop(p2p_stream* stream, const wchar_t* reason) {
    return Guard([&] {
        auto& state = Required(stream);
        state.session.Stop(reason ? reason : L"已断开");
        state.SyncState();
    });
}

int32_t p2p_stream_approve(p2p_stream* stream, int32_t desktop) {
    return Guard([&] { Required(stream).session.Approve(desktop ? StreamInputMode::desktop : StreamInputMode::view_only); });
}

int32_t p2p_stream_quality(p2p_stream* stream, uint32_t quality) {
    return Guard([&] {
        if (quality < P2P_720P || quality > P2P_1440P) throw std::invalid_argument("无效的分辨率预设");
        Required(stream).session.SetQuality(static_cast<StreamQuality>(quality));
    });
}

int32_t p2p_stream_bitrate(p2p_stream* stream, uint32_t bitrate) {
    return Guard([&] {
        if (bitrate < P2P_ECONOMY || bitrate > P2P_DETAIL) throw std::invalid_argument("无效的画质预设");
        Required(stream).session.SetVideoBitratePreset(static_cast<VideoBitratePreset>(bitrate));
    });
}

int32_t p2p_stream_set_audio(p2p_stream* stream, const p2p_audio_options_v1* options) {
    return Guard([&] {
        auto& state = Required(stream);
        const auto& opt = Required(options);
        state.session.SetAudioOptions({
            opt.speaker_enabled != 0,
            opt.mic_enabled != 0,
            opt.speaker_volume,
            opt.mic_speaker_fallback != 0
        });
    });
}

int32_t p2p_stream_resize(p2p_stream* stream, int32_t drawable) {
    return Guard([&] { Required(stream).Resize(drawable != 0); });
}

int32_t p2p_stream_display_changed(p2p_stream* stream) {
    return Guard([&] {
        auto& state = Required(stream);
        state.session.DisplayChanged();
        if (state.session.DesktopControl()) state.session.Stop(L"显示配置已改变，已停止整桌面控制");
        state.SyncState();
    });
}

int32_t p2p_stream_tick(p2p_stream* stream) {
    return Guard([&] {
        auto& state = Required(stream);
        state.session.Tick();
        if (!state.host) ViewerInput::UpdatePluginLocks(state.session);
        state.SyncState();
    });
}

int32_t p2p_stream_poll_cursor(p2p_stream* stream) {
    return Guard([&] {
        auto& state = Required(stream);
        if (state.poll_cursor && state.session.InputReady()) state.session.PollCursor();
    });
}

int32_t p2p_stream_heartbeat(p2p_stream* stream) {
    return Guard([&] {
        auto& state = Required(stream);
        if (!state.host) state.input.Heartbeat();
    });
}

int32_t p2p_stream_state(const p2p_stream* stream) {
    return stream ? static_cast<int32_t>(stream->session.State()) : P2P_STOPPED;
}

int32_t p2p_stream_input_ready(const p2p_stream* stream) {
    return stream && stream->session.InputReady();
}

void* p2p_stream_wait_handle(const p2p_stream* stream, uint32_t kind) {
    HANDLE handle{};
    Guard([&] {
        const auto& state = Required(stream);
        switch (kind) {
        case P2P_WAIT_SESSION: handle = state.session.Event(); break;
        case P2P_WAIT_CAPTURE: handle = state.session.CaptureEvent(); break;
        case P2P_WAIT_PRESENT: handle = state.session.PresentEvent(); break;
        case P2P_WAIT_INPUT: handle = state.move_timer.get(); break;
        }
    });
    return handle;
}

int32_t p2p_stream_dispatch(p2p_stream* stream, uint32_t kind) {
    return Guard([&] {
        auto& state = Required(stream);
        switch (kind) {
        case P2P_WAIT_SESSION:
            if (state.session.Event()) {
                state.session.ProcessEvents();
                state.SyncState();
                state.PresentReadyFrame();
            }
            break;
        case P2P_WAIT_CAPTURE:
            if (state.session.CaptureEvent()) state.session.Capture();
            break;
        case P2P_WAIT_PRESENT:
            if (!state.pumping_video && (state.session.PresentEvent() || state.session.CanPresent())) {
                state.pumping_video = true;
                try {
                    state.session.Present();
                } catch (...) {
                    state.pumping_video = false;
                    throw;
                }
                state.pumping_video = false;
            }
            break;
        case P2P_WAIT_INPUT:
            state.move_timer_armed = false;
            state.input.FlushPendingMove();
            state.SyncMoveTimer();
            break;
        default: throw std::invalid_argument("无效的串流事件");
        }
    });
}

int32_t p2p_stream_snapshot(p2p_stream* stream, p2p_stream_snapshot_v1* snapshot) {
    return Guard([&] {
        auto& state = Required(stream);
        state.status = state.session.Status();
        state.notice = state.session.PluginNotice();
        auto& stats = state.stats;
        stats = state.session.Statistics();
        Required(snapshot) = {
            state.status.c_str(), stats.video_codec.c_str(), state.notice.c_str(), stats.ice_protocol.c_str(),
            stats.captured, stats.submitted, stats.received, stats.decoded, stats.presented,
            stats.width, stats.height, stats.upload_bps.value_or(0), stats.download_bps.value_or(0),
            static_cast<uint32_t>(stats.video_bitrate_limit_bps.value_or(0)),
            static_cast<int32_t>(state.session.State()), static_cast<int32_t>(stats.ice_path),
            state.session.DesktopControl(), state.session.InputReady(), state.session.HasPluginMenu(),
            stats.srtp_active, stats.gpu_native, stats.gpu_conversion, stats.seconds, stats.ice_rtt_ms.value_or(-1),
            stats.video_loss_fraction.value_or(-1), stats.capture_to_submit_ms, stats.draw_to_present_ms,
            stats.packet_send_queue_ms.value_or(-1), stats.decoded_frame_wait_ms.value_or(-1)
        };
    });
}

const wchar_t* p2p_stream_overlay_text(p2p_stream* stream) {
    const wchar_t* text = L"";
    Guard([&] {
        auto& state = Required(stream);
        state.overlay = state.session.OverlayText();
        text = state.overlay.c_str();
    });
    return text;
}

size_t p2p_stream_read_overlays(p2p_stream* stream) {
    size_t size{};
    Guard([&] {
        auto& state = Required(stream);
        state.session.ReadOverlays(state.overlays);
        size = state.overlays.items.size();
    });
    return size;
}

uint64_t p2p_stream_overlay_revision(const p2p_stream* stream) {
    return stream ? stream->overlays.revision : 0;
}

int32_t p2p_stream_overlay_item(const p2p_stream* stream, size_t index, p2p_overlay_item_v1* item) {
    return Guard([&] {
        const auto& entry = Required(stream).overlays.items.at(index);
        Required(item) = {entry.id.c_str(), entry.plugin_id.c_str(), entry.title.c_str(), entry.text.c_str(),
            static_cast<int32_t>(entry.anchor), entry.priority, entry.font_size, entry.bg_rgb, entry.text_rgb,
            entry.opacity, entry.monospace, entry.visible, entry.default_visible};
    });
}

size_t p2p_stream_read_menu(p2p_stream* stream) {
    size_t size{};
    Guard([&] {
        auto& state = Required(stream);
        state.menu = state.session.PluginMenu();
        size = state.menu.size();
    });
    return size;
}

int32_t p2p_stream_menu_item(const p2p_stream* stream, size_t index, p2p_menu_item* item) {
    return Guard([&] {
        const auto& entry = Required(stream).menu.at(index);
        Required(item) = {entry.label.c_str(), entry.plugin_id.c_str(), entry.menu.c_str(),
            entry.menu_title.c_str(), entry.path.c_str(), entry.section.c_str(), entry.id,
            entry.checked ? static_cast<int32_t>(*entry.checked) : -1, entry.menu_order, entry.order,
            entry.toolbar_button};
    });
}

int32_t p2p_stream_invoke_menu(p2p_stream* stream, uint32_t id) {
    return Guard([&] {
        auto& session = Required(stream).session;
        ViewerInput::UpdatePluginLocks(session);
        session.InvokePluginMenu(id);
    });
}

int32_t p2p_plugin_update_config(const char* plugin_id, int32_t role, const char* updates_json) {
    return Guard([&] {
        if (!plugin_id || !IsPluginIdentifier(plugin_id) ||
            (role != P2P_PLUGIN_CONFIG_VIEWER && role != P2P_PLUGIN_CONFIG_HOST) || !updates_json ||
            strnlen_s(updates_json, 16 * 1024 + 1) > 16 * 1024) {
            throw std::invalid_argument("无效的插件配置");
        }
        std::optional<Apartment> apartment;
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (CoGetApartmentType(&type, &qualifier) == CO_E_NOTINITIALIZED) {
            apartment.emplace(winrt::apartment_type::multi_threaded);
        }
        using namespace winrt::Windows::Data::Json;
        JsonObject updates{nullptr};
        if (!JsonObject::TryParse(winrt::to_hstring(updates_json), updates)) {
            throw std::invalid_argument("无效的插件配置 JSON");
        }
        for (const auto& update : updates) {
            const auto value = update.Value().ValueType();
            if (!IsPluginIdentifier(winrt::to_string(update.Key())) ||
                (value != JsonValueType::String && value != JsonValueType::Number && value != JsonValueType::Boolean)) {
                throw std::invalid_argument("无效的插件配置项");
            }
        }
        const auto directory = DefaultPluginSettingsDirectory();
        const auto extension = role == P2P_PLUGIN_CONFIG_HOST ? ".host.json" : ".viewer.json";
        if (directory.empty() || !PluginSettings(directory / (std::string(plugin_id) + extension)).Merge(updates)) {
            throw std::runtime_error("无法保存插件配置");
        }
    });
}

uint32_t p2p_stream_pending_agent_approval(const p2p_stream* stream, int32_t* tool) {
    uint32_t id{};
    Guard([&] {
        AgentTool pending{};
        id = Required(stream).session.PendingAgentApproval(&pending);
        if (tool) *tool = pending == AgentTool::screen_snapshot ? P2P_AGENT_SCREEN_SNAPSHOT : P2P_AGENT_SYSTEM_INFO;
    });
    return id;
}

int32_t p2p_stream_decide_agent_approval(p2p_stream* stream, uint32_t id, int32_t approved,
    const char* image_png, size_t image_size) {
    return Guard([&] {
        const auto image = image_png && image_size <= agent_image_limit
            ? std::string_view(image_png, image_size) : std::string_view{};
        Required(stream).session.DecideAgentApproval(id, approved != 0, image);
    });
}

int32_t p2p_stream_revoke_agent_approval(p2p_stream* stream, int32_t pause) {
    return Guard([&] { Required(stream).session.RevokeAgentApproval(pause != 0); });
}

int32_t p2p_stream_block_agent_prompt_input(p2p_stream* stream, int32_t blocked) {
    return Guard([&] { Required(stream).session.SetAgentPromptInputBlocked(blocked != 0); });
}

uint32_t p2p_stream_request_agent_summary(p2p_stream* stream, int32_t tool) {
    uint32_t id{};
    Guard([&] {
        if (tool != P2P_AGENT_SYSTEM_INFO && tool != P2P_AGENT_SCREEN_SNAPSHOT) return;
        id = Required(stream).session.RequestAgentSummary(
            tool == P2P_AGENT_SCREEN_SNAPSHOT ? AgentTool::screen_snapshot : AgentTool::system_info);
    });
    return id;
}

int32_t p2p_stream_take_agent_summary(p2p_stream* stream, uint32_t id, p2p_agent_summary* summary) {
    bool ready{};
    const auto result = Guard([&] {
        auto& state = Required(stream);
        auto& out = Required(summary);
        out = {};
        if (auto response = state.session.TakeAgentSummary(id)) {
            state.current_agent_summary = std::move(*response);
            out = {state.current_agent_summary.status.c_str(), state.current_agent_summary.snapshot.c_str(),
                state.current_agent_summary.image.data(), state.current_agent_summary.image.size()};
            ready = true;
        }
    });
    return result != P2P_OK || ready ? result : P2P_PENDING;
}

int32_t p2p_stream_cancel_agent_summary(p2p_stream* stream, uint32_t id) {
    return Guard([&] { Required(stream).session.CancelAgentSummary(id); });
}

int32_t p2p_stream_poll_ui_command(p2p_stream* stream, p2p_ui_command* command) {
    int32_t status = P2P_PENDING;
    const auto result = Guard([&] {
        auto& state = Required(stream);
        auto& out = Required(command);
        if (state.ui_commands.empty()) {
            for (auto& item : state.session.TakePluginUiCommands()) {
                state.ui_commands.push_back(std::move(item));
            }
        }
        if (state.ui_commands.empty()) {
            out = {};
            status = P2P_PENDING;
            return;
        }
        state.current_ui_command = std::move(state.ui_commands.front());
        state.ui_commands.pop_front();
        out = {state.current_ui_command.dialog_id, state.current_ui_command.kind.c_str(),
            state.current_ui_command.json.c_str()};
        status = P2P_OK;
    });
    return result == P2P_OK ? status : result;
}

int32_t p2p_stream_send_ui_event(p2p_stream* stream, uint32_t dialog_id,
    const char* control, const char* event, const char* payload_json) {
    return Guard([&] {
        auto& state = Required(stream);
        if (!dialog_id || !event || !*event) throw std::invalid_argument("无效的插件界面事件");
        state.session.SendPluginUiEvent({dialog_id, control ? control : "", event, payload_json ? payload_json : ""});
    });
}

int32_t p2p_stream_export_csv(p2p_stream* stream, const wchar_t* file) {
    return Guard([&] {
        if (!file || !*file) throw std::invalid_argument("缺少 CSV 文件路径");
        Required(stream).session.ExportCsv(file);
    });
}
