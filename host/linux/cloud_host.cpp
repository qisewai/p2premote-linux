#include "cloud_host.h"
#include <utility>
#include "cloud_connection.h"
#include "host_session.h"
#include "service_support.h"
#include "update/update_archive.h"
#include "x11_shm_capturer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebSocket>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

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
        : config_(std::move(config)), device_(config_.device.toStdString()), media_config_(std::move(media)) {
        retry_.setSingleShot(true);
        deadline_.setSingleShot(true);
        connect(&retry_, &QTimer::timeout, this, [this] {
            if (state_ != State::retry) return;
            if (token_.isEmpty()) login();
            else listen();
        });
        connect(&deadline_, &QTimer::timeout, this, [this] {
            if (state_ == State::standby) return;
            if (state_ == State::session) endRoom();
            else reconnect(true);
        });
        ping_timer_.setInterval(15000);
        connect(&ping_timer_, &QTimer::timeout, this, [this] {
            if (socket_.state() == QAbstractSocket::ConnectedState) socket_.ping();
        });
        socket_.setMaxAllowedIncomingMessageSize(max_packet_bytes);
        socket_.setMaxAllowedIncomingFrameSize(max_packet_bytes);
        connect(&socket_, &QWebSocket::connected, this, [this] {
            if (state_ != State::opening) return;
            if (!cloudPeerMatches(socket_.requestUrl(), socket_.sslConfiguration().peerCertificate()) ||
                !send("listen_internet", {})) {
                reconnect(true);
                return;
            }
            ping_timer_.start();
        });
        connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
            const auto message = QJsonDocument::fromJson(text.toUtf8());
            if (!message.isObject()) reconnect(true);
            else handle(message.object());
        });
        connect(&socket_, &QWebSocket::disconnected, this, [this] {
            if (state_ != State::retry && state_ != State::locked && state_ != State::standby) {
                reconnect(state_ == State::opening);
            }
        });
        connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (state_ != State::retry && state_ != State::locked && state_ != State::standby) {
                reconnect(state_ == State::opening);
            }
        });
    }

    void start(bool active) {
        if (active) {
            NotifyCloudState("login", device_, false);
            login();
        } else {
            state_ = State::standby;
            NotifyCloudState("standby", device_, false);
        }
    }

    void setActive(bool active) {
        if (!active && state_ != State::standby) {
            state_ = State::standby;
            retry_.stop();
            deadline_.stop();
            ping_timer_.stop();
            if (auto* reply = std::exchange(login_reply_, nullptr)) reply->abort();
            token_.clear();
            ice_ = {};
            if (media_) media_->StopCloudPeer();
            socket_.abort();
            Publish("standby");
        } else if (active && state_ == State::standby) {
            retry_ms_ = 500;
            login();
        }
    }

    bool inSession() const noexcept { return state_ == State::session; }

