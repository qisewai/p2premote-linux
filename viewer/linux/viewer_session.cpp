#include "viewer_session.h"
#include "audio.h"

#include "minirtc_desktop_video.h"
#include "plugin_host.h"

#include <p2premote/plugin_protocol.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QStandardPaths>
#include <QSslConfiguration>
#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

namespace {

using p2premote::InputKind;

constexpr qsizetype max_frame_bytes = 256 * 1024;
constexpr qsizetype max_signal_bytes = 64 * 1024;
constexpr qsizetype frame_prefix_bytes = sizeof(quint32);
constexpr char video_stream[] = "desktop";
constexpr char control_stream[] = "control-v3";
constexpr char pointer_stream[] = "pointer";
constexpr char cursor_stream[] = "cursor";
constexpr char plugin_stream[] = "plugin";
constexpr char desktop_audio_stream[] = "desktop_audio";
constexpr char microphone_stream[] = "mic_audio";

QByteArray authenticationMac(const QByteArray& key, const QByteArray& message) {
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256).toHex();
}

bool isNonce(const QByteArray& nonce) {
    if (nonce.size() != 64) return false;
    for (const char ch : nonce) {
        if ((ch < '0' || ch > '9') && (ch < 'a' || ch > 'f')) return false;
    }
    return true;
}

bool supportsSession(const QString& sdp, bool diagnostics) {
    bool control{}, plugin{}, other_sender{}, encrypted_data{}, fingerprint{};
    for (const auto& entry : sdp.split(u'\n', Qt::SkipEmptyParts)) {
        const auto line = entry.trimmed();
        control |= line.startsWith("a=ssrc:") && line.endsWith(" name:control-v3");
        if (line.startsWith("a=ssrc:") && line.contains(" name:")) {
            plugin |= line.endsWith(" name:plugin");
            other_sender |= !line.endsWith(" name:plugin");
        }
        encrypted_data |= line == "a=x-minirtc-audio-data-srtp:1";
        fingerprint |= line.startsWith("a=fingerprint:sha-256 ");
    }
    return (diagnostics ? plugin && !other_sender : control) && encrypted_data && fingerprint;
}

TurnMode turnPolicy(const CloudConnection& cloud) {
    bool udp{}, tcp{};
    for (const auto& item : cloud.ice_servers) {
        const auto value = item.toObject().value("urls");
        for (const auto& entry : value.isString() ? QJsonArray{value} : value.toArray()) {
            const auto url = entry.toString();
            if (!url.startsWith("turn:")) continue;
            if (url.endsWith("transport=tcp")) tcp = true;
            else udp = true;
        }
    }
    if (cloud.relay_only) return udp ? TurnForceUdp : TurnForceTcp;
    return udp || tcp ? TurnAutoUdpTcp : TurnDisabled;
}

std::uint64_t monotonicMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

ViewerSession::ViewerSession() : api_(P2PRemoteMiniRtcApi(p2premote::native_minirtc::abi_version)),
    overlay_(std::make_shared<p2premote::PluginOverlays>()) {
    socket_.setReadBufferSize(max_frame_bytes + frame_prefix_bytes);
    connect(&socket_, &QTcpSocket::readyRead, this, [this] { receiveFrames(); });
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (isOpen() && error != QAbstractSocket::RemoteHostClosedError) {
            stop(QStringLiteral("信令连接失败 · %1").arg(socket_.errorString()));
        }
    });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        if (isOpen()) stop(QStringLiteral("已断开，可以重新连接。"));
    });
    web_socket_.setMaxAllowedIncomingMessageSize(max_frame_bytes);
    web_socket_.setMaxAllowedIncomingFrameSize(max_frame_bytes);
    connect(&web_socket_, &QWebSocket::connected, this, [this] {
        if (!cloud_ || !isOpen()) return;
        if (!cloudPeerMatches(cloud_->signal_url, web_socket_.sslConfiguration().peerCertificate())) {
            stop(QStringLiteral("云端信令服务身份校验失败"));
            return;
        }
        if (!sendFrame({{"type", diagnostics_ ? "join_diagnostics" : "join_internet"}, {"payload", cloud_device_},
                {"minirtc", true}})) stop(QStringLiteral("无法请求远程会话"));
    });
    connect(&web_socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
        if (!isOpen() || !cloud_) return;
        const auto document = QJsonDocument::fromJson(text.toUtf8());
        if (document.isObject()) handleFrame(document.object());
        else stop(QStringLiteral("收到无效的云端信令"));
    });
    connect(&web_socket_, &QWebSocket::disconnected, this, [this] {
        if (cloud_ && isOpen()) stop(QStringLiteral("云端信令已断开"));
    });
    connect(&web_socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (cloud_ && isOpen()) stop(QStringLiteral("云端信令连接失败 · %1").arg(web_socket_.errorString()));
    });

    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        if (state_ == State::streaming) {
            if (!diagnostics_ && !metrics_.width && statusChanged) {
                statusChanged(QStringLiteral("已连接，但暂未收到被控端画面。"));
            }
        } else if (state_ == State::approval) stop(QStringLiteral("等待被控端授权超时。"));
        else stop(state_ == State::negotiation ? QStringLiteral("媒体协商超时。") : QStringLiteral("连接或认证超时。"));
    });
    plugin_timer_.setInterval(50);
    connect(&plugin_timer_, &QTimer::timeout, this, [this] { processPluginActions(); });
}

