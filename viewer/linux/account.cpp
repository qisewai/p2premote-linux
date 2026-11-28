#include "cloud_connection.h"

#include <p2premote/core_api_v2.h>
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

thread_local QString last_error;
constexpr qsizetype max_response_size = 4 * 1024 * 1024;

struct Device {
    std::wstring id, name, platform, state;
};

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

int badRequest(const QString& message) {
    setCoreError(message);
    return P2P_ERROR;
}

} // namespace

struct p2p_account {
    enum class Operation { none, login, refresh, add, remove, logout };
    QNetworkAccessManager network;
    QNetworkReply* reply{};
    QByteArray body;
    bool invalid_body{};
    Operation operation{Operation::none};
    QUrl origin;
    bool relay_only{};
    std::optional<CloudConnection> connection;
    std::vector<Device> devices;
    std::wstring installation_id, device_id, removing_id;

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

void setCoreError(const QString& message) { last_error = message; }

const CloudConnection* cloudConnection(const p2p_account* account) {
    return account && account->connection ? &*account->connection : nullptr;
}

uint32_t p2p_core_abi_version() { return P2P_CORE_ABI_VERSION; }
const wchar_t* p2p_last_error() {
    thread_local std::wstring result;
    result = last_error.toStdWString();
    return result.c_str();
}
const char* p2p_last_error_utf8() {
    thread_local QByteArray result;
    result = last_error.toUtf8();
    return result.constData();
}
const char* p2p_default_server_utf8() {
    static const auto result = QString::fromWCharArray(p2p_default_server()).toUtf8();
    return result.constData();
}
const wchar_t* p2p_default_server() { return p2premote::wuhan_service_address; }

p2p_account* p2p_account_create() { return new p2p_account; }
void p2p_account_destroy(p2p_account* account) { delete account; }

namespace {

int32_t login(p2p_account* account, const QString& server, const QString& user, const QString& password,
    int32_t create, int32_t relay_only) {
    if (!account || account->reply || account->connection) {
        return badRequest(QStringLiteral("无法开始登录"));
    }
    auto origin = QUrl(server.trimmed(), QUrl::StrictMode);
    if (!isCloudOrigin(origin)) {
        return badRequest(QStringLiteral("请填写 HTTPS 服务根地址。本机测试可使用 http://127.0.0.1:8788"));
    }
    const auto installation = installationId();
    if (installation.isEmpty()) return badRequest(QStringLiteral("无法保存客户端标识"));
    const QJsonObject payload{{"username", user},
        {"password", password}, {"installationId", installation},
        {"deviceName", QSysInfo::machineHostName()}, {"platform", QStringLiteral("Linux x64")}};
    account->origin = std::move(origin);
    account->installation_id = installation.toStdWString();
    account->relay_only = relay_only != 0;
    return account->request(p2p_account::Operation::login, create ? "/api/v2/register" : "/api/v2/login",
        "POST", QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

} // namespace

int32_t p2p_account_login(p2p_account* account, const wchar_t* server, const wchar_t* user,
    const wchar_t* password, int32_t create, int32_t, int32_t relay_only) {
    if (!server || !user || !password) return badRequest(QStringLiteral("无法开始登录"));
    return login(account, QString::fromWCharArray(server), QString::fromWCharArray(user),
        QString::fromWCharArray(password), create, relay_only);
}

int32_t p2p_account_login_v2(p2p_account* account, const char* server, const char* user,
    const char* password, int32_t create, int32_t, int32_t relay_only) {
    if (!server || !user || !password) return badRequest(QStringLiteral("无法开始登录"));
    if (!QByteArrayView(server).isValidUtf8() || !QByteArrayView(user).isValidUtf8() ||
        !QByteArrayView(password).isValidUtf8()) {
        return badRequest(QStringLiteral("输入必须使用有效的 UTF-8"));
    }
    return login(account, QString::fromUtf8(server), QString::fromUtf8(user), QString::fromUtf8(password),
        create, relay_only);
}

int32_t p2p_account_add_local_device(p2p_account* account) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    return account->request(p2p_account::Operation::add, "/api/v2/devices/current", "POST", "{}");
}

int32_t p2p_account_refresh(p2p_account* account) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    return account->request(p2p_account::Operation::refresh, "/api/v2/devices", "GET");
}

int32_t p2p_account_remove_device(p2p_account* account, const wchar_t* device_id) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    const auto id = device_id ? QString::fromWCharArray(device_id) : QString{};
    if (!validIdentifier(id, 24)) return badRequest(QStringLiteral("设备编号无效"));
    const auto result = account->request(p2p_account::Operation::remove, QStringLiteral("/api/v2/devices/") + id, "DELETE");
    if (result == P2P_OK) account->removing_id = device_id;
    return result;
}

