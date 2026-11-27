#include "update_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <bzlib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>

#define CHECK(expr)                                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " #expr "\n";      \
            std::exit(1);                                                                           \
        }                                                                                           \
    } while (0)

// Stub platform hooks for unit-testing UpdateClient without touching real app dir.
void cleanPlatformUpdateLeftovers() noexcept {}
bool isPlatformSelfUpdateSupported() { return true; }
UpdateApplyResult applyPlatformUpdate(const std::filesystem::path&, const std::filesystem::path&,
                                      const p2premote::update::UpdateManifest&, bool) {
    return {true, false, {}};
}

namespace {

class LocalHttpServer final : public QObject {
public:
    bool start(std::map<std::string, std::string> routes) {
        routes_ = std::move(routes);
        if (!server_.listen(QHostAddress::LocalHost, 0)) return false;
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* sock = server_.nextPendingConnection()) {
                connect(sock, &QTcpSocket::readyRead, sock, [this, sock] {
                    const QByteArray req = sock->readAll();
                    const int first_sp = req.indexOf(' ');
                    const int second_sp = req.indexOf(' ', first_sp + 1);
                    if (first_sp < 0 || second_sp < 0) {
                        sock->disconnectFromHost();
                        return;
                    }
                    const std::string path = req.mid(first_sp + 1, second_sp - first_sp - 1).toStdString();
                    ++requests_[path];
                    auto it = routes_.find(path);
                    if (it == routes_.end()) {
                        sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    } else {
                        const std::string& body = it->second;
                        const std::string hdr = "HTTP/1.1 200 OK\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                        sock->write(hdr.data(), static_cast<qint64>(hdr.size()));
                        sock->write(body.data(), static_cast<qint64>(body.size()));
                    }
                    sock->flush();
                    sock->disconnectFromHost();
                });
                connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
            }
        });
        return true;
    }

    quint16 port() const { return server_.serverPort(); }
    void setRoutes(std::map<std::string, std::string> routes) { routes_ = std::move(routes); }
    int requestsFor(const std::string& path) { return requests_[path]; }

private:
    QTcpServer server_;
    std::map<std::string, std::string> routes_;
    std::map<std::string, int> requests_;
};

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string Bzip(std::string input) {
    auto capacity = static_cast<unsigned>(input.size() * 2 + 1024);
    std::string output(capacity, '\0');
    CHECK(BZ2_bzBuffToBuffCompress(output.data(), &capacity, input.data(),
                                     static_cast<unsigned>(input.size()), 9, 0, 0) == BZ_OK);
    output.resize(capacity);
    return output;
}

std::string Offset(std::uint64_t value) {
    std::string out(8, '\0');
    for (auto& c : out) {
        c = static_cast<char>(value & 0xff);
        value >>= 8;
    }
    return out;
}

std::string MakePatch(const std::string& base, const std::string& next) {
    CHECK(base.size() == next.size());
    std::string diff(next.size(), '\0');
    for (std::size_t i = 0; i < diff.size(); ++i) {
        diff[i] = static_cast<char>(static_cast<unsigned char>(next[i]) - static_cast<unsigned char>(base[i]));
    }
    const auto control = Bzip(Offset(next.size()) + Offset(0) + Offset(0));
    const auto delta = Bzip(std::move(diff));
    return "BSDIFF40" + Offset(control.size()) + Offset(delta.size()) + Offset(next.size()) +
        control + delta + Bzip({});
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("P2PRemoteTest"));
    QCoreApplication::setApplicationName(QStringLiteral("UpdateClientTest"));

    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/updates")).removeRecursively();

    using namespace p2premote::update;

    const QTemporaryDir temporary;
    CHECK(temporary.isValid());
#if defined(Q_OS_WIN)
    const std::filesystem::path tmp(temporary.path().toStdWString());
#else
    const std::filesystem::path tmp(temporary.path().toStdString());