ViewerSession::~ViewerSession() {
    statusChanged = {};
    stop({});
}

void ViewerSession::open(const QString& host, quint16 port, const QString& code) {
    if (isOpen()) close();
    if (!api_) {
        stop(QStringLiteral("MiniRTC 库版本不匹配。"));
        return;
    }
    const auto address = host.trimmed();
    if (address.isEmpty() || !port || code.size() < 6 || code.size() > 32) {
        stop(QStringLiteral("请输入被控端地址及 6 至 32 位验证码。"));
        return;
    }

    key_ = QCryptographicHash::hash("p2premote-lan-v1:" + code.toUtf8(), QCryptographicHash::Sha256);
    metrics_ = {};
    decoded_frames_.store(0, std::memory_order_relaxed);
    draw_to_present_ms_ = 0;
    state_ = State::challenge;
    if (statusChanged) statusChanged(QStringLiteral("正在连接 %1:%2…").arg(address).arg(port));
    timeout_.start(8000);
    socket_.connectToHost(address, port);
}

void ViewerSession::openCloud(CloudConnection connection, const QString& device, bool diagnostics) {
    if (isOpen()) close();
    if (web_socket_.state() != QAbstractSocket::UnconnectedState) web_socket_.abort();
    if (!api_) {
        stop(QStringLiteral("MiniRTC 库版本不匹配。"));
        return;
    }
    diagnostics_ = diagnostics;
    cloud_ = std::move(connection);
    cloud_device_ = device;
    metrics_ = {};
    decoded_frames_.store(0, std::memory_order_relaxed);
    draw_to_present_ms_ = 0;
    state_ = State::approval;
    if (statusChanged) statusChanged(QStringLiteral("正在连接云端信令…"));
    timeout_.start(15000);
    const auto request = cloudRequest(cloud_->signal_url, cloud_->token);
    if (request.url().scheme() == "wss") web_socket_.setSslConfiguration(request.sslConfiguration());
    web_socket_.open(request);
}

int ViewerSession::state() const {
    switch (state_) {
    case State::idle: return P2P_STOPPED;
    case State::challenge:
    case State::authentication: return P2P_WAITING;
    case State::approval: return P2P_APPROVAL;
    case State::negotiation: return P2P_NEGOTIATING;
    case State::streaming: return P2P_STREAMING;
    }
    return P2P_STOPPED;
}

void ViewerSession::close() {
    if (!isOpen()) return;
    sendInput(InputKind::reset);
    if (socket_.state() == QAbstractSocket::ConnectedState) {
        sendFrame({{"type", "ended"}, {"payload", "Viewer ended the session"}});
        socket_.flush();
    }
    if (web_socket_.state() == QAbstractSocket::ConnectedState) web_socket_.flush();
    stop(QStringLiteral("已断开。"));
}

void ViewerSession::stop(const QString& status) {
    state_ = State::idle;
    ++generation_;
    timeout_.stop();
    plugin_timer_.stop();
    std::shared_ptr<ViewerAudio> old_sound;
    {
        const std::lock_guard lock(sound_mutex_);
        sound_generation_ = 0;
        old_sound = std::exchange(sound_, {});
    }
    old_sound.reset();
    if (plugins_) plugins_->Stop();
    plugins_.reset();
    overlay_plugins_.reset();
    overlay_ = std::make_shared<p2premote::PluginOverlays>();
    socket_.abort();
    if (web_socket_.state() == QAbstractSocket::ConnectedState) web_socket_.close();
    else if (web_socket_.state() != QAbstractSocket::UnconnectedState) web_socket_.abort();
    incoming_.clear();
    key_.fill('\0');
    key_.clear();
    expected_host_mac_.clear();
    control_allowed_ = false;
    transport_ready_ = false;
    diagnostics_ = false;
    cloud_.reset();
    cloud_device_.clear();
    plugin_channel_ = plugin_ready_ = false;
    workspace_attempts_ = 0;
    workspace_retry_.invalidate();
    sent_buttons_.reset();
    drag_checkpoint_.reset();
    sequence_ = 0;
    video_request_ = confirmed_bitrate_ = 0;
    video_acknowledged_ = false;
    video_attempts_ = 0;
    video_retry_.invalidate();
    cursor_separate_ = false;
    cursor_retry_.invalidate();
    active_bitrate_ = requested_bitrate_;
    notice_.clear();
    ice_path_ = P2P_ICE_UNKNOWN;
    ice_protocol_.clear();
    srtp_active_ = false;
    if (peer_) {
        api_->destroy(&peer_);
        peer_ = nullptr;
    }
    context_.reset();
    {
        const std::lock_guard lock(frame_mutex_);
        pending_frame_ = {};
        recycled_frame_ = {};
        latest_frame_size_ = {};
        frame_wait_sum_ms_ = 0;
        frame_wait_samples_ = 0;
    }
    if (statusChanged) statusChanged(status);
}

