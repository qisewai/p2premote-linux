#include <p2premote/core_api.h>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <vector>

struct p2p_lan_scanner {
    struct Peer {
        std::wstring id, name, address;
        quint16 port{};
        bool busy{};
        qint64 last_seen{};
    };

    p2p_lan_scanner() {
        if (!socket.bind(QHostAddress::AnyIPv4, P2P_LAN_DISCOVERY_PORT,
                QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            socket.bind(QHostAddress::AnyIPv4, quint16(0));
        }
        elapsed.start();
        QObject::connect(&socket, &QUdpSocket::readyRead, &timer, [this] { read(); });
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { probe(); });
        timer.start(3000);
    }

    bool probe() {
        constexpr auto message = R"json({"p2p_lan_discover":1})json";
        const auto send = [this, message](const QHostAddress& address) {
            return socket.writeDatagram(message, address, P2P_LAN_DISCOVERY_PORT) > 0;
        };
        bool sent = send(QHostAddress::Broadcast);
        for (const auto& interface : QNetworkInterface::allInterfaces()) {
            if (!interface.flags().testFlag(QNetworkInterface::IsUp)) continue;
            for (const auto& address : interface.addressEntries()) {
                const auto broadcast = address.broadcast();
                if (broadcast.protocol() == QAbstractSocket::IPv4Protocol) sent |= send(broadcast);
            }
        }
        return sent;
    }

    void read() {
        while (socket.hasPendingDatagrams()) {
            const auto packet = socket.receiveDatagram(513);
            if (packet.data().size() > 512 || packet.senderAddress().protocol() != QAbstractSocket::IPv4Protocol) continue;
            const auto beacon = QJsonDocument::fromJson(packet.data()).object();
            if (!beacon.contains("p2p_lan_host")) continue;
            const auto port = beacon.value("port").toInt(P2P_LAN_DEFAULT_PORT);
            if (port < 1 || port > 65535) continue;
            Peer peer{beacon.value("id").toString().left(128).toStdWString(),
                beacon.value("name").toString("Windows PC").left(128).toStdWString(),
                packet.senderAddress().toString().toStdWString(), static_cast<quint16>(port),
                beacon.value("busy").toBool(), elapsed.elapsed()};
            const auto existing = std::ranges::find_if(peers, [&peer](const Peer& item) {
                return (!peer.id.empty() && peer.id == item.id) ||
                    (peer.address == item.address && peer.port == item.port);
            });
            if (existing != peers.end()) *existing = std::move(peer);
            else if (peers.size() < 256) peers.push_back(std::move(peer));
        }
    }

    QElapsedTimer elapsed;
    QUdpSocket socket;
    QTimer timer;
    std::vector<Peer> peers;
};

p2p_lan_scanner* p2p_lan_scanner_create() { return new p2p_lan_scanner; }
void p2p_lan_scanner_destroy(p2p_lan_scanner* scanner) { delete scanner; }

int32_t p2p_lan_scanner_probe(p2p_lan_scanner* scanner) {
    return scanner && scanner->probe() ? P2P_OK : P2P_ERROR;
}

size_t p2p_lan_scanner_read(p2p_lan_scanner* scanner) {
    if (!scanner) return 0;
    std::erase_if(scanner->peers, [now = scanner->elapsed.elapsed()](const auto& peer) {
        return now - peer.last_seen > 10000;
    });
    return scanner->peers.size();
}

int32_t p2p_lan_scanner_peer(const p2p_lan_scanner* scanner, size_t index, p2p_lan_peer_v1* peer) {
    if (!scanner || !peer || index >= scanner->peers.size()) return P2P_ERROR;
    const auto& item = scanner->peers[index];
    *peer = {item.id.c_str(), item.name.c_str(), item.address.c_str(), item.port, item.busy};
    return P2P_OK;
}

const wchar_t* p2p_lan_local_addresses() {
    QStringList addresses;
    for (const auto& address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() && !address.isNull()) {
            addresses.push_back(address.toString());
        }
    }
    static thread_local std::wstring text;
    text = addresses.join(QStringLiteral(", ")).toStdWString();
    return text.c_str();
}
