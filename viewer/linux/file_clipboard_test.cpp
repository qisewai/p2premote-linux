#include "plugin_host.h"
#include "plugin_platform.h"

#include <p2premote/plugin_protocol.h>

#include <QApplication>
#include <QClipboard>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace p2premote;
using namespace std::chrono_literals;

constexpr auto plugin_id = "file-clipboard";

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool await(auto predicate, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        QApplication::processEvents();
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class Peer {
public:
    bool send(std::string_view frame) {
        auto packet = DecodePluginPacket(frame);
        if (!packet || packet->id != plugin_id) return false;
        const std::lock_guard lock(mutex_);
        packets_.push_back({std::string(packet->type), std::string(packet->payload)});
        return true;
    }

    bool has(std::string_view type) {
        const std::lock_guard lock(mutex_);
        return std::ranges::any_of(packets_, [&](const Message& msg) { return msg.type == type; });
    }

    std::string take(std::string_view type) {
        std::string payload;
        const bool received = await([&] {
            const std::lock_guard lock(mutex_);
            const auto packet = std::ranges::find(packets_, type, &Message::type);
            if (packet == packets_.end()) return false;
            payload = std::move(packet->payload);
            packets_.erase(packet);
            return true;
        });
        if (!received) throw std::runtime_error("Timed out waiting for " + std::string(type));
        return payload;
    }

    static void receive(PluginHost& host, std::string_view type, std::string_view payload) {
        const auto frame = EncodePluginPacket({plugin_id, std::string(type), std::string(payload)});
        check(frame.has_value(), "Could not encode fixture packet");
        host.Receive(*frame);
    }

private:
    struct Message {
        std::string type, payload;
    };

    std::mutex mutex_;
    std::deque<Message> packets_;
};

template <typename... Values>
QByteArray encode(Values... values) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    (stream << ... << values);
    return bytes;
}

void fileOffer(PluginHost& host, int id, const QJsonArray& files) {
    const auto offer = QJsonDocument(QJsonObject{{"id", id}, {"files", files}}).toJson(QJsonDocument::Compact);
    Peer::receive(host, "offer", offer.toStdString());
}

ClipboardReadRequest readRequest(Peer& peer) {
    const auto payload = peer.take("read");
    QDataStream stream(QByteArray::fromStdString(payload));
    stream.setByteOrder(QDataStream::LittleEndian);
    ClipboardReadRequest request{};
    quint64 offset{};
    stream >> request.id >> request.offer >> request.index >> offset >> request.size;
    request.offset = offset;
    check(stream.status() == QDataStream::Ok && stream.atEnd(), "Unexpected format of clipboard read");
    return request;
}

void checkWebUrlFallback(QClipboard& clipboard) {
    ClipboardBridge bridge;
    bridge.WatchFiles();
    bridge.Poll(100);

    // Web URLs often carry both text/uri-list and text/plain; they must sync as text without external_files warnings.
    auto* mime = new QMimeData;
    mime->setUrls({QUrl(QStringLiteral("https://example.com/docs?q=1"))});
    mime->setText(QStringLiteral("https://example.com/docs?q=1"));
    clipboard.setMimeData(mime);
    QApplication::processEvents();
    check(bridge.Poll(300), "Bridge did not observe web URL clipboard change");
    check(bridge.Text() == "https://example.com/docs?q=1", "Web URL text was dropped by file URI detection");
    check(bridge.Files().empty() && bridge.FileIssue().empty(), "Web URL produced false file clipboard issue");
}