bool ViewerSession::sendFrame(const QJsonObject& message) {
    const auto json = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (cloud_) {
        if (web_socket_.state() != QAbstractSocket::ConnectedState || json.size() > max_frame_bytes ||
            web_socket_.bytesToWrite() > max_frame_bytes) return false;
        return web_socket_.sendTextMessage(QString::fromUtf8(json)) == json.size();
    }
    if (socket_.state() != QAbstractSocket::ConnectedState ||
        json.size() > max_frame_bytes || socket_.bytesToWrite() > max_frame_bytes) return false;
    QByteArray packet(frame_prefix_bytes, Qt::Uninitialized);
    qToBigEndian(static_cast<quint32>(json.size()), packet.data());
    packet.append(json);
    return socket_.write(packet) == packet.size();
}

void ViewerSession::receiveFrames() {
    incoming_.append(socket_.readAll());
    while (isOpen() && incoming_.size() >= frame_prefix_bytes) {
        const auto size = qFromBigEndian<quint32>(incoming_.constData());
        if (!size || size > max_frame_bytes) {
            stop(QStringLiteral("收到无效的信令帧长度。"));
            return;
        }
        const auto packet_size = frame_prefix_bytes + size;
        if (incoming_.size() < packet_size) return;
        const auto document = QJsonDocument::fromJson(incoming_.mid(frame_prefix_bytes, size));
        incoming_.remove(0, packet_size);
        if (!document.isObject()) {
            stop(QStringLiteral("收到无效的信令消息。"));
            return;
        }
        handleFrame(document.object());
    }
}

void ViewerSession::handleFrame(const QJsonObject& message) {
    const auto type = message.value("type").toString();
    const auto payload = message.value("payload").toString();
    if (type == "error" || type == "ended") {
        stop(payload.isEmpty() ? QStringLiteral("被控端已关闭连接。") : payload);
        return;
    }

    if (cloud_ && state_ == State::approval) {
        if (type == "waiting") {
            if (statusChanged) statusChanged(QStringLiteral("等待被控端授权…"));
            timeout_.start(60000);
            return;
        }
        if (type == "accepted") {
            accept(payload);
            return;
        }
    }
    if (state_ == State::challenge && type == "challenge") {
        const auto host_nonce = message.value("nonce").toString().toLatin1();
        QByteArray random(32, Qt::Uninitialized);
        if (!isNonce(host_nonce) || RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), random.size()) != 1) {
            stop(QStringLiteral("无法完成验证码挑战。"));
            return;
        }
        const auto viewer_nonce = random.toHex();
        const auto prefix = host_nonce + ':' + viewer_nonce + ':';
        const auto proof = authenticationMac(key_, prefix + "viewer");
        expected_host_mac_ = authenticationMac(key_, prefix + "host");
        key_.fill('\0');
        key_.clear();
        state_ = State::authentication;
        if (!sendFrame({{"type", "auth"}, {"nonce", QString::fromLatin1(viewer_nonce)},
                {"hmac", QString::fromLatin1(proof)}})) {
            stop(QStringLiteral("无法发送验证码认证。"));
        }
        return;
    }
    if (state_ == State::authentication && type == "accepted") {
        const auto mac = message.value("hmac").toString().toLatin1();
        if (mac.size() != expected_host_mac_.size() || CRYPTO_memcmp(mac.data(), expected_host_mac_.data(), mac.size())) {
            stop(QStringLiteral("被控端身份校验失败。"));
            return;
        }
        expected_host_mac_.clear();
        accept(payload);
        return;
    }
    if ((state_ == State::negotiation || state_ == State::streaming) && type == "minirtc") {
        receiveSignal(payload.toUtf8());
        return;
    }
    stop(QStringLiteral("收到与当前会话状态不符的信令。"));
}

