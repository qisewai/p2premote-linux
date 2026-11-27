#include "clipboard_session.h"
#include "plugin_host.h"
#include "plugin_platform.h"
#include <p2premote/plugin_protocol.h>

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <array>
#include <future>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>

namespace {
using namespace p2premote;
using namespace std::chrono_literals;

QByteArray contents(char seed) {
    QByteArray data(380123, seed);
    for (qsizetype i = 0; i < data.size(); ++i) data[i] ^= char(i % 251);
    return data;
}

bool copyFile(const QString& path, char seed) {
    QFile file(path);
    const auto bytes = contents(seed);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush()) return false;
    file.close();
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
    return true;
}

// File-manager reads run outside the GUI thread, just like an external paste.
std::future<QByteArray> pasteFile() {
    const auto* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat("application/x-p2premote-files") || mime->urls().isEmpty()) return {};
    return std::async(std::launch::async, [path = mime->urls().first().toLocalFile()] {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    });
}

int exercise(bool host, int fd, int argc, char** argv, const QString& temp) {
    QApplication app(argc, argv);
    auto* clipboard = QApplication::clipboard();
    const auto viewer_text = QStringLiteral("来自主控的文字");
    const auto host_text = QStringLiteral("来自被控的文字");
    int stage = 0;
    std::future<QByteArray> paste;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (stage == 0 && clipboard->text() == (host ? viewer_text : host_text)) {
            if (host) clipboard->setText(host_text);
            else if (!copyFile(temp + "/from viewer.bin", 'V')) { app.exit(1); return; }
            ++stage;
        } else if (stage == 1) {
            paste = pasteFile();
            if (paste.valid()) ++stage;
        } else if (stage == 2 && paste.wait_for(0ms) == std::future_status::ready) {
            if (paste.get() != contents(host ? 'V' : 'H')) { app.exit(1); return; }
            if (host && !copyFile(temp + "/来自被控.bin", 'H')) { app.exit(1); return; }
            ++stage;
            if (!host) app.quit();
        }
    });
    timer.start(15);
    QTimer::singleShot(12000, &app, [&] {
        std::cerr << (host ? "host" : "viewer") << " timeout at stage " << stage << '\n';
        app.exit(1);
    });
    if (host) {
        const auto rc = linux_host::RunHostClipboardSession(app, fd);
        return rc || stage != 3;
    }

    clipboard->setText(viewer_text);
    initializePluginClipboard();
    PluginHost viewer(false, [fd](std::string_view packet) {
        return send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(packet.size());
    }, {}, {}, {}, false, {}, false, true);
    QSocketNotifier readable(fd, QSocketNotifier::Read);
    QObject::connect(&readable, &QSocketNotifier::activated, [&] {
        std::array<char, plugin_frame_limit> buf;
        const auto n = recv(fd, buf.data(), buf.size(), MSG_TRUNC);
        if (n <= 0 || size_t(n) > buf.size()) { app.exit(1); return; }
        viewer.Receive({buf.data(), size_t(n)});
    });
    viewer.SetConnected(true);
    const int rc = app.exec();
    viewer.Stop();
    return rc || stage != 3;
}
}

int main(int argc, char** argv) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    qputenv("XDG_RUNTIME_DIR", tmp.path().toUtf8());
    qputenv("XDG_DATA_HOME", tmp.path().toUtf8());
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair)) return 1;
    const auto child = fork();
    if (child < 0) return 1;
    if (!child) {
        close(pair[0]);
        _exit(exercise(true, pair[1], argc, argv, tmp.path()));
    }
    close(pair[1]);
    const auto rc = exercise(false, pair[0], argc, argv, tmp.path());
    close(pair[0]);
    int status{};
    return waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) || rc;
}
