#include "agent_broker.h"

#include <windows.h>

#include "../../agent/snapshot.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

constexpr qsizetype request_limit = 4096;
constexpr qsizetype response_limit = 768 * 1024;

QString text(const wchar_t* value) { return value ? QString::fromWCharArray(value) : QString{}; }

QJsonObject failure(const QString& status) { return {{QStringLiteral("status"), status}}; }

bool localClient(QLocalSocket* socket) {
    // QLocalServer creates Windows pipes with PIPE_REJECT_REMOTE_CLIENTS.
    const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
    DWORD own_session{}, client_session{};
    return ProcessIdToSessionId(GetCurrentProcessId(), &own_session) &&
        GetNamedPipeClientSessionId(pipe, &client_session) && client_session == own_session;
}

} // namespace

AgentBroker::AgentBroker(QObject* parent) : QObject(parent) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    server_.setMaxPendingConnections(8);
    timer_.setInterval(100);
    connect(&server_, &QLocalServer::newConnection, this, &AgentBroker::acceptClients);
    connect(&timer_, &QTimer::timeout, this, &AgentBroker::poll);
}

AgentBroker::~AgentBroker() { stop(); }

bool AgentBroker::start(p2p_account* account, QString device, Session session, bool screenshots,
    QMap<QString, QStringList> reports) {
    stop();
    if (!account || !p2p_account_signed_in(account) || device.isEmpty() || !session()) return false;
    token_ = QUuid::createUuid().toString(QUuid::Id128);
    if (!server_.listen(QStringLiteral("p2premote-diagnostics-") + token_)) {
        token_.clear();
        return false;
    }
    account_ = account;
    device_ = std::move(device);
    session_ = std::move(session);
    screenshots_ = screenshots;
    static const QStringList permitted{QStringLiteral("architecture"), QStringLiteral("windows_major"),
        QStringLiteral("windows_minor"), QStringLiteral("windows_build"), QStringLiteral("logical_processors"),
        QStringLiteral("memory_total_mib"), QStringLiteral("memory_available_mib"), QStringLiteral("uptime_seconds"),
        QStringLiteral("system_disk_total_mib"), QStringLiteral("system_disk_free_mib")};
    for (auto it = reports.cbegin(); it != reports.cend() && reports_.size() < 32; ++it) {
        if (!it.value().isEmpty() && std::ranges::all_of(it.value(), [&](const auto& field) {
            return permitted.contains(field);
        })) reports_.insert(it.key(), it.value());
    }
    timer_.start();
    return true;
}

void AgentBroker::cancel() {
    if (pending_ && session_) {
        if (auto* stream = session_()) p2p_stream_cancel_agent_summary(stream, pending_);
    }
    pending_ = 0;
    pending_fields_.clear();
    waiting_.clear();
}

void AgentBroker::stop() {
    cancel();
    timer_.stop();
    server_.close();
    const auto clients = std::exchange(clients_, {});
    for (auto* socket : clients) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    session_ = {};
    account_ = nullptr;
    token_.clear();
    device_.clear();
    reports_.clear();
}

void AgentBroker::acceptClients() {
    while (auto* socket = server_.nextPendingConnection()) {
        socket->setParent(this);
        if (clients_.size() >= 8 || !localClient(socket)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        clients_.insert(socket);
        socket->setReadBufferSize(request_limit + 1);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (waiting_ == socket) cancel();
            clients_.remove(socket);
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readRequest(socket); });
        QTimer::singleShot(2'000, socket, [socket] {
            if (!socket->property("requestRead").toBool()) reply(socket, failure(QStringLiteral("request_timeout")));
        });
        readRequest(socket);
    }
}