private:
    enum class State { retry, login, opening, waiting, session, locked, standby };

    bool ensureMedia() {
        if (media_) return true;
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
            media_config_, std::make_unique<X11ShmCapturer>(), std::move(callbacks));
        if (!media_->Valid()) media_.reset();
        return media_ != nullptr;
    }

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
        login_reply_ = reply;
        reply->setReadBufferSize(max_signal_bytes + 1);
        connect(reply, &QNetworkReply::readyRead, reply, [reply] {
            if (reply->bytesAvailable() > max_signal_bytes) reply->abort();
        });
        connect(reply, &QNetworkReply::encrypted, reply, [reply] {
            if (!cloudPeerMatches(reply->url(), reply->sslConfiguration().peerCertificate())) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            if (reply != login_reply_ || state_ != State::login) return;
            login_reply_ = nullptr;
            const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto body = reply->readAll();
            const auto error = reply->error();
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
        if (state_ == State::standby || state_ == State::retry || state_ == State::locked) return;
        state_ = State::retry;
        deadline_.stop();
        ping_timer_.stop();
        if (media_) media_->StopCloudPeer();
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
            if (!ensureMedia() || !media_->StartCloudPeer(TravelMode(ice_, config_.relay_only))) {
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
    HostConfig media_config_;
    QByteArray token_;
    QJsonArray ice_;
    QNetworkAccessManager network_;
    QNetworkReply* login_reply_{};
    QWebSocket socket_;
    QTimer retry_, deadline_, ping_timer_;
    int retry_ms_{500};
    State state_{State::retry};
    std::unique_ptr<LinuxHostSession> media_;
};

#ifndef P2PREMOTE_VERSION
#define P2PREMOTE_VERSION "0.2.0"
#endif
#ifndef P2PREMOTE_UPDATE_FEED_URL
#define P2PREMOTE_UPDATE_FEED_URL ""
#endif
#ifndef P2PREMOTE_UPDATE_PUBLIC_KEY
#define P2PREMOTE_UPDATE_PUBLIC_KEY ""
#endif

// Unattended periodic auto-updater for headless Linux hosts (idle-only, zero-trust Ed25519).
class UnattendedHostUpdater final : public QObject {
public:
    explicit UnattendedHostUpdater(std::function<bool()> is_busy, QObject* parent = nullptr)
        : QObject(parent), is_busy_(std::move(is_busy)) {
        const std::string key = P2PREMOTE_UPDATE_PUBLIC_KEY;
        const std::string feed = P2PREMOTE_UPDATE_FEED_URL;
        if (key.empty() || feed.empty() || !p2premote::update::ParsePublicKeyBase64(key)) return;
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        timer_.setInterval(4 * 60 * 60 * 1000);
        connect(&timer_, &QTimer::timeout, this, &UnattendedHostUpdater::checkNow);
        QTimer::singleShot(30 * 1000, this, [this] {
            checkNow();
            timer_.start();
        });
    }

private:
    void checkNow() {
        if (busy_ || (!pending_install_.isEmpty() && QFile::exists(pending_install_)) ||
            !IsUnattendedModeEnabled() || (is_busy_ && is_busy_())) return;
        const QUrl url(QString::fromUtf8(P2PREMOTE_UPDATE_FEED_URL));
        if (!url.isValid() || (url.scheme() != QStringLiteral("https") &&
            !(url.scheme() == QStringLiteral("http") && QHostAddress(url.host()).isLoopback()))) {
            return;
        }
        busy_ = true;
        QNetworkRequest req(url);
        req.setTransferTimeout(30000);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("User-Agent", "p2premote-updater");
        req.setRawHeader("Cache-Control", "no-cache");
        auto* reply = network_.get(req);
        connect(reply, &QNetworkReply::readyRead, reply, [reply] {
            if (reply->bytesAvailable() > static_cast<qint64>(p2premote::update::kMaxManifestBytes)) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError || code != 200 ||
                body.size() > static_cast<qsizetype>(p2premote::update::kMaxManifestBytes)) {
                busy_ = false;
                return;
            }
            const auto parsed = p2premote::update::ParseUpdateManifest(
                std::string_view(body.constData(), static_cast<std::size_t>(body.size())));
            if (!parsed || !p2premote::update::VerifyUpdateManifest(*parsed, "linux-x64", P2PREMOTE_UPDATE_PUBLIC_KEY) ||
                p2premote::update::CompareVersions(parsed->version, P2PREMOTE_VERSION) <= 0) {
                busy_ = false;
                return;
            }
            downloadAndApply(*parsed);
        });
    }

    void downloadAndApply(p2premote::update::UpdateManifest manifest) {
        const QUrl url(QString::fromStdString(manifest.url));
        if (!url.isValid() || (url.scheme() != QStringLiteral("https") &&
            !(url.scheme() == QStringLiteral("http") && QHostAddress(url.host()).isLoopback()))) {
            busy_ = false;
            return;
        }
        QNetworkRequest req(url);
        req.setTransferTimeout(60000);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("User-Agent", "p2premote-updater");
        auto work = std::make_shared<QTemporaryDir>(QStringLiteral("/tmp/p2premote-auto-XXXXXX"));
        if (!work->isValid()) {
            busy_ = false;
            return;
        }
        const auto archive_path = std::filesystem::path(work->filePath(QStringLiteral("update.tar.gz")).toStdString());
        const auto manifest_path = std::filesystem::path(work->filePath(QStringLiteral("manifest.json")).toStdString());
        auto file = std::make_shared<QFile>(QString::fromUtf8(archive_path.c_str()));
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            busy_ = false;
            return;
        }
        auto* reply = network_.get(req);
        auto written = std::make_shared<std::int64_t>(0);
        connect(reply, &QNetworkReply::readyRead, this, [reply, file, written, limit = manifest.size] {
            const QByteArray chunk = reply->readAll();
            *written += chunk.size();
            if (*written > limit || file->write(chunk) != chunk.size()) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, file, work, manifest, archive_path, manifest_path] {
            reply->deleteLater();
            file->flush();
            file->close();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            busy_ = false;
            if (reply->error() != QNetworkReply::NoError || code != 200 ||
                !IsUnattendedModeEnabled() || (is_busy_ && is_busy_())) {
                return;
            }
            if (!p2premote::update::VerifyArchiveFile(archive_path, manifest.size, manifest.signature,
                                                      P2PREMOTE_UPDATE_PUBLIC_KEY)) {
                return;
            }
            {
                std::ofstream mf(manifest_path, std::ios::binary | std::ios::trunc);
                if (!mf || !(mf << p2premote::update::SerializeUpdateManifest(manifest))) {
                    return;
                }
            }
            // The transient systemd service survives stopping the host's cgroup.
            std::error_code ec;
            const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (ec || self != "/usr/local/libexec/p2premote/p2premote_linux_host") return;
            QProcess runner;
            runner.start(QStringLiteral("/usr/bin/systemd-run"),
                         {QStringLiteral("--quiet"), QStringLiteral("--collect"), QStringLiteral("--service-type=exec"),
                          QString::fromStdString(self.string()), QStringLiteral("--apply-downloaded-update"), work->path()});
            if (runner.waitForFinished(10000) && runner.exitStatus() == QProcess::NormalExit && runner.exitCode() == 0) {
                work->setAutoRemove(false);
                pending_install_ = work->path();
            }
        });
    }

    QNetworkAccessManager network_;
    QTimer timer_;
    std::function<bool()> is_busy_;
    QString pending_install_;
    bool busy_{false};
};

} // namespace