#endif
    const auto staged = tmp / "staged";
    std::filesystem::create_directories(staged);
    {
        std::ofstream f(staged / "hello.txt", std::ios::binary);
        std::mt19937 random(9);
        for (int i = 0; i < 64 * 1024; ++i) f.put(static_cast<char>(random() & 0xff));
    }
    CHECK(WriteFileList(staged, {"hello.txt"}));

    const auto archive_path = tmp / "p2premote-9.9.9.tar.gz";
    std::int64_t archive_size{};
    Sha256Digest archive_sha{};
    CHECK(WriteTarGzArchive(staged, {"hello.txt", std::string(kManagedFileListName)},
                            archive_path, &archive_size, &archive_sha));

    Ed25519PublicKey pub{};
    Ed25519PrivateKey priv{};
    CHECK(Ed25519GenerateKeypair(pub, priv));
    const QString pub_b64 = QString::fromStdString(Base64Encode(pub.data(), pub.size()));

    const std::string archive_bytes = ReadBytes(archive_path);
    std::string old_bytes = archive_bytes;
    old_bytes.back() ^= 0x5a;
    const auto patch = MakePatch(old_bytes, archive_bytes);
    CHECK(patch.size() < archive_bytes.size());
    const auto cache_dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("updates/") + UpdateClient::currentVersion());
    CHECK(QDir().mkpath(cache_dir));
    {
        QFile cached(QDir(cache_dir).filePath(QStringLiteral("update.tar.gz")));
        CHECK(cached.open(QIODevice::WriteOnly));
        CHECK(cached.write(old_bytes.data(), old_bytes.size()) == static_cast<qint64>(old_bytes.size()));
    }

    LocalHttpServer srv;
    CHECK(srv.start({}));
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port());

    UpdateManifest good_manifest;
    good_manifest.version = "9.9.9";
    good_manifest.notes = "- Loopback update test";
    good_manifest.date = "2026-11-27T06:00:00Z";
    good_manifest.url = base + "/p2premote-9.9.9.tar.gz";
    good_manifest.size = archive_size;
    good_manifest.signature = SignDigestBase64(priv, archive_sha);
    good_manifest.target = P2PREMOTE_UPDATE_TARGET;
    good_manifest.previous.push_back({UpdateClient::currentVersion().toStdString(), base + "/previous.tar.gz",
        archive_size, SignDigestBase64(priv, Sha256::Hash(old_bytes.data(), old_bytes.size()))});
    good_manifest.deltas.push_back({good_manifest.previous.front().version, base + "/good.delta",
        static_cast<std::int64_t>(patch.size()), SignDigestBase64(priv, Sha256::Hash(patch.data(), patch.size()))});
    SignUpdateManifest(good_manifest, priv);

    auto fallback_manifest = good_manifest;
    fallback_manifest.version = "9.9.10";
    fallback_manifest.deltas.front().url = base + "/tampered.delta";
    SignUpdateManifest(fallback_manifest, priv);

    UpdateManifest bad_manifest = good_manifest;
    bad_manifest.version = "9.9.11";
    bad_manifest.url = base + "/tampered.tar.gz";
    bad_manifest.deltas.clear();
    SignUpdateManifest(bad_manifest, priv);
    auto altered_manifest = good_manifest;
    altered_manifest.version = "9.9.12"; // signature still refers to 9.9.9
    std::string tampered_bytes = archive_bytes;
    tampered_bytes[0] ^= 0x5a;
    auto tampered_patch = patch;
    tampered_patch.back() ^= 0x5a;

    srv.setRoutes({
        {"/update.json", SerializeUpdateManifest(good_manifest)},
        {"/fallback.json", SerializeUpdateManifest(fallback_manifest)},
        {"/bad-update.json", SerializeUpdateManifest(bad_manifest)},
        {"/altered.json", SerializeUpdateManifest(altered_manifest)},
        {"/p2premote-9.9.9.tar.gz", archive_bytes},
        {"/tampered.tar.gz", tampered_bytes},
        {"/good.delta", patch},
        {"/tampered.delta", tampered_patch}
    });

    auto wait_until = [&](UpdateClient& client, UpdateState target) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { loop.exit(1); });
        QObject::connect(&client, &UpdateClient::stateChanged, &loop, [&] {
            if (client.state() == target || client.state() == UpdateState::Error) loop.exit(0);
        });
        if (client.state() == target) return;
        timeout.start(5000);
        CHECK(loop.exec() == 0);
        CHECK(client.state() == target);
    };

    const auto update = [&](std::string_view path, UpdateState final_state) {
        UpdateClient client;
        client.setConfigOverride(QString::fromStdString(base + std::string(path)), pub_b64);
        client.applySettings(false, false, {});
        client.checkForUpdates(true);
        wait_until(client, UpdateState::Available);
        client.downloadUpdate();
        wait_until(client, final_state);
    };
    update("/update.json", UpdateState::ReadyToInstall);
    CHECK(srv.requestsFor("/good.delta") == 1 && srv.requestsFor("/p2premote-9.9.9.tar.gz") == 0);
    update("/fallback.json", UpdateState::ReadyToInstall);
    CHECK(srv.requestsFor("/tampered.delta") == 1 && srv.requestsFor("/p2premote-9.9.9.tar.gz") == 1);
    update("/bad-update.json", UpdateState::Error);

    {
        UpdateClient client;
        client.setConfigOverride(QString::fromStdString(base + "/altered.json"), pub_b64);
        client.applySettings(false, false, {});
        client.checkForUpdates(true);
        wait_until(client, UpdateState::Error);
    }
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/updates")).removeRecursively();

    std::cout << "update_client_test: all assertions passed\n";
    return 0;
}
