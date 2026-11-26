#include "cloud_host.h"
#include "cloud_connection.h"
#include "host_session.h"
#include "service_support.h"
#include "x11_shm_capturer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTimer>
#include <QWebSocket>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace p2premote::linux_host {
namespace {

constexpr qsizetype max_packet_bytes = 256 * 1024;
constexpr qsizetype max_signal_bytes = 64 * 1024;
volatile std::sig_atomic_t stopping = 0;

struct CloudConfig {
    QUrl origin;
    QString user, password, installation, device;
    bool relay_only{};
};

std::optional<CloudConfig> ParseConfig(std::string_view data) {
    const auto json = QJsonDocument::fromJson(QByteArray(data.data(), data.size())).object();
    CloudConfig cfg{QUrl(json.value("server").toString(), QUrl::StrictMode), json.value("user").toString(),
        json.value("password").toString(), json.value("installationId").toString(),
        json.value("deviceId").toString(), json.value("relayOnly").toBool()};
    static const QRegularExpression account_name(QStringLiteral("\\A[A-Za-z0-9._-]{3,32}\\z"));
    const auto password_size = cfg.password.toUtf8().size();
    if (!isCloudOrigin(cfg.origin) || !account_name.match(cfg.user).hasMatch() ||
        password_size < 8 || password_size > 72 || !validIdentifier(cfg.installation, 43) ||
        !validIdentifier(cfg.device, 24)) return std::nullopt;
    return cfg;
}

TurnMode TravelMode(const QJsonArray& servers, bool relay_only) {
    bool udp{}, tcp{};
    for (const auto& entry : servers) {
        const auto value = entry.toObject().value("urls");
        for (const auto& url_value : value.isString() ? QJsonArray{value} : value.toArray()) {
            const auto url = url_value.toString();
            if (!url.startsWith("turn:")) continue;
            if (url.endsWith("transport=tcp")) tcp = true;
            else udp = true;
        }
    }
    if (relay_only) return udp ? TurnForceUdp : TurnForceTcp;
    return udp || tcp ? TurnAutoUdpTcp : TurnDisabled;
}

QJsonObject IceConfiguration(const QJsonArray& entries) {
    const auto expires = QDateTime::currentSecsSinceEpoch() + 600;
    QJsonArray servers;
    for (const auto& entry : entries) {
        auto server = entry.toObject();
        if (server.value("urls").isString()) server["urls"] = QJsonArray{server.value("urls")};
        if (server.contains("username")) server["expires_at"] = expires;
        servers.append(server);
    }
    return {{"version", 1}, {"id", QString::number(expires)}, {"expires_at", expires}, {"servers", servers}};
}

bool EncryptedControl(const QString& sdp) {
    bool encrypted{}, fingerprint{}, control{};
    for (const auto& line : sdp.split(u'\n')) {
        const auto field = line.trimmed();
        encrypted |= field == "a=x-minirtc-audio-data-srtp:1";
        fingerprint |= field.startsWith("a=fingerprint:sha-256 ");
        control |= field.startsWith("a=ssrc:") && field.endsWith(" name:control-v3");
    }
    return encrypted && fingerprint && control;
}

class CloudHost final : public QObject {
public:
    CloudHost(CloudConfig config, HostConfig media)
        : config_(std::move(config)), device_(config_.device.toStdString()) {
        LinuxHostSession::CloudCallbacks callbacks;
        callbacks.send_signal = [this](std::string payload, std::uint64_t generation) {
            QMetaObject::invokeMethod(this, [this, payload = std::move(payload), generation] {
                if (state_ != State::session || generation != media_->PeerGeneration()) return;
                const auto body = QByteArray::fromStdString(payload);
                const auto type = QJsonDocument::fromJson(body).object().value("type").toString();
                if (type == "offer" || type == "new_candidate" || type == "new_candidate_mid") {
                    if (!send("minirtc", QString::fromUtf8(body))) reconnect(false);
                }
            }, Qt::QueuedConnection);
        };
        callbacks.disconnected = [this](std::uint64_t generation) {
            QMetaObject::invokeMethod(this, [this, generation] {
                if (state_ == State::session && generation == media_->PeerGeneration()) endRoom();
            }, Qt::QueuedConnection);
        };
        callbacks.status = [this](std::string_view status) {
            const auto generation = media_->PeerGeneration();
            QMetaObject::invokeMethod(this, [this, status, generation] {
                if (state_ != State::session || generation != media_->PeerGeneration()) return;
                deadline_.stop();
                Publish(status);
            }, Qt::QueuedConnection);
        };
        media_ = std::make_unique<LinuxHostSession>(
            std::move(media), std::make_unique<X11ShmCapturer>(), std::move(callbacks));

        retry_.setSingleShot(true);
        deadline_.setSingleShot(true);
        connect(&retry_, &QTimer::timeout, this, [this] {
            if (token_.isEmpty()) login();
            else listen();
        });
        connect(&deadline_, &QTimer::timeout, this, [this] {
            if (state_ == State::session) endRoom();
            else reconnect(true);
        });
        socket_.setMaxAllowedIncomingMessageSize(max_packet_bytes);
        socket_.setMaxAllowedIncomingFrameSize(max_packet_bytes);
        connect(&socket_, &QWebSocket::connected, this, [this] {
            if (!cloudPeerMatches(socket_.requestUrl(), socket_.sslConfiguration().peerCertificate()) ||
                !send("listen_internet", {})) reconnect(true);
        });
        connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
            const auto message = QJsonDocument::fromJson(text.toUtf8());
            if (!message.isObject()) reconnect(true);
            else handle(message.object());
        });
        connect(&socket_, &QWebSocket::disconnected, this, [this] {
            if (state_ != State::retry && state_ != State::locked) reconnect(state_ == State::opening);
        });
        connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (state_ != State::retry && state_ != State::locked) reconnect(state_ == State::opening);
        });
    }

    bool start() {
        if (!media_->Valid()) return false;
        NotifyCloudState("login", device_, true);
        login();
        return true;
    }

