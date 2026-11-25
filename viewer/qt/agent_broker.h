#pragma once

#include <p2premote/core_api.h>

#include <QLocalServer>
#include <QDeadlineTimer>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>

class QJsonObject;
class QLocalSocket;

class AgentBroker final : public QObject {
public:
    using Session = std::function<p2p_stream*()>;

    explicit AgentBroker(QObject* parent = nullptr);
    ~AgentBroker() override;
    bool start(p2p_account* account, QString device, Session session, bool screenshots = false,
        QMap<QString, QStringList> reports = {});
    void stop();
    bool active() const { return server_.isListening(); }
    QString token() const { return token_; }

private:
    void acceptClients();
    void readRequest(QLocalSocket* socket);
    void poll();
    void cancel();
    static void reply(QLocalSocket* socket, const QJsonObject& result);

    QLocalServer server_;
    QTimer timer_;
    QSet<QLocalSocket*> clients_;
    QPointer<QLocalSocket> waiting_;
    Session session_;
    p2p_account* account_{};
    QString device_;
    QString token_;
    QMap<QString, QStringList> reports_;
    QStringList pending_fields_;
    std::uint32_t pending_{};
    bool screenshots_{};
    QDeadlineTimer deadline_;
};
