#include "cloud_connection.h"

#include <p2premote/core_api.h>
#include <p2premote/service_identity.h>

#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslKey>
#include <QStandardPaths>
#include <QSysInfo>

#include <openssl/rand.h>

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace {

thread_local std::wstring last_error;
constexpr qsizetype max_response_size = 4 * 1024 * 1024;

struct Device {
    std::wstring id, name, platform, state;
};

bool isBundledService(const QUrl& url) {
    const QUrl bundled(QString::fromWCharArray(p2premote::wuhan_service_address));
    return url.host() == bundled.host() && url.port() == bundled.port();
}

bool validIdentifier(QStringView value, qsizetype length) {
    if (value.size() != length) return false;
    return std::ranges::all_of(value, [](QChar c) {
        const auto n = c.unicode();
        return (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
            (n >= '0' && n <= '9') || n == '_' || n == '-';
    });
}

QString installationId() {
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir)) return {};
    const auto path = QDir(dir).filePath(QStringLiteral("installation-id"));
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(150)) return {};
    QFile saved(path);
    if (saved.open(QIODevice::ReadOnly)) {
        const auto id = QString::fromLatin1(saved.read(44));
        if (validIdentifier(id, 43)) return id;
    }
    QByteArray bytes(32, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()) != 1) return {};
    const auto id = bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(id) != id.size() || !file.commit()) return {};
    return QString::fromLatin1(id);
}

bool parseIceServers(const QJsonArray& servers) {
    if (servers.isEmpty() || servers.size() > 32) return false;
    for (const auto& entry : servers) {
        const auto server = entry.toObject();
        const auto value = server.value("urls");
        const auto urls = value.isString() ? QJsonArray{value} : value.toArray();
        if (urls.isEmpty() || urls.size() > 32) return false;
        for (const auto& item : urls) {
            const auto url = item.toString();
            const auto prefix = url.section(u':', 0, 0);
            if (url.size() <= prefix.size() + 1 || url.contains(u' ') || url.contains(u'\n') ||
                url.contains(u'\r') || url.contains(u'\t')) return false;
            if (prefix == "turn" || prefix == "turns") {
                if (server.value("username").toString().isEmpty() || server.value("credential").toString().isEmpty()) {
                    return false;
                }
            } else if (prefix != "stun") {
                return false;
            }
        }
    }
    return true;
}

int badRequest(const QString& message) {
    setCoreError(message);
    return P2P_ERROR;
}

} // namespace

struct p2p_account {
    enum class Operation { none, login, refresh, remove, logout };
    QNetworkAccessManager network;
    QNetworkReply* reply{};
    QByteArray body;
    bool invalid_body{};
    Operation operation{Operation::none};
    QUrl origin;
    bool relay_only{};
    std::optional<CloudConnection> connection;
    std::vector<Device> devices;

    ~p2p_account() { if (reply) consumeReply(); }

