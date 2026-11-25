#include "account_window.h"
#include "app_settings.h"
#include "app_theme.h"
#include "single_instance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QStyle>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("P2PRemote"));
    QCoreApplication::setApplicationName(QStringLiteral("Desktop"));
    QApplication::setApplicationDisplayName(QStringLiteral("P2P Remote"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("P2P Remote Qt 客户端"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption compatibility(QStringLiteral("compat-account"), QStringLiteral("使用兼容账号请求"));
    const QCommandLineOption ice(QStringLiteral("ice-trace"), QStringLiteral("记录 ICE 诊断"), QStringLiteral("目录"));
    const QCommandLineOption latency(QStringLiteral("latency-trace"), QStringLiteral("记录延迟诊断"), QStringLiteral("目录"));
    const QCommandLineOption stun(QStringLiteral("stun"), QStringLiteral("覆盖 STUN 地址"), QStringLiteral("stun:主机:端口"));
    const QCommandLineOption stun_only(QStringLiteral("stun-only"), QStringLiteral("仅使用直连"));
    const QCommandLineOption relay_only(QStringLiteral("relay-only"), QStringLiteral("仅使用 TURN 中继"));
    const QCommandLineOption lan_host(QStringLiteral("lan-host"), QStringLiteral("启动局域网无人值守被控模式"));
    const QCommandLineOption lan_connect(QStringLiteral("lan-connect"), QStringLiteral("局域网直连目标 IP[:端口]"), QStringLiteral("地址"));
    const QCommandLineOption lan_port(QStringLiteral("lan-port"), QStringLiteral("局域网监听端口"), QStringLiteral("端口"));
    const QCommandLineOption lan_code(QStringLiteral("lan-code"), QStringLiteral("局域网固定被控码"), QStringLiteral("被控码"));
    QCommandLineOption elevated_restart(QStringLiteral("elevated-restart"));
    elevated_restart.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({compatibility, ice, latency, stun, stun_only, relay_only,
        lan_host, lan_connect, lan_port, lan_code, elevated_restart});
    parser.process(app);

    const auto ice_server = parser.value(stun);
    if ((parser.isSet(stun) && !ice_server.startsWith(QStringLiteral("stun:"))) ||
        (parser.isSet(stun_only) && parser.isSet(relay_only))) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"), QStringLiteral(
            "STUN 地址须以 stun: 开头，--stun-only 和 --relay-only 不能同时使用。"));
        return 1;
    }

    SingleInstance instance;
    const auto launch = instance.start(parser.isSet(elevated_restart) && isProcessElevated());
    if (launch == SingleInstance::Result::activated) return 0;
    if (launch == SingleInstance::Result::failed) {
        QMessageBox::warning(nullptr, QStringLiteral("P2P Remote"),
            QStringLiteral("无法启动或激活 P2P Remote，请稍后再试。"));
        return 1;
    }
    if (p2p_core_abi_version() != P2P_CORE_ABI_VERSION) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"),
            QStringLiteral("核心 DLL 版本不匹配，请重新解压完整客户端。"));
        return 1;
    }

    QApplication::setWindowIcon(app.style()->standardIcon(QStyle::SP_ComputerIcon));
    auto& settings = AppSettings::instance();
    AppTheme::apply(settings.data());
    QObject::connect(&settings, &AppSettings::changed, &app, [](const AppSettingsData& next) {
        AppTheme::apply(next);
    });

    AccountWindow window({parser.value(ice).toStdWString(), parser.value(latency).toStdWString(),
        ice_server.toStdString(), parser.isSet(stun_only), parser.isSet(relay_only), parser.isSet(compatibility)});
    window.setObjectName(QStringLiteral("accountWindow"));
    if (!instance.registerWindow(window)) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"), QStringLiteral("无法注册客户端窗口。"));
        return 1;
    }
    window.show();
    if (parser.isSet(lan_host)) {
        const auto port = static_cast<std::uint16_t>(parser.isSet(lan_port) ? parser.value(lan_port).toUShort() : P2P_LAN_DEFAULT_PORT);
        window.startLanHostFromCli(parser.value(lan_code), port);
    } else if (parser.isSet(lan_connect)) {
        window.connectLanFromCli(parser.value(lan_connect), parser.value(lan_code));
    }
    const int result = app.exec();
    instance.unregisterWindow();
    return result;
}
