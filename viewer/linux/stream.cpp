#include "viewer_session.h"
#include "video_surface.h"
#include "plugin_host.h"
#include "plugin_platform.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QWidget>

#include <deque>

using namespace p2premote;

struct p2p_stream {
    struct Lan {
        QString host, code;
        quint16 port{};
    };

    explicit p2p_stream(QWidget* widget) : video(widget) {
        if (video) attachVideoSession(video, &session);
        session.frameReady = [this] {
            if (video) notifyVideoFrameReady(video);
        };
        session.cursorChanged = [this](DecodedCursor cursor) {
            if (video) setVideoCursor(video, std::move(cursor));
        };
        session.statusChanged = [this](const QString& text) {
            status = text.toStdWString();
            const int next_state = p2p_stream_state(this);
            if (video && next_state != last_video_state && (next_state == P2P_STOPPED || next_state == P2P_RECONNECTING)) {
                attachVideoSession(video, &session);
            }
            last_video_state = next_state;
        };
    }

    ~p2p_stream() {
        if (video) attachVideoSession(video, nullptr);
        session.cursorChanged = {};
        session.statusChanged = {};
        session.close();
    }

    QWidget* video{};
    ViewerSession session;
    std::optional<Lan> lan;
    const p2p_account* account{};
    QString device;
    std::string stun;
    bool stun_only{};
    bool diagnostics{};
    bool started{};
    int last_video_state{P2P_IDLE};
    int quality{P2P_1080P}, bitrate{P2P_AUTOMATIC};
    std::wstring status, notice;
    PluginOverlaySnapshot overlays;
    std::vector<PluginMenuItem> menu;
    std::deque<PluginUiCommand> commands;
    PluginUiCommand current_command;
    AgentSummary summary;
};

namespace {

int fail(const QString& message) {
    setCoreError(message);
    return P2P_ERROR;
}

QString fromWide(const wchar_t* text) { return text ? QString::fromWCharArray(text) : QString{}; }

std::optional<CloudConnection> tuneIce(const CloudConnection& original, const std::string& stun, bool stun_only) {
    auto result = original;
    QJsonArray servers;
    bool replaced{}, has_relay{};
    if (result.relay_only && stun_only) return {};
    for (const auto& item : original.ice_servers) {
        auto server = item.toObject();
        const auto value = server.value("urls");
        QJsonArray urls;
        for (const auto& entry : value.isString() ? QJsonArray{value} : value.toArray()) {
            const auto url = entry.toString();
            if (url.startsWith("stun:")) {
                urls.append(stun.empty() ? url : QString::fromStdString(stun));
                replaced = true;
            } else if (!stun_only && url.startsWith("turn:")) {
                urls.append(url);
                has_relay = true;
            }
        }
        if (!urls.isEmpty()) {
            server.insert("urls", urls);
            servers.append(server);
        }
    }
    if (!stun.empty() && !replaced) servers.append(QJsonObject{{"urls", QJsonArray{QString::fromStdString(stun)}}});
    if (servers.isEmpty() || (result.relay_only && !has_relay)) return {};
    result.ice_servers = servers;
    return result;
}

} // namespace

p2p_stream* p2p_stream_create(const p2p_account* account, int32_t host, const wchar_t* device,
    void* video, const p2p_connection_options_v1* options) {
    const auto target = fromWide(device);
    if (host || target.isEmpty() || !cloudConnection(account)) {
        fail(QStringLiteral("请登录后选择远程设备"));
        return nullptr;
    }
    auto stream = std::make_unique<p2p_stream>(static_cast<QWidget*>(video));
    stream->account = account;
    stream->device = target;
    if (options) {
        if (options->stun_url) stream->stun = options->stun_url;
        stream->stun_only = options->stun_only;
        stream->diagnostics = options->diagnostics;
    }
    return stream.release();
}

p2p_stream* p2p_stream_create_lan(int32_t host, void* video, const p2p_lan_options_v1* options) {
    if (host || !options || !options->host_address || !options->access_code) {
        fail(QStringLiteral("请输入远程设备地址及连接验证码"));
        return nullptr;
    }
    auto stream = std::make_unique<p2p_stream>(static_cast<QWidget*>(video));
    stream->lan = p2p_stream::Lan{fromWide(options->host_address), fromWide(options->access_code),
        options->port ? options->port : static_cast<quint16>(P2P_LAN_DEFAULT_PORT)};
    return stream.release();
}

void p2p_stream_destroy(p2p_stream* stream) { delete stream; }