void ViewerSession::accept(const QString& mode) {
    if ((diagnostics_ && mode != "diagnostics") || (!diagnostics_ && mode != "control" && mode != "view")) {
        stop(QStringLiteral("被控端未接受请求的会话类型。"));
        return;
    }
    control_allowed_ = mode == "control";
    state_ = State::negotiation;
    if (statusChanged) statusChanged(QStringLiteral("认证成功，正在协商连接…"));
    timeout_.start(20000);
    if (!startPeer()) stop(QStringLiteral("MiniRTC 初始化失败。"));
}

bool ViewerSession::startPeer() {
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/minirtc";
    if (!QDir().mkpath(dir)) return false;
    const auto log_path = dir.toUtf8();
    Params params{};
    if (log_path.size() >= static_cast<qsizetype>(sizeof(params.log_path))) return false;
    std::memcpy(params.log_path, log_path.constData(), log_path.size());
    context_ = std::make_unique<PeerContext>(PeerContext{*this, generation_});
    params.user_id = "viewer";
    params.user_data = context_.get();
    params.turn_mode = cloud_ ? turnPolicy(*cloud_) : TurnDisabled;
    params.enable_srtp = true;
    params.hardware_acceleration = true;
    p2premote::ConfigureMiniRtcDesktopVideo(params, 60);
    params.on_signal_status = [](SignalStatus, const char*, size_t, void*) {};
    params.on_connection_status = connectionChanged;
    params.on_net_status_report = reportTraffic;
    params.on_receive_video_frame = diagnostics_ ? nullptr : receiveVideo;
    params.on_receive_audio_buffer = diagnostics_ ? nullptr : receiveAudio;
    params.on_receive_data_buffer = receiveData;

    peer_ = api_->create(&params);
    return peer_ && (diagnostics_ || (api_->add_video(peer_, video_stream) == 0
        && api_->add_data(peer_, control_stream, true) == 0
        && api_->add_data(peer_, pointer_stream, false) == 0
        && api_->add_data(peer_, cursor_stream, true) == 0 && AddAudioStream(peer_, microphone_stream) == 0))
        && (!(control_allowed_ || diagnostics_) || api_->add_data(peer_, plugin_stream, true) == 0)
        && api_->init(peer_, sendSignal, context_.get()) == 0
        && api_->join(peer_, "host") == 0;
}

void ViewerSession::sendSignal(const char* data, size_t size, void* context) noexcept {
    if (!data || !size || size > max_signal_bytes) return;
    auto& peer = *static_cast<PeerContext*>(context);
    try {
        QMetaObject::invokeMethod(&peer.session, [&session = peer.session, generation = peer.generation,
                                                 bytes = QByteArray(data, size)] {
            if (session.generation_ == generation) session.forwardSignal(bytes);
        }, Qt::QueuedConnection);
    } catch (...) {}
}

void ViewerSession::forwardSignal(const QByteArray& data) {
    const auto type = QJsonDocument::fromJson(data).object().value("type").toString();
    if (type != "join_transmission" && type != "answer" && type != "new_candidate" && type != "new_candidate_mid") return;
    if (!sendFrame({{"type", "minirtc"}, {"payload", QString::fromUtf8(data)}})) {
        stop(QStringLiteral("无法发送媒体信令。"));
    }
}

void ViewerSession::receiveSignal(const QByteArray& data) {
    if (!peer_ || data.size() > max_signal_bytes) {
        stop(QStringLiteral("收到无效的媒体信令。"));
        return;
    }
    const auto request = QJsonDocument::fromJson(data).object();
    const auto type = request.value("type").toString();
    QJsonObject notice{{"type", type}, {"transmission_id", "host"}, {"remote_user_id", "host"}};
    if (type == "offer") {
        const auto sdp = request.value("sdp").toString();
        if (!supportsSession(sdp, diagnostics_)) {
            stop(QStringLiteral("被控端缺少当前版本的加密视频或控制协议。"));
            return;
        }
        const auto expires = QDateTime::currentSecsSinceEpoch() + 600;
        plugin_channel_ = (control_allowed_ || diagnostics_) && sdp.contains(QStringLiteral(" name:plugin"));
        QJsonArray ice;
        if (cloud_) {
            for (const auto& item : cloud_->ice_servers) {
                auto server = item.toObject();
                if (server.value("urls").isString()) server.insert("urls", QJsonArray{server.value("urls")});
                if (server.contains("username")) server.insert("expires_at", expires);
                ice.append(server);
            }
        }
        notice.insert("sdp", sdp);
        notice.insert("ice", QJsonObject{{"version", 1}, {"id", QString::number(expires)},
            {"expires_at", expires}, {"servers", ice}});
    } else if (type == "new_candidate" || type == "new_candidate_mid") {
        if (type == "new_candidate") {
            notice.insert("sdp", request.value("sdp").toString());
        } else {
            notice.insert("mid", request.value("mid").toString());
            notice.insert("candidate", request.value("candidate").toString());
        }
        if (request.contains("ufrag")) notice.insert("ufrag", request.value("ufrag").toString());
    } else {
        stop(QStringLiteral("收到未知的媒体信令。"));
        return;
    }

    const auto json = QJsonDocument(notice).toJson(QJsonDocument::Compact);
    if (api_->receive_signal(peer_, json.constData(), json.size()) != 0) {
        stop(QStringLiteral("MiniRTC 拒绝了媒体信令。"));
    }
}