int32_t p2p_account_logout(p2p_account* account) {
    if (!cloudConnection(account)) return badRequest(QStringLiteral("请先登录"));
    const auto result = account->request(p2p_account::Operation::logout, "/api/v2/logout", "POST");
    if (result == P2P_OK) {
        account->connection.reset();
        account->devices.clear();
        account->device_id.clear();
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
    account->body.append(account->reply->readAll());
    const auto json = QJsonDocument::fromJson(std::exchange(account->body, {})).object();
    account->consumeReply();
    if (response == 401 && operation != p2p_account::Operation::login) {
        account->connection.reset();
        account->devices.clear();
        account->device_id.clear();
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
        const auto device_id = json.value("currentDeviceId").toString();
        if (!validIdentifier(access_token, 43) || !parseIceServers(servers) ||
            (!device_id.isEmpty() && !validIdentifier(device_id, 24))) {
            return badRequest(QStringLiteral("账号服务返回了无效的连接配置"));
        }
        auto signal_url = account->origin;
        signal_url.setScheme(signal_url.scheme() == "https" ? "wss" : "ws");
        signal_url.setPath("/signal");
        account->connection = CloudConnection{signal_url, access_token.toLatin1(), servers, account->relay_only};
        account->device_id = device_id.toStdWString();
    } else if (operation == p2p_account::Operation::refresh) {
        const auto device_id = json.value("currentDeviceId").toString();
        if (!json.value("devices").isArray() || (!device_id.isEmpty() && !validIdentifier(device_id, 24))) {
            return badRequest(QStringLiteral("账号服务返回了无效的设备列表"));
        }
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
        account->device_id = device_id.toStdWString();
    } else if (operation == p2p_account::Operation::add) {
        const auto id = json.value("id").toString();
        if (!validIdentifier(id, 24)) return badRequest(QStringLiteral("账号服务返回了无效的设备编号"));
        account->device_id = id.toStdWString();
    } else if (operation == p2p_account::Operation::remove) {
        if (account->device_id == account->removing_id) account->device_id.clear();
        std::erase_if(account->devices, [&](const Device& item) { return item.id == account->removing_id; });
        account->removing_id.clear();
    }
    return P2P_OK;
}

int32_t p2p_account_signed_in(const p2p_account* account) { return cloudConnection(account) != nullptr; }
const wchar_t* p2p_account_device_id(const p2p_account* account) {
    return cloudConnection(account) && !account->device_id.empty() ? account->device_id.c_str() : nullptr;
}
const wchar_t* p2p_account_installation_id(const p2p_account* account) {
    return cloudConnection(account) ? account->installation_id.c_str() : nullptr;
}
size_t p2p_account_device_count(const p2p_account* account) { return account ? account->devices.size() : 0; }

int32_t p2p_account_device(const p2p_account* account, size_t index, p2p_device* device) {
    if (!account || !device || index >= account->devices.size()) return P2P_ERROR;
    const auto& item = account->devices[index];
    *device = {item.id.c_str(), item.name.c_str(), item.platform.c_str(), item.state.c_str()};
    return P2P_OK;
}

int32_t p2p_account_poll_events(p2p_account* account) {
    return p2p_account_complete(account);
}