void checkOutgoing(PluginHost& host, Peer& peer, const QTemporaryDir& temp, QClipboard& clipboard) {
    const auto folder = temp.filePath(QStringLiteral("Folder with spaces"));
    check(QDir().mkpath(folder + QStringLiteral("/Empty")), "Could not create fixture directory");
    const auto name = QStringLiteral("样本.txt");
    const auto path = QDir(folder).filePath(name);
    const QByteArray content(40000, 'a');
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(content) == content.size(), "Could not write fixture file");
    file.close();

    // GTK-only file copy format with CRLF line endings, plus an additional top-level file.
    auto* mime = new QMimeData;
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), "copy\r\n" +
        QUrl::fromLocalFile(folder).toEncoded() + "\r\n" + QUrl::fromLocalFile(path).toEncoded());
    clipboard.setMimeData(mime);
    const auto offer = QJsonDocument::fromJson(QByteArray::fromStdString(peer.take("offer"))).object();
    const auto entries = offer.value("files").toArray();
    check(entries.size() == 4 && entries.at(0).toArray().at(2).toBool(), "Incorrect multi-file offer");
    check(entries.at(2).toArray().at(0).toString() == QStringLiteral("Folder with spaces\\样本.txt"),
        "Incorrect Windows manifest path for a Linux directory");
    const auto offer_id = static_cast<quint32>(offer.value("id").toInt());
    const auto request = encode(quint32{1}, offer_id, quint32{3}, qint64{0}, static_cast<quint32>(content.size()));
    Peer::receive(host, "read", request.toStdString());
    const auto first = peer.take("data");
    const auto second = peer.take("data");
    check(first.size() == 32768 + 8 && second.size() == 40000 - 32768 + 8, "Legacy peer chunk size did not work");
    check(first.substr(8) + second.substr(8) == content.toStdString(), "Outgoing bytes changed");
}

