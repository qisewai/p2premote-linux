#include "../qt/single_instance.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QWidget>

namespace {

std::wstring instanceName() {
    const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) return {};
    const auto display = qgetenv("WAYLAND_DISPLAY") + '/' + qgetenv("DISPLAY");
    const auto id = QCryptographicHash::hash(display, QCryptographicHash::Sha256).toHex().left(16);
    return (runtime + QStringLiteral("/p2premote-") + QString::fromLatin1(id)).toStdWString();
}

} // namespace

SingleInstance::SingleInstance() : name_(instanceName()) {}

SingleInstance::~SingleInstance() {
    unregisterWindow();
    delete static_cast<QLockFile*>(mutex_);
}

SingleInstance::Result SingleInstance::start(bool) {
    if (name_.empty()) return Result::failed;
    const auto name = QString::fromStdWString(name_);
    auto lock = std::make_unique<QLockFile>(name + QStringLiteral(".lock"));
    lock->setStaleLockTime(0);
    const QDeadlineTimer pending(5000);
    do {
        if (lock->tryLock()) {
            mutex_ = lock.release();
            return Result::primary;
        }
        if (lock->error() != QLockFile::LockFailedError) return Result::failed;
        QLocalSocket socket;
        socket.connectToServer(name);
        if (socket.waitForConnected(100) && socket.waitForReadyRead(250) && socket.read(1) == "1") {
            return Result::activated;
        }
        QThread::msleep(50);
    } while (!pending.hasExpired());
    return Result::failed;
}

bool SingleInstance::registerWindow(QWidget& widget) {
    if (!mutex_) return false;
    const auto name = QString::fromStdWString(name_);
    QLocalServer::removeServer(name);
    auto server = std::make_unique<QLocalServer>();
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name)) return false;
    QObject::connect(server.get(), &QLocalServer::newConnection, &widget, [source = server.get(), &widget] {
        while (auto* socket = source->nextPendingConnection()) {
            widget.setWindowState(widget.windowState() & ~Qt::WindowMinimized);
            widget.show();
            widget.raise();
            widget.activateWindow();
            socket->write("1", 1);
            socket->flush();
            socket->disconnectFromServer();
            socket->deleteLater();
        }
    });
    window_ = server.release();
    return true;
}

void SingleInstance::unregisterWindow() {
    delete static_cast<QLocalServer*>(window_);
    window_ = nullptr;
}