    int request(Operation next, const QString& path, const char* method, const QByteArray& payload = {}) {
        if (reply) return badRequest(QStringLiteral("已有账号请求正在处理"));
        auto url = origin;
        url.setPath(path);
        auto options = cloudRequest(url, connection ? connection->token : QByteArray{});
        if (QByteArrayView(method) == "POST") {
            options.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
        }
        body.clear();
        invalid_body = false;
        operation = next;
        reply = network.sendCustomRequest(options, method, payload);
        reply->setReadBufferSize(max_response_size);
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [this] {
            body.append(reply->readAll());
            if (body.size() > max_response_size) {
                invalid_body = true;
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::encrypted, reply, [this] {
            if (!cloudPeerMatches(reply->url(), reply->sslConfiguration().peerCertificate())) reply->abort();
        });
        return P2P_OK;
    }

    void consumeReply() {
        auto* previous = std::exchange(reply, nullptr);
        previous->disconnect();
        if (!previous->isFinished()) previous->abort();
        previous->deleteLater();
    }
};

void setCoreError(const QString& message) { last_error = message.toStdWString(); }

QNetworkRequest cloudRequest(const QUrl& url, const QByteArray& token) {
    QNetworkRequest request(url);
    request.setTransferTimeout(12000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    if (url.scheme() == "https" || url.scheme() == "wss") {
        auto tls = QSslConfiguration::defaultConfiguration();
        tls.setProtocol(QSsl::TlsV1_2OrLater);
        if (isBundledService(url)) {
            tls.setCaCertificates(QSslCertificate::fromData(p2premote::wuhan_ca_certificate));
        }
        request.setSslConfiguration(tls);
    }
    return request;
}

bool cloudPeerMatches(const QUrl& url, const QSslCertificate& peer) {
    if (!isBundledService(url)) return true;
    const auto hash = QCryptographicHash::hash(peer.publicKey().toDer(), QCryptographicHash::Sha256).toBase64();
    return "sha256//" + hash == p2premote::wuhan_public_key_pin;
}

const CloudConnection* cloudConnection(const p2p_account* account) {
    return account && account->connection ? &*account->connection : nullptr;
}

uint32_t p2p_core_abi_version() { return P2P_CORE_ABI_VERSION; }
const wchar_t* p2p_last_error() { return last_error.c_str(); }
const wchar_t* p2p_default_server() { return p2premote::wuhan_service_address; }

p2p_account* p2p_account_create() { return new p2p_account; }
void p2p_account_destroy(p2p_account* account) { delete account; }

int32_t p2p_account_login(p2p_account* account, const wchar_t* server, const wchar_t* user,
    const wchar_t* password, int32_t create, int32_t, int32_t relay_only) {
    if (!account || account->reply || account->connection || !server || !user || !password) {
        return badRequest(QStringLiteral("无法开始登录"));
    }
    auto origin = QUrl(QString::fromWCharArray(server).trimmed(), QUrl::StrictMode);
    const bool loopback = origin.scheme() == "http" && origin.host() == "127.0.0.1";
    if (!origin.isValid() || (origin.scheme() != "https" && !loopback) || origin.host().isEmpty() ||
        (!origin.path().isEmpty() && origin.path() != "/") || !origin.userInfo().isEmpty() ||
        origin.hasQuery() || origin.hasFragment()) {
        return badRequest(QStringLiteral("请填写 HTTPS 服务根地址。本机测试可使用 http://127.0.0.1:8788"));
    }
    const auto installation = installationId();
    if (installation.isEmpty()) return badRequest(QStringLiteral("无法保存客户端标识"));
    const QJsonObject payload{{"username", QString::fromWCharArray(user)},
        {"password", QString::fromWCharArray(password)}, {"installationId", installation},
        {"deviceName", QSysInfo::machineHostName()}, {"platform", QStringLiteral("Linux x64")}};
    account->origin = std::move(origin);
    account->relay_only = relay_only != 0;
    return account->request(p2p_account::Operation::login, create ? "/api/v2/register" : "/api/v2/login",
        "POST", QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

int32_t p2p_account_refresh(p2p_account* account) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    return account->request(p2p_account::Operation::refresh, "/api/v2/devices", "GET");
}

int32_t p2p_account_remove_device(p2p_account* account, const wchar_t* device_id) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    const auto id = device_id ? QString::fromWCharArray(device_id) : QString{};
    if (!validIdentifier(id, 24)) return badRequest(QStringLiteral("设备编号无效"));
    return account->request(p2p_account::Operation::remove, QStringLiteral("/api/v2/devices/") + id, "DELETE");
}

int32_t p2p_account_logout(p2p_account* account) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    const auto result = account->request(p2p_account::Operation::logout, "/api/v2/logout", "POST");
    if (result == P2P_OK) {
        account->connection.reset();
        account->devices.clear();
    }
    return result;
}

void p2p_account_cancel(p2p_account* account) {
    if (account && account->reply) {
        account->consumeReply();
        account->operation = p2p_account::Operation::none;
    }
}

int32_t p2p_account_complete(p2p_account* account) {
    if (!account || !account->reply || !account->reply->isFinished()) return P2P_PENDING;
    const auto operation = std::exchange(account->operation, p2p_account::Operation::none);
    const auto response = account->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto network_error = account->reply->error();
    const auto error_text = account->reply->errorString();
    const auto json = QJsonDocument::fromJson(std::exchange(account->body, {})).object();
    account->consumeReply();
    if (response == 401 && operation != p2p_account::Operation::login) {
        account->connection.reset();
        account->devices.clear();
        setCoreError(QStringLiteral("登录已失效，请重新登录"));
        return P2P_LOGIN_EXPIRED;
    }
    if (network_error != QNetworkReply::NoError || response < 200 || response >= 300 || account->invalid_body) {
        const auto message = json.value("error").toString();
        return badRequest(message.isEmpty() ? QStringLiteral("账号服务请求失败 · %1").arg(error_text) : message);
    }
    if (operation == p2p_account::Operation::login) {
        const auto access_token = json.value("accessToken").toString();
        const auto servers = json.value("iceServers").toArray();
        if (!validIdentifier(access_token, 43) || !parseIceServers(servers)) {
            return badRequest(QStringLiteral("账号服务返回了无效的连接配置"));
        }
        auto signal_url = account->origin;
        signal_url.setScheme(signal_url.scheme() == "https" ? "wss" : "ws");
        signal_url.setPath("/signal");
        account->connection = CloudConnection{signal_url, access_token.toLatin1(), servers, account->relay_only};
    } else if (operation == p2p_account::Operation::refresh) {
        if (!json.value("devices").isArray()) return badRequest(QStringLiteral("账号服务返回了无效的设备列表"));
        std::vector<Device> devices;
        for (const auto& entry : json.value("devices").toArray()) {
            const auto item = entry.toObject();
            const auto id = item.value("id").toString();
            if (!validIdentifier(id, 24)) continue;
            devices.push_back({id.toStdWString(), item.value("name").toString().toStdWString(),
                item.value("platform").toString().toStdWString(), item.value("state").toString("offline").toStdWString()});
        }
        std::ranges::sort(devices, [](const Device& a, const Device& b) {
            return std::tie(a.name, a.id) < std::tie(b.name, b.id);
        });
        account->devices = std::move(devices);
    }
    return P2P_OK;
}

int32_t p2p_account_signed_in(const p2p_account* account) { return cloudConnection(account) != nullptr; }
const wchar_t* p2p_account_device_id(const p2p_account*) { return nullptr; }
size_t p2p_account_device_count(const p2p_account* account) { return account ? account->devices.size() : 0; }

int32_t p2p_account_device(const p2p_account* account, size_t index, p2p_device* device) {
    if (!account || !device || index >= account->devices.size()) return P2P_ERROR;
    const auto& item = account->devices[index];
    *device = {item.id.c_str(), item.name.c_str(), item.platform.c_str(), item.state.c_str()};
    return P2P_OK;
}
