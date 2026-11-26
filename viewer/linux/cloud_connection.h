#pragma once

#include <QJsonArray>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QUrl>

struct p2p_account;

struct CloudConnection {
    QUrl signal_url;
    QByteArray token;
    QJsonArray ice_servers;
    bool relay_only{};
};

const CloudConnection* cloudConnection(const p2p_account* account);
void setCoreError(const QString& message);
QNetworkRequest cloudRequest(const QUrl& url, const QByteArray& token = {});
bool cloudPeerMatches(const QUrl& url, const QSslCertificate& peer);
bool validIdentifier(QStringView value, qsizetype length);
bool parseIceServers(const QJsonArray& servers);
bool isCloudOrigin(const QUrl& origin);