int32_t p2p_stream_start(p2p_stream* stream, void*, uint32_t) {
    if (!stream) return P2P_ERROR;
    if (stream->session.isOpen()) return P2P_OK;
    stream->started = true;
    stream->commands.clear();
    if (stream->lan) {
        const auto& target = *stream->lan;
        stream->session.open(target.host, target.port, target.code);
    } else {
        const auto* connection = cloudConnection(stream->account);
        if (!connection) return fail(QStringLiteral("登录已失效，请重新登录"));
        auto options = tuneIce(*connection, stream->stun, stream->stun_only);
        if (!options) return fail(QStringLiteral("云端会话缺少可用的 STUN 或 TURN 配置"));
        stream->session.openCloud(std::move(*options), stream->device, stream->diagnostics);
    }
    return P2P_OK;
}

int32_t p2p_stream_stop(p2p_stream* stream, const wchar_t* reason) {
    if (!stream) return P2P_ERROR;
    stream->session.close();
    if (reason && *reason) stream->status = reason;
    stream->commands.clear();
    return P2P_OK;
}

int32_t p2p_stream_quality(p2p_stream* stream, uint32_t quality) {
    if (!stream || quality < P2P_720P || quality > P2P_1440P) return P2P_ERROR;
    stream->quality = quality;
    stream->session.setQuality(stream->quality, stream->bitrate);
    return P2P_OK;
}

int32_t p2p_stream_bitrate(p2p_stream* stream, uint32_t bitrate) {
    if (!stream || bitrate < P2P_ECONOMY || bitrate > P2P_DETAIL) return P2P_ERROR;
    stream->bitrate = bitrate;
    stream->session.setQuality(stream->quality, stream->bitrate);
    return P2P_OK;
}

int32_t p2p_stream_set_audio(p2p_stream* stream, const p2p_audio_options_v1* values) {
    if (!stream || !values) return P2P_ERROR;
    stream->session.setAudio(*values);
    return P2P_OK;
}

int32_t p2p_stream_resize(p2p_stream* stream, int32_t) { return stream ? P2P_OK : P2P_ERROR; }

int32_t p2p_stream_tick(p2p_stream* stream) {
    if (!stream) return P2P_ERROR;
    stream->session.tick();
    return P2P_OK;
}

int32_t p2p_stream_state(const p2p_stream* stream) {
    return stream && stream->started ? stream->session.state() : P2P_IDLE;
}

int32_t p2p_stream_input_ready(const p2p_stream* stream) { return stream && stream->session.canControl(); }

int32_t p2p_stream_snapshot(p2p_stream* stream, p2p_stream_snapshot_v1* out) {
    if (!stream || !out) return P2P_ERROR;
    const auto& session = stream->session;
    const auto& m = session.metrics();
    const auto decoded = session.decodedFrames();
    stream->notice = session.notice().toStdWString();
    const auto* plugins = session.plugins();
    *out = {stream->status.c_str(), L"H264 (MiniRTC)", stream->notice.c_str(), session.iceProtocol().constData(),
        0, 0, decoded, decoded, m.presented, m.width, m.height, m.upload_bps.value_or(0),
        m.download_bps.value_or(0), session.confirmedBitrate(), p2p_stream_state(stream), session.icePath(),
        session.canControl(), session.canControl(), plugins && plugins->HasMenu(), session.srtpActive(), 0, 0,
        m.seconds, m.rtt_ms.value_or(-1), m.loss.value_or(-1), -1, session.drawToPresentMs(), -1,
        m.frame_wait_ms.value_or(-1)};
    return P2P_OK;
}

size_t p2p_stream_read_overlays(p2p_stream* stream) {
    if (!stream) return 0;
    stream->session.overlays()->Read(stream->overlays);
    return stream->overlays.items.size();
}

uint64_t p2p_stream_overlay_revision(const p2p_stream* stream) { return stream ? stream->overlays.revision : 0; }

int32_t p2p_stream_overlay_item(const p2p_stream* stream, size_t index, p2p_overlay_item_v1* item) {
    if (!stream || !item || index >= stream->overlays.items.size()) return P2P_ERROR;
    const auto& entry = stream->overlays.items[index];
    *item = {entry.id.c_str(), entry.plugin_id.c_str(), entry.title.c_str(), entry.text.c_str(),
        static_cast<int32_t>(entry.anchor), entry.priority, entry.font_size, entry.bg_rgb, entry.text_rgb,
        entry.opacity, entry.monospace, entry.visible, entry.default_visible};
    return P2P_OK;
}

size_t p2p_stream_read_menu(p2p_stream* stream) {
    if (!stream) return 0;
    auto* plugins = stream->session.plugins();
    stream->menu = plugins ? plugins->Menu() : std::vector<PluginMenuItem>{};
    return stream->menu.size();
}