void checkIncomingVirtualFuse(PluginHost& host, Peer& peer, QClipboard& clipboard) {
    const auto marker = QStringLiteral("application/x-p2premote-files");
    QByteArray contents(310000, '\0');
    for (qsizetype i = 0; i < contents.size(); ++i) contents[i] = static_cast<char>('a' + (i % 26));

    // Include a multi-gigabyte virtual file entry to prove there is no 512 MiB cap and zero eager download.
    const qint64 huge_size = qint64{4} * 1024 * 1024 * 1024;
    const QJsonArray manifest{
        QJsonArray{"Incoming", 0, true},
        QJsonArray{"Incoming\\样本.txt", contents.size(), false},
        QJsonArray{"Empty", 0, false},
        QJsonArray{"Huge.iso", huge_size, false}
    };
    fileOffer(host, 1, manifest);

    // Publishing must happen immediately with ZERO "read" requests issued beforehand!
    check(await([&] {
        const auto* current = clipboard.mimeData();
        return current && current->hasFormat(marker);
    }), "Virtual files were never published");
    std::this_thread::sleep_for(50ms);
    QApplication::processEvents();
    check(!peer.has("read"), "Offer eagerly requested file bytes before paste");

    const auto* mime = clipboard.mimeData();
    check(mime->urls().size() == 3 && mime->data("application/x-kde-cutselection") == "0" &&
        mime->data("x-special/gnome-copied-files").startsWith("copy\nfile:"), "Missing file manager clipboard formats");
    const auto folder = mime->urls().at(0).toLocalFile();
    const auto empty_path = mime->urls().at(1).toLocalFile();
    const auto huge_path = mime->urls().at(2).toLocalFile();

    // Stat & directory listing must work purely from virtual metadata, still with zero "read" RPCs.
    check(QFileInfo(folder).isDir(), "Virtual folder is missing in FUSE mount");
    const auto entries = QDir(folder).entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    check(entries.size() == 1 && entries.front() == QStringLiteral("样本.txt"), "Virtual folder listing mismatch");
    const QFileInfo empty_info(empty_path);
    check(empty_info.isFile() && empty_info.size() == 0, "Virtual empty file stat mismatch");
    const QFileInfo huge_info(huge_path);
    check(huge_info.isFile() && huge_info.size() == huge_size, "Virtual >512 MiB file stat mismatch");
    check(!peer.has("read"), "Metadata stat/readdir triggered remote file reads");

    // Now actually read the virtual file from a background thread (simulating Dolphin/Nautilus paste)
    // and serve chunks on demand from the peer.
    const auto virtual_file = QDir(folder).filePath(QStringLiteral("样本.txt"));
    auto reader = std::async(std::launch::async, [virtual_file] {
        QFile file(virtual_file);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray{};
        return file.readAll();
    });

    for (std::uint64_t pos = 0; pos < static_cast<std::uint64_t>(contents.size());) {
        const auto req = readRequest(peer);
        check(req.offer == 1 && req.index == 2 && req.offset == pos && req.size <= contents.size() - pos,
            "On-demand FUSE read request did not match expected window");
        for (unsigned part = 0; part < req.size;) {
            const auto count = std::min(32768u, req.size - part);
            const auto data = encode(req.id, part) + contents.sliced(static_cast<qsizetype>(pos + part), count);
            Peer::receive(host, "data", data.toStdString());
            part += count;
        }
        pos += req.size;
    }
    check(reader.wait_for(3s) == std::future_status::ready, "Virtual file reader hung");
    check(reader.get() == contents, "On-demand FUSE file contents mismatch");

    // Back-to-back offers must replace cleanly without triggering false clipboard_changed errors!
    fileOffer(host, 2, QJsonArray{QJsonArray{"Next", 0, false}});
    fileOffer(host, 3, QJsonArray{QJsonArray{"Final.txt", 16, false}});
    check(await([&] {
        const auto* current = clipboard.mimeData();
        return current && current->hasFormat(marker) && current->urls().size() == 1 &&
            current->urls().front().fileName() == QStringLiteral("Final.txt");
    }), "Back-to-back virtual offers failed to publish the latest offer");
    // Old offer path must no longer exist after replacement.
    check(!QFileInfo::exists(folder), "Stale offer path remained visible after replacement");
    for (const auto& action : host.TakeActions()) {
        check(action.notice.find(L"已变更") == std::wstring::npos, "Back-to-back offers caused false clipboard_changed");
    }

    // Invalid path traversal offer must be rejected.
    fileOffer(host, 4, QJsonArray{QJsonArray{"Invalid", 0, true}, QJsonArray{"Invalid\\..\\outside", 1, false}});
    check(await([&] {
        const auto actions = host.TakeActions();
        return std::ranges::any_of(actions, [](const auto& action) {
            return action.notice.find(L"无法识别") != std::wstring::npos;
        });
    }), "An invalid nested path was not rejected");

    // Mid-read disconnect/stop must unblock FUSE reader cleanly without hanging and leave local clipboard intact.
    fileOffer(host, 5, QJsonArray{QJsonArray{"Disconnect.txt", 64, false}});
    QString disconnect_path;
    check(await([&] {
        const auto* current = clipboard.mimeData();
        if (!current || !current->hasFormat(marker) || current->urls().size() != 1) return false;
        disconnect_path = current->urls().front().toLocalFile();
        return current->urls().front().fileName() == QStringLiteral("Disconnect.txt");
    }), "Disconnect fixture offer did not publish");

    auto blocked_reader = std::async(std::launch::async, [disconnect_path] {
        QFile file(disconnect_path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        const auto data = file.read(64);
        return data.isEmpty() || file.error() != QFileDevice::NoError;
    });
    const auto interrupted = readRequest(peer);
    Peer::receive(host, "data", (encode(interrupted.id, quint32{0}) + QByteArray(8, 'x')).toStdString());
    clipboard.setText(QStringLiteral("new local copy"));
    QApplication::processEvents();
    host.Stop();
    QApplication::processEvents();
    check(blocked_reader.wait_for(3s) == std::future_status::ready, "Disconnect left FUSE reader blocked");
    check(blocked_reader.get(), "Interrupted FUSE read unexpectedly succeeded");
    check(clipboard.text() == QStringLiteral("new local copy"), "Disconnect clobbered local clipboard");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        check(argc == 2, "Expected the file-clipboard plugin directory");
        QTemporaryDir temp;
        check(temp.isValid(), "Could not create test directory");
        const auto root = std::filesystem::path(temp.filePath("plugins").toStdString());
        std::filesystem::create_directory(root);
        std::filesystem::copy(argv[1], root / plugin_id, std::filesystem::copy_options::recursive);
        initializePluginClipboard();
        checkWebUrlFallback(*QApplication::clipboard());

        Peer peer;
        PluginHost host(false, [&](auto frame) { return peer.send(frame); }, root);
        host.SetConnected(true);
        check(peer.take("hello") == "2", "File clipboard plugin did not load");
        Peer::receive(host, "ready", "1");
        checkOutgoing(host, peer, temp, *QApplication::clipboard());
        checkIncomingVirtualFuse(host, peer, *QApplication::clipboard());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