void AgentBroker::reply(QLocalSocket* socket, const QJsonObject& result) {
    if (socket->property("replySent").toBool()) return;
    socket->setProperty("replySent", true);
    auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (bytes.size() >= response_limit) bytes = QByteArrayLiteral("{\"status\":\"response_too_large\"}");
    bytes.append('\n');
    if (socket->write(bytes) != bytes.size()) {
        socket->abort();
        return;
    }
    // DisconnectNamedPipe can discard data the client has not read yet.
    QTimer::singleShot(2'000, socket, &QLocalSocket::abort);
}

void AgentBroker::readRequest(QLocalSocket* socket) {
    if (socket->property("requestRead").toBool() || socket->property("replySent").toBool()) return;
    if (!socket->canReadLine() && socket->bytesAvailable() <= request_limit) return;
    socket->setProperty("requestRead", true);
    const auto line = socket->readLine(request_limit + 1);
    const auto request = QJsonDocument::fromJson(line).object();
    const auto op = request.value(QStringLiteral("op")).toString();
    const bool screen = op == QStringLiteral("screen_snapshot");
    const auto report = op == QStringLiteral("system_report") ? reports_.constFind(request.value(QStringLiteral("report")).toString())
        : reports_.cend();
    if (!line.endsWith('\n') || !account_ || !p2p_account_signed_in(account_) || !session_ || !session_()) {
        reply(socket, failure(QStringLiteral("unavailable")));
        return;
    }
    if (op == QStringLiteral("list_devices") && request.size() == 1) {
        QJsonArray devices;
        const auto count = p2p_account_device_count(account_);
        for (std::size_t i = 0; i < std::min<std::size_t>(count, 128); ++i) {
            p2p_device device{};
            if (p2p_account_device(account_, i, &device) != P2P_OK) continue;
            const auto id = text(device.id);
            devices.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), text(device.name).left(128)},
                {QStringLiteral("state"), text(device.state).left(32)}, {QStringLiteral("shared"), id == device_}});
        }
        QJsonArray reports;
        for (auto it = reports_.cbegin(); it != reports_.cend(); ++it) {
            reports.append(QJsonObject{{QStringLiteral("name"), it.key()},
                {QStringLiteral("fields"), QJsonArray::fromStringList(it.value())}});
        }
        reply(socket, {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("devices"), devices},
            {QStringLiteral("reports"), reports},
            {QStringLiteral("screen_snapshot"), screenshots_},
            {QStringLiteral("shared_device_id"), device_}, {QStringLiteral("truncated"), count > 128}});
    } else if (screen && !screenshots_ && request.size() == 2 &&
        request.value(QStringLiteral("device_id")).toString() == device_) {
        reply(socket, failure(QStringLiteral("requires_diagnostics")));
    } else if ((((op == QStringLiteral("system_info") || (screen && screenshots_)) && request.size() == 2) ||
        (report != reports_.cend() && request.size() == 3)) &&
        request.value(QStringLiteral("device_id")).toString() == device_) {
        if (pending_) {
            reply(socket, failure(QStringLiteral("busy")));
            return;
        }
        pending_ = p2p_stream_request_agent_summary(session_(), screen ? P2P_AGENT_SCREEN_SNAPSHOT : P2P_AGENT_SYSTEM_INFO);
        if (!pending_) {
            reply(socket, failure(QStringLiteral("unavailable")));
            return;
        }
        waiting_ = socket;
        pending_fields_ = report == reports_.cend() ? QStringList{} : report.value();
        deadline_ = static_cast<qint64>(GetTickCount64()) + 21'000;
    } else {
        reply(socket, failure(QStringLiteral("not_shared")));
    }
}

void AgentBroker::poll() {
    auto* stream = session_ ? session_() : nullptr;
    if (!stream || !account_ || !p2p_account_signed_in(account_)) {
        stop();
        return;
    }
    if (!pending_ || !waiting_) return;
    auto* socket = waiting_.data();
    if (static_cast<qint64>(GetTickCount64()) >= deadline_) {
        cancel();
        reply(socket, failure(QStringLiteral("expired")));
        return;
    }
    p2p_agent_summary summary{};
    const auto result = p2p_stream_take_agent_summary(stream, pending_, &summary);
    if (result == P2P_PENDING) return;
    if (result != P2P_OK) {
        cancel();
        reply(socket, failure(QStringLiteral("unavailable")));
        return;
    }
    pending_ = 0;
    waiting_.clear();
    const auto fields = std::exchange(pending_fields_, {});
    const auto status = QString::fromUtf8(summary.status);
    if (status != QStringLiteral("ok")) {
        reply(socket, failure(status));
        return;
    }
    if (summary.image_size) {
        if (screenshots_ && summary.image_png && summary.image_size <= P2P_AGENT_IMAGE_LIMIT) {
            const auto png = QByteArray::fromRawData(summary.image_png, static_cast<qsizetype>(summary.image_size));
            reply(socket, {{QStringLiteral("status"), status},
                {QStringLiteral("image_png"), QString::fromLatin1(png.toBase64())}});
        } else reply(socket, failure(QStringLiteral("invalid_snapshot")));
        return;
    }
    try {
        const auto filtered = p2premote::agent::ParseDiagnosticSnapshot(summary.snapshot_json);
        auto object = QJsonDocument::fromJson(QByteArray::fromStdString(winrt::to_string(filtered.Stringify()))).object();
        if (!fields.isEmpty()) {
            QJsonObject report;
            for (const auto& field : fields) {
                const auto value = object.value(field);
                report.insert(field, value.isUndefined() ? QJsonValue(QJsonValue::Null) : value);
            }
            object = std::move(report);
        }
        reply(socket, {{QStringLiteral("status"), status}, {QStringLiteral("snapshot"), object}});
    } catch (...) {
        reply(socket, failure(QStringLiteral("invalid_snapshot")));
    }
}
