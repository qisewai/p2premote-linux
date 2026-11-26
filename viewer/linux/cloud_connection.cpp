#include "cloud_connection.h"

#include <p2premote/service_identity.h>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QSslKey>

#include <algorithm>

namespace {

bool isBundledService(const QUrl& url) {
    const QUrl bundled(QString::fromWCharArray(p2premote::wuhan_service_address));
    return url.host() == bundled.host() && url.port() == bundled.port();
}

} // namespace

bool isCloudOrigin(const QUrl& origin) {
    const bool loopback = origin.scheme() == "http" && origin.host() == "127.0.0.1";
    return origin.isValid() && (origin.scheme() == "https" || loopback) && !origin.host().isEmpty() &&
        (origin.path().isEmpty() || origin.path() == "/") && origin.userInfo().isEmpty() &&
        !origin.hasQuery() && !origin.hasFragment();
}

bool validIdentifier(QStringView value, qsizetype length) {
    if (value.size() != length) return false;
    return std::ranges::all_of(value, [](QChar c) {
        const auto n = c.unicode();
        return (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
            (n >= '0' && n <= '9') || n == '_' || n == '-';
    });
}

bool parseIceServers(const QJsonArray& servers) {
    if (servers.isEmpty() || servers.size() > 32) return false;
    for (const auto& entry : servers) {
        const auto server = entry.toObject();
        const auto value = server.value("urls");
        const auto urls = value.isString() ? QJsonArray{value} : value.toArray();
        if (urls.isEmpty() || urls.size() > 32) return false;
        for (const auto& item : urls) {
            const auto url = item.toString();
            const auto prefix = url.section(u':', 0, 0);
            if (url.size() <= prefix.size() + 1 || url.contains(u' ') || url.contains(u'\n') ||
                url.contains(u'\r') || url.contains(u'\t')) return false;
            if (prefix == "turn" || prefix == "turns") {
                if (server.value("username").toString().isEmpty() || server.value("credential").toString().isEmpty()) {
                    return false;
                }
            } else if (prefix != "stun") {
                return false;
            }
        }
    }
    return true;
}

QNetworkRequest cloudRequest(const QUrl& url, const QByteArray& token) {
    QNetworkRequest request(url);
    request.setTransferTimeout(12000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    if (url.scheme() == "https" || url.scheme() == "wss") {
        auto tls = QSslConfiguration::defaultConfiguration();
        tls.setProtocol(QSsl::TlsV1_2OrLater);
        if (isBundledService(url)) {
            tls.setCaCertificates(QSslCertificate::fromData(p2premote::wuhan_ca_certificate));
        }
        request.setSslConfiguration(tls);
    }
    return request;
}

bool cloudPeerMatches(const QUrl& url, const QSslCertificate& peer) {
    if (!isBundledService(url)) return true;
    const auto hash = QCryptographicHash::hash(peer.publicKey().toDer(), QCryptographicHash::Sha256).toBase64();
    return "sha256//" + hash == p2premote::wuhan_public_key_pin;
}