void ViewerSession::connectionChanged(ConnectionStatus status, const char*, size_t, void* context) {
    auto& peer = *static_cast<PeerContext*>(context);
    QMetaObject::invokeMethod(&peer.session, [&session = peer.session, generation = peer.generation, status] {
        if (generation != session.generation_) return;
        if (status == Connected && session.state_ == State::negotiation) {
            session.transport_ready_ = true;
            session.startStreaming();
        } else if (status == Closed || status == Failed || status == Disconnected ||
                   status == IncorrectPassword || status == NoSuchTransmissionId || status == RemoteUnavailable) {
            session.stop(QStringLiteral("媒体连接已断开，可以重新连接。"));
        }
    }, Qt::QueuedConnection);
}

void ViewerSession::startStreaming() {
    state_ = State::streaming;
    timeout_.stop();
    if (!diagnostics_ && !metrics_.width) timeout_.start(8000);
    connected_since_.start();
    if (!diagnostics_) {
        overlay_plugins_ = std::make_unique<p2premote::PluginHost>(false, p2premote::PluginHost::SendFrame{},
            std::filesystem::path{}, p2premote::PluginHost::Notify{}, std::filesystem::path{}, true, overlay_);
        overlay_plugins_->SetConnected(true);
        auto created_sound = std::make_shared<ViewerAudio>([this, generation = generation_](QByteArray pcm) {
            QMetaObject::invokeMethod(this, [this, generation, pcm = std::move(pcm)] {
                if (generation != generation_ || !canControl() || !audio_.mic_enabled) return;
                const MiniRtcAudioFrame frame{pcm.constData(), static_cast<std::size_t>(pcm.size()), 0};
                SendAudioFrame(peer_, &frame, microphone_stream);
            }, Qt::QueuedConnection);
        });
        {
            const std::lock_guard lock(sound_mutex_);
            sound_ = std::move(created_sound);
            sound_generation_ = generation_;
        }
    }
    if (plugin_channel_) {
        plugins_ = std::make_unique<p2premote::PluginHost>(false, [api = api_, peer = peer_](std::string_view bytes) {
            return api->send_reliable(peer, bytes.data(), bytes.size(), plugin_stream) == 0;
        }, std::filesystem::path{}, p2premote::PluginHost::Notify{}, std::filesystem::path{},
            false, overlay_, diagnostics_);
        plugins_->SetConnected(true);
        plugin_timer_.start();
    }
    applyMediaSettings();
    if (canControl()) sendCursorReady();
    tick();
    if (statusChanged) {
        if (diagnostics_) {
            statusChanged(QStringLiteral("已连接 · 只读诊断"));
        } else if (metrics_.width) {
            statusChanged(control_allowed_ ? QStringLiteral("已连接 · H.264") : QStringLiteral("已连接 · H.264 · 仅观看"));
        } else {
            statusChanged(QStringLiteral("已建立媒体通道，等待被控端画面…"));
        }
    }
}

void ViewerSession::receiveVideo(const MiniRtcVideoFrame* frame, const char*, size_t, const char* stream,
    size_t stream_size, void* context) {
    if (!frame || !stream || std::string_view(stream, stream_size) != video_stream ||
        !frame->width || !frame->height || frame->width > 2560 || frame->height > 1440 ||
        ((frame->width | frame->height) & 1u)) return;

    const auto width = static_cast<int>(frame->width);
    const auto height = static_cast<int>(frame->height);
    const auto luma = static_cast<std::size_t>(width) * height;
    const auto nv12_size = luma + luma / 2;
    if (!frame->data || frame->size < nv12_size) return;

    auto& peer = *static_cast<PeerContext*>(context);
    peer.session.queueFrame(frame->data, width, height, nv12_size, peer.generation);
}