bool ValidCloudHostConfig(std::string_view data) { return ParseConfig(data).has_value(); }

int RunSystemService(int argc, char** argv, HostConfig lan_base) {
    if (const char* logs = std::getenv("LOGS_DIRECTORY"); logs && *logs) {
        lan_base.log_dir = logs;
    } else {
        char temp_logs[] = "/tmp/p2premote-minirtc.XXXXXX";
        if (mkdtemp(temp_logs)) lan_base.log_dir = temp_logs;
        else {
            std::cerr << "[host] Could not create a private MiniRTC log directory\n";
            return 1;
        }
    }

    HostConfig lan_cfg = lan_base;
    if (!LoadServiceConfig(lan_cfg)) {
        std::cerr << "[host] Could not read the protected P2P Remote LAN configuration\n";
        return 1;
    }
    const bool lan_configured = lan_cfg.access_code.size() >= 6 && lan_cfg.access_code.size() <= 32;

    std::optional<std::string> saved_cloud;
    if (!LoadCloudServiceConfig(saved_cloud)) {
        std::cerr << "[host] Could not read the protected P2P Remote cloud configuration\n";
        return 1;
    }
    std::optional<CloudConfig> cloud_cfg;
    if (saved_cloud) {
        cloud_cfg = ParseConfig(*saved_cloud);
        if (!cloud_cfg) {
            std::cerr << "[host] Invalid cloud service configuration\n";
            return 1;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });

    QCoreApplication app(argc, argv);

    std::unique_ptr<LinuxHostSession> lan_session;
    auto start_lan = [&]() -> bool {
        auto next = std::make_unique<LinuxHostSession>(lan_cfg, std::make_unique<X11ShmCapturer>());
        if (!next->Valid()) return false;
        lan_session = std::move(next);
        return true;
    };

    std::unique_ptr<CloudHost> cloud_service;
    if (cloud_cfg) {
        HostConfig media = lan_base;
        media.access_code.clear();
        cloud_service = std::make_unique<CloudHost>(std::move(*cloud_cfg), std::move(media));
    }

    const auto should_run = []() noexcept { return HostReceptionAllowed(); };
    const bool initial_active = should_run();

    if (lan_configured) {
        if (initial_active) {
            if (start_lan()) {
                NotifyHostState("waiting", lan_cfg.port, false);
            } else if (!cloud_service) {
                std::cerr << "[host] Failed to initialize Linux LAN host session\n";
                return 1;
            } else {
                std::cerr << "[host] Failed to initialize Linux LAN host session; continuing with cloud channel\n";
                NotifyHostState("error", lan_cfg.port, false);
            }
        } else {
            NotifyHostState("standby", lan_cfg.port, false);
        }
    } else {
        NotifyHostState("off", 0, false);
    }

    if (cloud_service) {
        cloud_service->start(initial_active);
    } else {
        NotifyCloudState("off", "-", false);
    }
    NotifyServiceReady();

    std::cout << "[host] P2P Remote Linux Host service running (lan="
              << (lan_configured ? "enabled" : "off")
              << ", cloud=" << (cloud_service ? "enabled" : "off") << ")\n" << std::flush;

    UnattendedHostUpdater unattended_updater([&] {
        return (lan_session && lan_session->PeerActive()) || (cloud_service && cloud_service->inSession());
    });

    QTimer poll_timer;
    QObject::connect(&poll_timer, &QTimer::timeout, &app, [&] {
        if (stopping) {
            lan_session.reset();
            cloud_service.reset();
            app.quit();
            return;
        }
        const bool active = should_run();
        if (lan_configured) {
            if (active && !lan_session) {
                if (start_lan()) NotifyHostState("waiting", lan_cfg.port, false);
            } else if (!active && lan_session) {
                lan_session.reset();
                NotifyHostState("standby", lan_cfg.port, false);
            }
        }
        if (cloud_service) {
            cloud_service->setActive(active);
        }
    });
    poll_timer.start(250);
    return app.exec();
}

} // namespace p2premote::linux_host
