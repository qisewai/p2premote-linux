#include "plugin_platform.h"

#include <QDir>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <openssl/rand.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace p2premote {

std::filesystem::path DefaultPluginSettingsDirectory() {
    const auto root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (root.isEmpty()) return {};
    return QDir(root).filePath(QStringLiteral("P2PRemote/plugin-config")).toStdString();
}

PluginSettings::PluginSettings(std::filesystem::path path) : file_(QString::fromStdString(path.native())) {}

QJsonObject PluginSettings::read() const {
    if (file_.isEmpty()) return {};
    QFile file(file_);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.read(16 * 1024 + 1)).object() : QJsonObject{};
}

QJsonValue PluginSettings::value(std::string_view key) const {
    return read().value(QString::fromUtf8(key.data(), key.size()));
}

bool PluginSettings::set(std::string_view key, const QJsonValue& value) const {
    return merge(QJsonObject{{QString::fromUtf8(key.data(), key.size()), value}});
}

bool PluginSettings::merge(const QJsonObject& values) const {
    if (file_.isEmpty() || !QDir().mkpath(QFileInfo(file_).absolutePath())) return false;
    QLockFile guard(file_ + QStringLiteral(".lock"));
    if (!guard.tryLock(150)) return false;
    auto merged = read();
    for (auto entry = values.begin(); entry != values.end(); ++entry) {
        if (entry.value().isNull()) merged.remove(entry.key());
        else merged.insert(entry.key(), entry.value());
    }
    const auto bytes = QJsonDocument(merged).toJson(QJsonDocument::Compact);
    if (bytes.size() > 16 * 1024) return false;
    QSaveFile file(file_);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
        file.write(bytes) == bytes.size() && file.commit();
}

DirectoryHandle::DirectoryHandle(const std::filesystem::path& path) {
    if (!path.is_absolute()) return;
    constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    fd_ = ::open("/", flags);
    for (const auto& entry : path.relative_path()) {
        if (fd_ < 0) break;
        if (entry.empty() || entry == ".") continue;
        const auto next = entry == ".." ? -1 : ::openat(fd_, entry.c_str(), flags);
        const auto error = errno;
        ::close(fd_);
        fd_ = next;
        errno = error;
    }
}

DirectoryHandle::~DirectoryHandle() { if (fd_ >= 0) ::close(fd_); }

LinuxPluginFile::~LinuxPluginFile() { clear(); }

bool LinuxPluginFile::openRead(const std::filesystem::path& path, struct stat& info) {
    const DirectoryHandle parent(path.parent_path());
    if (parent.get() < 0 || path.filename().empty()) return false;
    file_ = ::openat(parent.get(), path.filename().c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (file_ < 0 || ::fstat(file_, &info) != 0 || !S_ISREG(info.st_mode)) {
        clear();
        return false;
    }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    return true;
}

bool LinuxPluginFile::openWrite(const std::filesystem::path& path) {
    auto parent = std::make_unique<DirectoryHandle>(path.parent_path());
    const auto name = path.filename().native();
    if (parent->get() < 0 || name.empty()) return false;
    struct stat info{};
    if (::fstatat(parent->get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(info.st_mode)) return false;
    QByteArray random(12, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), random.size()) != 1) return false;
    auto temp = ".p2ptmp." + random.toHex().toStdString();
    const int file = ::openat(parent->get(), temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) return false;
    parent_ = std::move(parent);
    temp_ = std::move(temp);
    target_ = name;
    file_ = file;
    writable = true;
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    return true;
}

bool LinuxPluginFile::seek(std::int64_t offset) {
    if (offset < 0 || ::lseek(file_, offset, SEEK_SET) < 0) return false;
    hash_.reset();
    return true;
}

std::optional<QByteArray> LinuxPluginFile::read(qsizetype length) {
    QByteArray data(length, Qt::Uninitialized);
    ssize_t count;
    do { count = ::read(file_, data.data(), data.size()); } while (count < 0 && errno == EINTR);
    if (count < 0) return {};
    data.resize(count);
    if (hash_) hash_->addData(data);
    return data;
}

bool LinuxPluginFile::write(std::string_view data) {
    while (!data.empty()) {
        const auto count = ::write(file_, data.data(), data.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        if (hash_) hash_->addData(QByteArrayView(data.data(), count));
        data.remove_prefix(count);
    }
    return true;
}

QByteArray LinuxPluginFile::hashResult() { return hash_ ? hash_->result().toHex() : QByteArray{}; }

bool LinuxPluginFile::commit() {
    if (!parent_ || ::fsync(file_) != 0) return false;
    const auto fd = parent_->get();
    if (::renameat(fd, temp_.c_str(), fd, target_.c_str()) != 0) return false;
    temp_.clear();
    clear();
    return true;
}

void LinuxPluginFile::clear() noexcept {
    if (file_ >= 0) ::close(file_);
    if (parent_ && !temp_.empty()) ::unlinkat(parent_->get(), temp_.c_str(), 0);
    file_ = -1;
    parent_.reset();
    temp_.clear();
    target_.clear();
    hash_.reset();
    id = 0;
    writable = false;
}

} // namespace p2premote