void ViewerSession::queueFrame(const char* data, int width, int height, std::size_t size, std::uint64_t generation) {
    const auto decoded_at = std::chrono::steady_clock::now();
    VideoFrame buffer;
    {
        const std::lock_guard lock(frame_mutex_);
        buffer = std::exchange(recycled_frame_, {});
    }
    if (buffer.nv12.size() != static_cast<qsizetype>(size)) {
        buffer.nv12.resize(static_cast<qsizetype>(size));
    }
    std::memcpy(buffer.nv12.data(), data, size);
    buffer.width = width;
    buffer.height = height;
    buffer.decoded_at = decoded_at;

    bool notify{};
    {
        const std::lock_guard lock(frame_mutex_);
        if (!pending_frame_.nv12.isEmpty() && recycled_frame_.nv12.isEmpty()) {
            recycled_frame_ = std::move(pending_frame_);
        }
        pending_frame_ = std::move(buffer);
        latest_frame_size_ = {width, height};
        latest_frame_generation_ = generation;
        decoded_frames_.fetch_add(1, std::memory_order_relaxed);
        notify = !std::exchange(frame_update_pending_, true);
    }
    if (!notify) return;

    // Wake the UI without taking the frame. Painting selects the newest frame available at that point.
    QMetaObject::invokeMethod(this, [this] {
        QSize size;
        bool has_frame{};
        {
            const std::lock_guard lock(frame_mutex_);
            frame_update_pending_ = false;
            if (latest_frame_generation_ != generation_) return;
            size = latest_frame_size_;
            has_frame = !pending_frame_.nv12.isEmpty();
        }
        if (size.isEmpty()) return;
        const bool first_frame = !metrics_.width;
        metrics_.width = size.width();
        metrics_.height = size.height();
        if (has_frame && frameReady) frameReady();
        if (transport_ready_ && state_ == State::negotiation) startStreaming();
        else if (first_frame && state_ == State::streaming && !diagnostics_) {
            timeout_.stop();
            if (statusChanged) {
                statusChanged(control_allowed_ ? QStringLiteral("已连接 · H.264") : QStringLiteral("已连接 · H.264 · 仅观看"));
            }
        }
    }, Qt::QueuedConnection);
}

bool ViewerSession::takeLatestFrame(VideoFrame& frame) {
    const std::lock_guard lock(frame_mutex_);
    if (latest_frame_generation_ != generation_ || pending_frame_.nv12.isEmpty()) return false;
    std::swap(frame, pending_frame_);
    if (recycled_frame_.nv12.isEmpty()) recycled_frame_ = std::move(pending_frame_);
    pending_frame_ = {};
    return true;
}

void ViewerSession::recordFrameSwapped(double wait_ms, double draw_to_swap_ms) {
    frame_wait_sum_ms_ += wait_ms;
    ++frame_wait_samples_;
    draw_to_present_ms_ += (draw_to_swap_ms - draw_to_present_ms_) / static_cast<double>(++metrics_.presented);
}

bool ViewerSession::sendInput(InputKind kind, std::uint16_t code, std::uint16_t x, std::uint16_t y) {
    if (!canControl()) return false;
    const p2premote::InputPacket packet{kind, code, x, y, ++sequence_, monotonicMilliseconds()};
    const auto input = p2premote::EncodeInput(packet);
    const std::string_view bytes(reinterpret_cast<const char*>(input.data()), input.size());
    if (kind == InputKind::move) {
        if (sent_buttons_.any()) drag_checkpoint_ = packet;
        return api_->send_data(peer_, bytes.data(), bytes.size(), pointer_stream) == 0;
    }
    if (kind == InputKind::heartbeat && drag_checkpoint_) {
        const auto copy = p2premote::EncodeInput(*drag_checkpoint_);
        if (sendReliable({reinterpret_cast<const char*>(copy.data()), copy.size()}, control_stream)) drag_checkpoint_.reset();
    }
    if (!sendReliable(bytes, control_stream)) return false;
    if (kind == InputKind::reset) {
        sent_buttons_.reset();
        drag_checkpoint_.reset();
    } else if ((kind == InputKind::button_down || kind == InputKind::button_up) && code < sent_buttons_.size()) {
        sent_buttons_.set(code, kind == InputKind::button_down);
        drag_checkpoint_.reset();
    } else if (kind == InputKind::wheel) {
        drag_checkpoint_.reset();
    }
    return true;
}

bool ViewerSession::updateKeyboardLocks(bool caps_lock, bool num_lock) {
    if (!plugins_) return false;
    plugins_->UpdateLockState(caps_lock, num_lock);
    return plugins_->CapturesLockKeys();
}

bool ViewerSession::sendReliable(std::string_view data, const char* channel) {
    return peer_ && api_->send_reliable(peer_, data.data(), data.size(), channel) == 0;
}