int32_t p2p_stream_menu_item(const p2p_stream* stream, size_t index, p2p_menu_item* item) {
    if (!stream || !item || index >= stream->menu.size()) return P2P_ERROR;
    const auto& entry = stream->menu[index];
    *item = {entry.label.c_str(), entry.plugin_id.c_str(), entry.menu.c_str(), entry.menu_title.c_str(),
        entry.path.c_str(), entry.section.c_str(), entry.id, entry.checked ? static_cast<int32_t>(*entry.checked) : -1,
        entry.menu_order, entry.order, entry.toolbar_button};
    return P2P_OK;
}

int32_t p2p_stream_invoke_menu(p2p_stream* stream, uint32_t id) {
    if (stream && stream->session.plugins()) stream->session.plugins()->Invoke(id);
    return P2P_OK;
}

int32_t p2p_stream_poll_ui_command(p2p_stream* stream, p2p_ui_command* command) {
    if (!stream || !command) return P2P_ERROR;
    if (stream->commands.empty()) {
        if (auto* plugins = stream->session.plugins()) {
            for (auto& item : plugins->TakeUiCommands()) stream->commands.push_back(std::move(item));
        }
    }
    if (stream->commands.empty()) return P2P_PENDING;
    auto& item = stream->current_command;
    item = std::move(stream->commands.front());
    stream->commands.pop_front();
    *command = {item.dialog_id, item.kind.c_str(), item.json.c_str()};
    return P2P_OK;
}

int32_t p2p_stream_send_ui_event(p2p_stream* stream, uint32_t dialog, const char* control,
    const char* event, const char* json) {
    if (!stream || !dialog || !event || !*event || !stream->session.plugins()) return P2P_ERROR;
    stream->session.plugins()->SendUiEvent({dialog, control ? control : "", event, json ? json : ""});
    return P2P_OK;
}

int32_t p2p_plugin_update_config(const char* id, int32_t role, const char* updates) {
    const auto key = QString::fromUtf8(id);
    static const QRegularExpression pattern(QStringLiteral("^[a-zA-Z0-9_-]{1,64}$"));
    const auto json = QJsonDocument::fromJson(updates ? QByteArray(updates) : QByteArray{});
    const auto directory = DefaultPluginSettingsDirectory();
    if (role != P2P_PLUGIN_CONFIG_VIEWER || !pattern.match(key).hasMatch() || !json.isObject() ||
        directory.empty()) return P2P_ERROR;
    PluginSettings settings(directory / (key.toStdString() + ".viewer.json"));
    return settings.merge(json.object()) ? P2P_OK : P2P_ERROR;
}

int32_t p2p_stream_export_csv(p2p_stream* stream, const wchar_t* name) {
    if (!stream || !name || !*name) return fail(QStringLiteral("缺少 CSV 文件路径"));
    QSaveFile file(fromWide(name));
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    const auto& stats = stream->session.metrics();
    QTextStream out(&file);
    out << "seconds,width,height,presented,ice_path,rtt_ms,loss_fraction,download_bps,upload_bps,"
           "decoded,decoded_frame_wait_ms,draw_to_present_ms\n"
        << stats.seconds << ',' << stats.width << ',' << stats.height << ',' << stats.presented << ','
        << static_cast<int>(stats.route) << ',' << stats.rtt_ms.value_or(-1) << ',' << stats.loss.value_or(-1) << ','
        << stats.download_bps.value_or(0) << ',' << stats.upload_bps.value_or(0) << ','
        << stream->session.decodedFrames() << ',' << stats.frame_wait_ms.value_or(-1) << ','
        << stream->session.drawToPresentMs() << '\n';
    out.flush();
    return out.status() == QTextStream::Ok && file.commit() ? P2P_OK : fail(file.errorString());
}

uint32_t p2p_stream_request_agent_summary(p2p_stream* stream, int32_t tool) {
    if (!stream || !stream->session.plugins()) return 0;
    if (tool == P2P_AGENT_SYSTEM_INFO) return stream->session.plugins()->RequestAgentSummary(AgentTool::system_info);
    if (tool == P2P_AGENT_SCREEN_SNAPSHOT) return stream->session.plugins()->RequestAgentSummary(AgentTool::screen_snapshot);
    return 0;
}

int32_t p2p_stream_take_agent_summary(p2p_stream* stream, uint32_t id, p2p_agent_summary* out) {
    if (!stream || !out || !stream->session.plugins()) return P2P_ERROR;
    auto summary = stream->session.plugins()->TakeAgentSummary(id);
    if (!summary) return P2P_PENDING;
    stream->summary = std::move(*summary);
    const auto& item = stream->summary;
    *out = {item.status.c_str(), item.snapshot.c_str(), item.image.data(), item.image.size()};
    return P2P_OK;
}

int32_t p2p_stream_cancel_agent_summary(p2p_stream* stream, uint32_t id) {
    if (!stream || !stream->session.plugins()) return P2P_ERROR;
    stream->session.plugins()->CancelAgentSummary(id);
    return P2P_OK;
}