private:
    enum class State { retry, login, opening, waiting, session, locked };

    void Publish(std::string_view status) { NotifyCloudState(status, device_); }

    void login() {
        state_ = State::login;
        Publish("login");
        auto url = config_.origin;
        url.setPath("/api/v2/login");
        auto request = cloudRequest(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
        const auto hostname = QSysInfo::machineHostName().left(64);
        const QJsonObject payload{{"username", config_.user}, {"password", config_.password},
            {"installationId", config_.installation}, {"serviceDeviceId", config_.device},
            {"deviceName", hostname.isEmpty() ? QStringLiteral("Linux") : hostname}, {"platform", "Linux x64"}};
        auto* reply = network_.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        reply->setReadBufferSize(max_signal_bytes + 1);
        connect(reply, &QNetworkReply::readyRead, reply, [reply] {
            if (reply->bytesAvailable() > max_signal_bytes) reply->abort();
        });
        connect(reply, &QNetworkReply::encrypted, reply, [reply] {
            if (!cloudPeerMatches(reply->url(), reply->sslConfiguration().peerCertificate())) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto body = reply->readAll();
            const auto error = reply->error();
            reply->deleteLater();
            if (code == 401 || code == 403) {
                state_ = State::locked;
                Publish(code == 403 ? "removed" : "credentials");
                return;
            }
            if (error != QNetworkReply::NoError || code != 200 || body.size() > max_signal_bytes) {
                reconnect(true);
                return;
            }
            const auto result = QJsonDocument::fromJson(body).object();
            const auto token = result.value("accessToken").toString();
            const auto ice = result.value("iceServers").toArray();
            if (!validIdentifier(token, 43) || result.value("currentDeviceId").toString() != config_.device ||
                (!ice.isEmpty() && !parseIceServers(ice)) || (ice.isEmpty() && config_.relay_only)) {
                reconnect(true);
                return;
            }
            token_ = token.toLatin1();
            ice_ = ice;
            listen();
        });
    }

    void listen() {
        state_ = State::opening;
        auto url = config_.origin;
        url.setScheme(url.scheme() == "https" ? "wss" : "ws");
        url.setPath("/signal");
        const auto request = cloudRequest(url, token_);
        if (url.scheme() == "wss") socket_.setSslConfiguration(request.sslConfiguration());
        deadline_.start(12000);
        socket_.open(request);
    }

    bool send(const QString& type, const QString& payload) {
        const auto json = QJsonDocument(QJsonObject{{"type", type}, {"payload", payload}, {"minirtc", true}})
            .toJson(QJsonDocument::Compact);
        return socket_.state() == QAbstractSocket::ConnectedState && json.size() <= max_packet_bytes &&
            socket_.bytesToWrite() <= max_packet_bytes && socket_.sendTextMessage(QString::fromUtf8(json)) == json.size();
    }

    void endRoom() {
        send("stop", {});
        reconnect(false);
    }

    void reconnect(bool new_token) {
        if (state_ == State::retry || state_ == State::locked) return;
        state_ = State::retry;
        deadline_.stop();
        media_->StopCloudPeer();
        socket_.abort();
        if (new_token) token_.clear();
        Publish("reconnecting");
        retry_.start(retry_ms_);
        retry_ms_ = std::min(retry_ms_ * 2, 30000);
    }

    void handle(const QJsonObject& message) {
        const auto type = message.value("type").toString();
        if (type == "error" || type == "ended") {
            reconnect(type == "error" && !message.value("retryable").toBool());
            return;
        }
        if (type == "ready" && state_ == State::opening) {
            state_ = State::waiting;
            deadline_.stop();
            retry_ms_ = 500;
            Publish("waiting");
            return;
        }
        if (type == "request_diagnostics" && state_ == State::waiting) {
            if (!send("reject", {})) reconnect(false);
            return;
        }
        if (type == "request" && state_ == State::waiting) {
            if (!media_->StartCloudPeer(TravelMode(ice_, config_.relay_only))) {
                endRoom();
                return;
            }
            state_ = State::session;
            Publish("negotiating");
            deadline_.start(25000);
            if (!send("accept", "control")) reconnect(false);
            return;
        }
        if (type != "minirtc" || state_ != State::session) {
            reconnect(false);
            return;
        }
        const auto payload = message.value("payload").toString().toUtf8();
        if (payload.isEmpty() || payload.size() > max_signal_bytes) {
            endRoom();
            return;
        }
        auto signal = QJsonDocument::fromJson(payload).object();
        const auto kind = signal.value("type").toString();
        if (kind == "user_join_transmission") signal.insert("ice", IceConfiguration(ice_));
        if ((kind == "answer" && !EncryptedControl(signal.value("sdp").toString())) ||
            (kind != "user_join_transmission" && kind != "answer" && kind != "new_candidate" && kind != "new_candidate_mid") ||
            !media_->ReceiveCloudSignal(QJsonDocument(signal).toJson(QJsonDocument::Compact).toStdString())) {
            endRoom();
        }
    }

    CloudConfig config_;
    std::string device_;
    QByteArray token_;
    QJsonArray ice_;
    QNetworkAccessManager network_;
    QWebSocket socket_;
    QTimer retry_, deadline_;
    int retry_ms_{500};
    State state_{State::retry};
    std::unique_ptr<LinuxHostSession> media_;
};

} // namespace

bool ValidCloudHostConfig(std::string_view data) { return ParseConfig(data).has_value(); }

int RunCloudHost(int argc, char** argv) {
    std::optional<std::string> saved;
    if (!LoadCloudServiceConfig(saved) || !saved) {
        std::cerr << "[cloud] Cloud service configuration is missing or inaccessible\n";
        return 1;
    }
    auto config = ParseConfig(*saved);
    if (!config) {
        std::cerr << "[cloud] Invalid cloud service configuration\n";
        return 1;
    }
    const char* logs = std::getenv("LOGS_DIRECTORY");
    if (!logs || !*logs) {
        std::cerr << "[cloud] Systemd must supply the protected log directory\n";
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });

    QCoreApplication app(argc, argv);
    HostConfig media;
    media.log_dir = logs;
    CloudHost service(std::move(*config), std::move(media));
    if (!service.start()) return 1;
    QTimer stop_timer;
    QObject::connect(&stop_timer, &QTimer::timeout, &app, [&] {
        if (stopping) app.quit();
    });
    stop_timer.start(100);
    return app.exec();
}

} // namespace p2premote::linux_host