void ViewerSession::receiveData(const char* data, size_t size, const char*, size_t, const char* source,
    size_t source_size, void* context) {
    if (!data || !source || size > max_frame_bytes || source_size > 32) return;
    auto& peer = *static_cast<PeerContext*>(context);
    const QByteArray label(source, source_size);
    if (label != control_stream && label != plugin_stream && label != cursor_stream) return;
    if (label == cursor_stream && size > linux_cursor_max_packet) return;
    QMetaObject::invokeMethod(&peer.session, [&session = peer.session, generation = peer.generation,
        bytes = QByteArray(data, size), label] {
        if (session.generation_ != generation) return;
        session.processData(bytes, label);
    }, Qt::QueuedConnection);
}

void ViewerSession::receiveAudio(const char* data, size_t size, const char*, size_t, const char* source,
    size_t source_size, void* context) {
    if (!data || !source || !size || size > 8192 || std::string_view(source, source_size) != desktop_audio_stream) return;
    auto& peer = *static_cast<PeerContext*>(context);
    std::shared_ptr<ViewerAudio> sound;
    {
        const std::lock_guard lock(peer.session.sound_mutex_);
        if (peer.session.sound_generation_ == peer.generation) sound = peer.session.sound_;
    }
    if (sound) sound->play(data, size);
}

void ViewerSession::processData(QByteArray data, const QByteArray& label) {
    if (state_ != State::streaming) return;
    if (label == control_stream && !diagnostics_) {
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(data.constData()), data.size());
        if (const auto packet = p2premote::DecodeInput(bytes); packet && packet->kind == InputKind::video_settings_reply &&
            packet->sequence == video_request_) {
            confirmed_bitrate_ = p2premote::VideoSettingsBitrate(*packet);
            const bool newly_acknowledged = !video_acknowledged_;
            video_acknowledged_ = true;
            if (newly_acknowledged) sendAudioSettings();
        }
    } else if (label == cursor_stream && !diagnostics_ && control_allowed_) {
        if (auto cursor = decodeRemoteCursor({data.constData(), static_cast<std::size_t>(data.size())})) {
            if (!std::exchange(cursor_separate_, true)) qInfo("P2P Remote: received separate cursor");
            if (cursorChanged) cursorChanged(std::move(*cursor));
        }
    } else if (label == plugin_stream && plugins_) {
        const std::string_view frame(data.constData(), data.size());
        const auto packet = p2premote::DecodePluginPacket(frame);
        if (!packet) return;
        if (packet->id == p2premote::workspace_protocol_id) {
            if (packet->type == "ready" && packet->payload == "1") plugin_ready_ = true;
        } else if (!diagnostics_ || packet->id == "ai-agent") {
            plugins_->Receive(frame);
        }
    }
}

void ViewerSession::reportTraffic(const char*, size_t, TraversalMode route, const MiniRtcNetTrafficStats* stats,
    const char*, size_t, void* context) {
    if (!stats || route == UnknownMode) return;
    auto& peer = *static_cast<PeerContext*>(context);
    const auto path = stats->selected_path;
    const auto rtt = stats->rtt_ms;
    const auto srtp = stats->srtp_active;
    const auto loss = stats->video_inbound_stats.loss_rate;
    const auto received = stats->video_inbound_stats.rtp_packet_count;
    const auto download = stats->total_inbound_stats.bitrate;
    const auto upload = stats->total_outbound_stats.bitrate;
    QMetaObject::invokeMethod(&peer.session, [&session = peer.session, generation = peer.generation, route, path, rtt, srtp,
        loss, received, download, upload] {
        if (session.generation_ != generation) return;
        using Path = MiniRtcNetTrafficStats::Path;
        int ice = route == Relay ? P2P_ICE_RELAY : (session.cloud_ ? P2P_ICE_NAT : P2P_ICE_HOST);
        QByteArray proto;
        if (path == Path::Relay) {
            ice = P2P_ICE_RELAY;
        } else if (path != Path::Unknown) {
            ice = (path == Path::HostUdp || path == Path::HostTcp) ? P2P_ICE_HOST : P2P_ICE_NAT;
            proto = (path == Path::HostUdp || path == Path::DirectUdp) ? QByteArrayLiteral("udp") : QByteArrayLiteral("tcp");
        }
        session.ice_path_ = ice;
        session.ice_protocol_ = proto;
        session.srtp_active_ = srtp;
        auto& metrics = session.metrics_;
        metrics.route = static_cast<p2premote::IcePath>(ice);
        metrics.protocol = proto.toStdString();
        metrics.rtt_ms = rtt >= 0 ? std::optional<double>{rtt} : std::nullopt;
        metrics.loss = received ? std::optional<double>{loss} : std::nullopt;
        metrics.download_bps = download;
        metrics.upload_bps = upload;
    }, Qt::QueuedConnection);
}

