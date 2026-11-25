#include "agent_broker.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

#include <iostream>
#include <stdexcept>

struct p2p_account {};
struct p2p_stream {};

namespace {

bool response_ready = true;
bool in_flight{};
bool cancelled{};
int requests{};
int last_tool{};

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct CommandResult {
    int code;
    QByteArray output;
    QByteArray error;
};

CommandResult run(const QString& program, const QStringList& arguments, const QByteArray& input = {},
    const QByteArray& cancellation = {}) {
    QProcess process;
    QEventLoop loop;
    QTimer timeout;
    QTimer cancel_timer;
    timeout.setSingleShot(true);
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::started, &process, [&] {
        process.write(input);
        if (cancellation.isEmpty()) process.closeWriteChannel();
        else cancel_timer.start(10);
    });
    QObject::connect(&cancel_timer, &QTimer::timeout, &process, [&] {
        if (in_flight && !cancel_timer.property("sent").toBool()) {
            process.write(cancellation);
            cancel_timer.setProperty("sent", true);
        }
        if (cancelled) process.closeWriteChannel();
    });
    timeout.start(5'000);
    process.start(program, arguments);
    if (process.state() != QProcess::NotRunning) loop.exec();
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("Local tool did not finish");
    }
    check(process.error() != QProcess::FailedToStart && process.exitStatus() == QProcess::NormalExit,
        "Could not run the local tool");
    return {process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
}

} // namespace

int32_t p2p_account_signed_in(const p2p_account*) { return 1; }
size_t p2p_account_device_count(const p2p_account*) { return 2; }
int32_t p2p_account_device(const p2p_account*, size_t index, p2p_device* device) {
    *device = {index ? L"pc-2" : L"pc-1", L"Test device", L"windows", L"online"};
    return P2P_OK;
}

uint32_t p2p_stream_request_agent_summary(p2p_stream*, int32_t tool) {
    if (in_flight) return 0;
    in_flight = true;
    ++requests;
    last_tool = tool;
    return 1;
}

int32_t p2p_stream_take_agent_summary(p2p_stream*, uint32_t, p2p_agent_summary* result) {
    if (!response_ready) return P2P_PENDING;
    *result = {"ok", R"({"schema":1,"architecture":"x64","windows_major":10,"windows_minor":0,"windows_build":26100,"logical_processors":8,"memory_total_mib":8192,"memory_available_mib":4096,"uptime_seconds":100,"system_disk_total_mib":102400,"system_disk_free_mib":50000,"not_allowed":"ignored"})", nullptr, 0};
    if (last_tool == P2P_AGENT_SCREEN_SNAPSHOT) {
        static const auto png = QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
        *result = {"ok", nullptr, png.constData(), static_cast<std::size_t>(png.size())};
    }
    in_flight = false;
    return P2P_OK;
}

int32_t p2p_stream_cancel_agent_summary(p2p_stream*, uint32_t id) {
    cancelled = in_flight && id == 1;
    in_flight = false;
    return P2P_OK;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    try {
        const auto cli = app.arguments().at(1);
        const auto mcp = app.arguments().at(2);
        p2p_account account;
        p2p_stream stream;
        AgentBroker broker;
        check(broker.start(&account, QStringLiteral("pc-1"), [&] { return &stream; }), "Could not start broker");
        const auto token = broker.token();
        const auto devices = run(cli, {QStringLiteral("devices"), QStringLiteral("--live"), token});
        if (devices.code) std::cerr << devices.error.constData();
        const auto list = QJsonDocument::fromJson(devices.output).object().value(QStringLiteral("devices")).toArray();
        check(!devices.code && list.size() == 2 && list.at(0).toObject().value(QStringLiteral("shared")).toBool() &&
            !list.at(1).toObject().value(QStringLiteral("shared")).toBool(), "Device list did not identify the shared session");

        const QStringList args{QStringLiteral("system-info"), QStringLiteral("--live"), token, QStringLiteral("pc-1")};
        auto other = args;
        other.last() = QStringLiteral("pc-2");
        check(run(cli, other).code == 1 && requests == 0, "Tool queried a device outside the shared session");
        const auto summary = run(cli, args);
        if (summary.code) std::cerr << summary.error.constData();
        const auto parsed = QJsonDocument::fromJson(summary.output).object();
        check(!summary.code && requests == 1 && parsed.value(QStringLiteral("system_disk_free_mib")).toInt() == 50000 &&
            !parsed.contains(QStringLiteral("not_allowed")), "CLI did not return a filtered approved summary");

        const QByteArray initialize =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\"}}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
        const QByteArray diagnose =
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"system_info\","
            "\"arguments\":{\"device_id\":\"pc-1\"}}}\n";
        const auto tools = run(mcp, {QStringLiteral("--live"), token}, initialize +
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_devices\"}}\n" + diagnose);
        check(!tools.code && requests == 2 && tools.output.contains("\"structuredContent\"") &&
            tools.output.contains("\"list_devices\"") && tools.output.contains("\"windows_build\":26100") &&
            !tools.output.contains("\"name\":\"screen_snapshot\""),
            "MCP did not query devices and diagnostics using live sharing");

        response_ready = false;
        const auto cancelled_mcp = run(mcp, {QStringLiteral("--live"), token}, initialize + diagnose,
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":4}}\n");
        check(!cancelled_mcp.code && requests == 3 && cancelled && !cancelled_mcp.output.contains("\"id\":4"),
            "MCP cancellation did not reach the pending query");

        cancelled = false;
        QTimer revoke;
        revoke.setInterval(20);
        QObject::connect(&revoke, &QTimer::timeout, [&] { if (in_flight) broker.stop(); });
        revoke.start();
        check(run(cli, args).code == 1 && requests == 4 && cancelled && !broker.active(),
            "Stopping sharing did not cancel the pending query");
        check(run(cli, args).code == 1 && requests == 4, "Sharing code still worked after revocation");

        revoke.stop();
        response_ready = true;
        check(broker.start(&account, QStringLiteral("pc-1"), [&] { return &stream; }, true,
            {{QStringLiteral("plugin_office_capacity"), {QStringLiteral("system_disk_free_mib")}},
             {QStringLiteral("plugin_invalid_report"), {QStringLiteral("host_user")}}}), "Could not share read-only tools");
        const QStringList live_args{QStringLiteral("--live"), broker.token()};
        const auto screen = run(mcp, live_args, initialize +
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"screen_snapshot\","
            "\"arguments\":{\"device_id\":\"pc-1\"}}}\n");
        check(!screen.code && requests == 5 && last_tool == P2P_AGENT_SCREEN_SNAPSHOT &&
            screen.output.contains("image/png") && screen.output.contains("iVBORw0KGgo") &&
            screen.output.contains("plugin_office_capacity") && !screen.output.contains("plugin_invalid_report"),
            "MCP did not list the permitted plugin or receive a PNG image");
        const auto report = run(mcp, live_args, initialize +
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"plugin_office_capacity\","
            "\"arguments\":{\"device_id\":\"pc-1\"}}}\n");
        check(!report.code && requests == 6 && last_tool == P2P_AGENT_SYSTEM_INFO &&
            report.output.contains("\"system_disk_free_mib\":50000") && !report.output.contains("windows_build"),
            "Plugin report did not restrict the approved summary to its registered fields");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
