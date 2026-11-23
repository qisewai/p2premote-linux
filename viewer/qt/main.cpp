#include "account_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QStyle>

namespace {

constexpr auto stylesheet = R"(
QWidget { color: #172a3f; font-family: "Segoe UI", "Microsoft YaHei UI"; font-size: 14px; }
QWidget#accountWindow, QStackedWidget { background: #f1f5f9; }
QLabel#heading { color: #11253c; font-size: 26px; font-weight: 600; }
QLabel#muted, QLabel#status { color: #475d74; }
QFrame#accountCard { background: #ffffff; border: 1px solid #d4dfe9; border-radius: 9px; }
QLineEdit {
    background: #ffffff; border: 1px solid #bccbd9; border-radius: 5px; min-height: 28px; padding: 3px 8px;
    selection-background-color: #205995;
}
QLineEdit:focus { border: 1px solid #205995; }
QPushButton {
    background: #ffffff; border: 1px solid #bdccda; border-radius: 5px; padding: 5px 12px; min-height: 19px;
}
QPushButton:hover, QPushButton:focus { background: #e8f1fb; border-color: #6b8fb3; }
QPushButton#primary { background: #205995; color: #ffffff; border-color: #205995; font-weight: 600; }
QPushButton#primary:hover, QPushButton#primary:focus { background: #194876; }
QPushButton:disabled, QPushButton#primary:disabled { background: #e9eef3; color: #687b8d; border-color: #d3dee7; }
QListWidget#deviceList { background: #f1f5f9; border: 0; outline: 0; }
QListWidget#deviceList::item {
    background: #ffffff; border: 1px solid #d4dfe9; border-radius: 7px; padding: 8px 12px; min-height: 35px;
}
QListWidget#deviceList::item:hover { background: #eef5fd; }
QListWidget#deviceList::item:selected { background: #dfedfc; border-color: #8fb6dc; color: #11253c; }
QFrame#streamToolbar { background: #f1f5f9; border-bottom: 1px solid #bfccd8; }
QWidget#video { background: #000000; }
)";

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("P2PRemote"));
    QCoreApplication::setApplicationName(QStringLiteral("Desktop"));
    QApplication::setApplicationDisplayName(QStringLiteral("P2P Remote"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setWindowIcon(app.style()->standardIcon(QStyle::SP_ComputerIcon));
    app.setStyleSheet(QString::fromUtf8(stylesheet));

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
    parser.addOptions({compatibility, ice, latency, stun, stun_only, relay_only});
    parser.process(app);

    const auto ice_server = parser.value(stun);
    if ((parser.isSet(stun) && !ice_server.startsWith(QStringLiteral("stun:"))) ||
        (parser.isSet(stun_only) && parser.isSet(relay_only))) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"), QStringLiteral(
            "STUN 地址须以 stun: 开头，--stun-only 和 --relay-only 不能同时使用。"));
        return 1;
    }
    if (p2p_core_abi_version() != P2P_CORE_ABI_VERSION) {
        QMessageBox::critical(nullptr, QStringLiteral("P2P Remote"),
            QStringLiteral("核心 DLL 版本不匹配，请重新解压完整客户端。"));
        return 1;
    }

    AccountWindow window({parser.value(ice).toStdWString(), parser.value(latency).toStdWString(),
        ice_server.toStdString(), parser.isSet(stun_only), parser.isSet(relay_only), parser.isSet(compatibility)});
    window.setObjectName(QStringLiteral("accountWindow"));
    window.show();
    return app.exec();
}