void ViewerSession::setQuality(int resolution, int bitrate) {
    if (resolution == requested_quality_ && bitrate == requested_bitrate_) return;
    requested_quality_ = resolution;
    requested_bitrate_ = bitrate;
    active_bitrate_ = bitrate;
    video_attempts_ = 0;
    video_acknowledged_ = false;
    if (isViewable()) sendVideoSettings();
}

void ViewerSession::setAudio(p2p_audio_options_v1 options) {
    audio_ = options;
    if (isViewable()) sendAudioSettings();
}

void ViewerSession::applyMediaSettings() {
    if (!isViewable()) return;
    sendVideoSettings();
    sendAudioSettings();
}

void ViewerSession::sendCursorReady() {
    cursor_retry_.start();
    sendInput(InputKind::cursor_ready);
}

void ViewerSession::sendVideoSettings() {
    using namespace p2premote;
    ++video_attempts_;
    video_retry_.start();
    const auto packet = VideoSettingsPacket(InputKind::set_video_settings, static_cast<StreamQuality>(requested_quality_),
        VideoBitrateLimit(static_cast<VideoBitratePreset>(active_bitrate_)), ++sequence_, monotonicMilliseconds());
    const auto bytes = EncodeInput(packet);
    if (sendReliable({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, control_stream)) {
        video_request_ = packet.sequence;
    }
}

void ViewerSession::sendAudioSettings() {
    using namespace p2premote;
    std::uint16_t flags{};
    if (audio_.speaker_enabled) flags |= audio_flag_speaker;
    if (audio_.mic_enabled && canControl()) flags |= audio_flag_mic;
    if (audio_.mic_speaker_fallback) flags |= audio_flag_mic_fallback;
    const auto bytes = EncodeInput({InputKind::set_audio_settings, flags, 0, 0, ++sequence_, monotonicMilliseconds()});
    sendReliable({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, control_stream);
    if (sound_) sound_->configure(audio_.speaker_enabled, audio_.mic_enabled && canControl(), audio_.speaker_volume);
    SetAudioSendPaused(peer_, "host", 4, !(audio_.mic_enabled && canControl()));
}

void ViewerSession::tick() {
    if (state_ != State::streaming) return;
    metrics_.seconds = connected_since_.elapsed() / 1000.0;
    const auto wait_samples = std::exchange(frame_wait_samples_, 0);
    const auto wait_sum = std::exchange(frame_wait_sum_ms_, 0.0);
    if (wait_samples > 0) metrics_.frame_wait_ms = wait_sum / static_cast<double>(wait_samples);
    if (!notice_.isEmpty() && notice_age_.elapsed() >= 3500) notice_.clear();
    if (isViewable() && !video_acknowledged_ && video_attempts_ < 12 &&
        video_retry_.isValid() && video_retry_.hasExpired(1000)) {
        sendVideoSettings();
    }
    const auto cursor_retry_ms = connected_since_.elapsed() < 12'000 ? 1000 : 4000;
    if (canControl() && !cursor_separate_ && cursor_retry_.isValid() && cursor_retry_.hasExpired(cursor_retry_ms)) {
        sendCursorReady();
    }
    if (overlay_plugins_) overlay_plugins_->UpdateMetrics(metrics_);
    if (!plugins_) return;
    plugins_->UpdateMetrics(metrics_);
    if (!diagnostics_ && !plugin_ready_ && workspace_attempts_ < 12 &&
        (!workspace_retry_.isValid() || workspace_retry_.hasExpired(1000))) {
        if (const auto hello = p2premote::EncodePluginPacket({p2premote::workspace_protocol_id, "hello", "1"})) {
            sendReliable(*hello, plugin_stream);
        }
        ++workspace_attempts_;
        workspace_retry_.start();
    }
}

void ViewerSession::processPluginActions() {
    if (!plugins_) return;
    for (auto& action : plugins_->TakeActions()) {
        using Kind = p2premote::PluginAction::Kind;
        if (action.kind == Kind::notice) {
            notice_ = QString::fromStdWString(action.notice);
            notice_age_.start();
        } else if (action.kind == Kind::bitrate && isViewable() && requested_bitrate_ == P2P_AUTOMATIC) {
            const auto requested = static_cast<int>(action.bitrate);
            if (active_bitrate_ == requested) continue;
            active_bitrate_ = requested;
            video_attempts_ = 0;
            video_acknowledged_ = false;
            sendVideoSettings();
        } else if (action.kind == Kind::input && canControl() && plugin_ready_) {
            action.input.sent_ms = monotonicMilliseconds();
            const auto bytes = p2premote::EncodeInputCommand(action.input);
            if (!bytes.empty()) sendReliable({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, control_stream);
        }
    }
}
