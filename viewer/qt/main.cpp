#include "account_window.h"
#include "app_settings.h"
#include "app_theme.h"
#include "single_instance.h"

#if defined(__linux__)
#include "../linux/plugin_platform.h"
#endif
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QStyle>
#if defined(__linux__)
#include <QSurfaceFormat>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#endif

int main(int argc, char* argv[]) {
#if defined(__linux__)
    // QOpenGLWidget renders through the top-level window's OpenGL surface;
    // swapInterval must be configured on the default surface format so
    // eglSwapBuffers does not block the GUI thread (input + video) for a full vblank.
    auto surface_format = QSurfaceFormat::defaultFormat();
    surface_format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(surface_format);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("P2PRemote"));
    QCoreApplication::setApplicationName(QStringLiteral("Desktop"));
    QApplication::setApplicationDisplayName(QStringLiteral("P2P Remote"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("P2P Remote Qt 客户端"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption stun(QStringLiteral("stun"), QStringLiteral("覆盖 STUN 地址"), QStringLiteral("stun:主机:端口"));
    const QCommandLineOption stun_only(QStringLiteral("stun-only"), QStringLiteral("仅使用直连"));
    const QCommandLineOption relay_only(QStringLiteral("relay-only"), QStringLiteral("仅使用 TURN 中继"));
    const QCommandLineOption lan_connect(QStringLiteral("lan-connect"), QStringLiteral("局域网直连目标 IP[:端口]"), QStringLiteral("地址"));
    const QCommandLineOption lan_code(QStringLiteral("lan-code"), QStringLiteral("局域网固定被控码"), QStringLiteral("被控码"));
    parser.addOptions({stun, stun_only, relay_only, lan_connect, lan_code});
#if defined(Q_OS_WIN)
    const QCommandLineOption compatibility(QStringLiteral("compat-account"), QStringLiteral("使用兼容账号请求"));
    const QCommandLineOption ice(QStringLiteral("ice-trace"), QStringLiteral("记录 ICE 诊断"), QStringLiteral("目录"));
    const QCommandLineOption latency(QStringLiteral("latency-trace"), QStringLiteral("记录延迟诊断"), QStringLiteral("目录"));
    const QCommandLineOption lan_host(QStringLiteral("lan-host"), QStringLiteral("启动局域网无人值守被控模式"));
    const QCommandLineOption lan_port(QStringLiteral("lan-port"), QStringLiteral("局域网监听端口"), QStringLiteral("端口"));
    QCommandLineOption elevated_restart(QStringLiteral("elevated-restart"));
    elevated_restart.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({compatibility, ice, latency, lan_host, lan_port, elevated_restart});
#endif
    parser.process(app);

    const auto ice_server = parser.value(stun);
    if ((parser.isSet(stun) && !ice_server.startsWith(QStringLiteral("stun:"))) ||
        (parser.isSet(stun_only) && parser.isSet(relay_only))) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"), QStringLiteral(
            "STUN 地址须以 stun: 开头，--stun-only 和 --relay-only 不能同时使用。"));
        return 1;
    }

    SingleInstance instance;
#if defined(Q_OS_WIN)
    const auto launch = instance.start(parser.isSet(elevated_restart) && isProcessElevated());
#else
    const auto launch = instance.start(false);
#endif
    if (launch == SingleInstance::Result::activated) return 0;
    if (launch == SingleInstance::Result::failed) {
        QMessageBox::warning(nullptr, QStringLiteral("P2P Remote"),
            QStringLiteral("无法启动或激活 P2P Remote，请稍后再试。"));
        return 1;
    }
    if (p2p_core_abi_version() != P2P_CORE_ABI_VERSION) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"),
            QStringLiteral("核心版本不匹配，请重新安装完整客户端。"));
        return 1;
    }

#if defined(__linux__)
    const auto clipboard_agent = QFileInfo(QString::fromUtf8(P2PREMOTE_LINUX_HOST_EXECUTABLE)).dir()
        .filePath(QStringLiteral("p2premote_clipboard"));
    if (QFileInfo(clipboard_agent).isExecutable()) QProcess::startDetached(clipboard_agent, {});
    p2premote::initializePluginClipboard();
#endif
    QApplication::setWindowIcon(app.style()->standardIcon(QStyle::SP_ComputerIcon));
    auto& settings = AppSettings::instance();
    AppTheme::apply(settings.data());
    QObject::connect(&settings, &AppSettings::changed, &app, [](const AppSettingsData& next) {
        AppTheme::apply(next);
    });

    LaunchOptions options;
    options.stun = ice_server.toStdString();
    options.stun_only = parser.isSet(stun_only);
    options.relay_only = parser.isSet(relay_only);
#if defined(Q_OS_WIN)
    options.ice_trace = parser.value(ice).toStdWString();
    options.latency_trace = parser.value(latency).toStdWString();
    options.compatible_http = parser.isSet(compatibility);
#endif
    AccountWindow window(std::move(options));
    window.setObjectName(QStringLiteral("accountWindow"));
    if (!instance.registerWindow(window)) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"), QStringLiteral("无法注册客户端窗口。"));
        return 1;
    }
    window.show();
#if defined(Q_OS_WIN)
    if (parser.isSet(lan_host)) {
        const auto port = static_cast<std::uint16_t>(parser.isSet(lan_port) ? parser.value(lan_port).toUShort() : P2P_LAN_DEFAULT_PORT);
        window.startLanHostFromCli(parser.value(lan_code), port);
    } else
#endif
    if (parser.isSet(lan_connect)) {
        window.connectLanFromCli(parser.value(lan_connect), parser.value(lan_code));
    }
    const int result = app.exec();
    instance.unregisterWindow();
    return result;
}
