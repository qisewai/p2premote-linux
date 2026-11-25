#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <QStandardPaths>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace p2premote {

void initializePluginClipboard();

struct LocalClipboardFile {
    std::string path, name;
};

class ClipboardBridge {
public:
    ClipboardBridge();
    ~ClipboardBridge();
    bool Available() const noexcept;
    std::string_view Text() const noexcept { return text_; }
    bool SetText(std::string_view text);
    bool Poll(std::uint64_t now);
    unsigned long Wait(std::uint64_t now) const noexcept;
    void CancelWrite() noexcept;
    const std::vector<LocalClipboardFile>& Files() const noexcept { return files_; }
    std::string_view FileIssue() const noexcept { return {}; }

    struct State;

private:
    std::shared_ptr<State> state_;
    std::shared_ptr<std::atomic_bool> active_;
    std::string text_;
    std::vector<LocalClipboardFile> files_;
    std::uint64_t poll_at_{};
};

struct ClipboardReadRequest {
    std::uint32_t id, offer, index;
    std::uint64_t offset;
    unsigned size;
};

class FileClipboard {
public:
    bool Available() const noexcept { return false; }
    std::string_view TakeError() const { return {}; }
    std::optional<ClipboardReadRequest> TakeRequest() { return {}; }
    bool Reply(std::uint32_t, std::optional<std::string_view>) { return false; }
    void Clear() {}
};

class PluginSettings {
public:
    explicit PluginSettings(std::filesystem::path path);
    QJsonValue value(std::string_view key) const;
    bool set(std::string_view key, const QJsonValue& value) const;
    bool merge(const QJsonObject& values) const;

private:
    QJsonObject read() const;
    QString file_;
};

std::filesystem::path DefaultPluginSettingsDirectory();

class DirectoryHandle {
public:
    explicit DirectoryHandle(const std::filesystem::path& path);
    ~DirectoryHandle();
    DirectoryHandle(const DirectoryHandle&) = delete;
    DirectoryHandle& operator=(const DirectoryHandle&) = delete;
    int get() const noexcept { return fd_; }

private:
    int fd_{-1};
};

struct LinuxPluginFile {
    LinuxPluginFile() = default;
    ~LinuxPluginFile();
    LinuxPluginFile(const LinuxPluginFile&) = delete;
    LinuxPluginFile& operator=(const LinuxPluginFile&) = delete;

    bool openRead(const std::filesystem::path& path, struct stat& info);
    bool openWrite(const std::filesystem::path& path);
    bool seek(std::int64_t offset);
    std::optional<QByteArray> read(qsizetype length);
    bool write(std::string_view data);
    QByteArray hashResult();
    bool commit();
    void clear() noexcept;

    long long id{};
    bool writable{};

private:
    int file_{-1};
    std::unique_ptr<DirectoryHandle> parent_;
    std::string temp_, target_;
    std::unique_ptr<QCryptographicHash> hash_;
};

} // namespace p2premote
