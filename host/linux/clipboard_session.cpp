#include "clipboard_session.h"
#include "clipboard_endpoint.h"
#include "plugin_host.h"
#include "plugin_platform.h"

#include <QApplication>
#include <QSocketNotifier>
#include <array>

namespace p2premote::linux_host {
int RunHostClipboardSession(QApplication& app, int fd) {
    QCoreApplication::setOrganizationName(QStringLiteral("P2PRemote"));
    QCoreApplication::setApplicationName(QStringLiteral("Desktop"));
    app.setQuitOnLastWindowClosed(false);
    initializePluginClipboard();
    PluginHost plugins(true, [fd](std::string_view packet) {
        return ClipboardFrame(packet) && send(fd, packet.data(), packet.size(), MSG_NOSIGNAL | MSG_DONTWAIT)
            == static_cast<ssize_t>(packet.size());
    }, {}, {}, {}, false, {}, false, false);
    plugins.SetConnected(true);
    QSocketNotifier incoming(fd, QSocketNotifier::Read);
    QObject::connect(&incoming, &QSocketNotifier::activated, &app, [&] {
        std::array<char, plugin_frame_limit> buf;
        for (int i = 0; i < 32; ++i) {
            const auto n = recv(fd, buf.data(), buf.size(), MSG_TRUNC);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (n <= 0 || static_cast<size_t>(n) > buf.size() || !ClipboardFrame({buf.data(), size_t(n)})) {
                incoming.setEnabled(false);
                app.quit();
                return;
            }
            plugins.Receive({buf.data(), size_t(n)});
        }
    });
    const int rc = app.exec();
    plugins.Stop();
    return rc;
}

} // namespace p2premote::linux_host
