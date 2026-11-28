#include <p2premote/core_api_v2.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstring>
#include <iostream>
#include <memory>

namespace {
bool check(bool passed, const char* description) {
    if (!passed) std::cerr << description << '\n';
    return passed;
}
}

int main(int argc, char** argv) {
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("p2premote-core-test");

    std::unique_ptr<p2p_account, decltype(&p2p_account_destroy)> account(p2p_account_create(), p2p_account_destroy);
    bool passed = check(account && p2p_default_server_utf8()[0], "Could not initialize the C ABI");
    passed &= check(p2p_account_login_v2(account.get(), "https://localhost", "\xC0\xAF", "test", 0, 0, 0) == P2P_ERROR &&
        *p2p_last_error_utf8() && p2p_account_poll_events(account.get()) == P2P_PENDING,
        "Invalid UTF-8 credentials entered the request queue");

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return 1;
    bool received_unicode{};
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request = QByteArray{}]() mutable {
            request += socket->readAll();
            const auto end = request.indexOf("\r\n\r\n");
            if (end < 0) return;
            const auto fields = QJsonDocument::fromJson(request.mid(end + 4)).object();
            if (fields.isEmpty()) return;
            received_unicode = request.startsWith("POST /api/v2/login ") &&
                fields.value("username").toString() == QStringLiteral("测试用户") &&
                fields.value("password").toString() == QStringLiteral("测试😀");
            const auto body = QByteArray("{\"accessToken\":\"") + QByteArray(43, 'a') +
                "\",\"currentDeviceId\":\"\",\"iceServers\":[{\"urls\":\"stun:127.0.0.1:3478\"}]}";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    const auto url = QByteArray("http://127.0.0.1:") + QByteArray::number(server.serverPort());
    if (!check(p2p_account_login_v2(account.get(), url.constData(), "测试用户", "测试😀", 0, 0, 0) == P2P_OK,
            "Could not submit a loopback login")) return 1;
    QEventLoop loop;
    QTimer timer;
    int32_t result = P2P_PENDING;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        result = p2p_account_poll_events(account.get());
        if (result != P2P_PENDING) loop.quit();
    });
    timer.start(10);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    passed &= check(result == P2P_OK && received_unicode && p2p_account_signed_in(account.get()) &&
        p2p_account_poll_events(account.get()) == P2P_PENDING, "Loopback credentials or account events did not round-trip");

    const p2p_lan_options_v2 lan{"127.0.0.1", P2P_LAN_DEFAULT_PORT, "test-code", 0, nullptr, nullptr};
    const p2p_render_target_v2 unsupported{P2P_RENDER_TARGET_WIN32_WINDOW, 1, nullptr, nullptr};
    passed &= check(!p2p_stream_create_lan_v2(0, &unsupported, &lan), "Accepted a renderer from a different platform");
    std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream(
        p2p_stream_create_lan_v2(0, nullptr, &lan), p2p_stream_destroy);
    p2p_stream_snapshot_v2 snapshot{};
    p2p_stream_events_v2 events{};
    passed &= check(stream && p2p_stream_poll_events(stream.get(), &events) == P2P_OK && events.ready == 0,
        "Idle stream polled an unexpected event");
    passed &= check(stream && p2p_stream_stop_v2(stream.get(), "关闭连接 😀") == P2P_OK &&
        p2p_stream_poll_events(stream.get(), &events) == P2P_OK && (events.ready & P2P_STREAM_EVENT_SESSION) &&
        p2p_stream_snapshot_v2_read(stream.get(), &snapshot) == P2P_OK && !std::strcmp(snapshot.status, "关闭连接 😀"),
        "Stream events or UTF-8 status did not round-trip");
    return passed ? 0 : 1;
}
