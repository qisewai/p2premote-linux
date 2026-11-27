#pragma once

#include "../../core/update/update_archive.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

class QFile;
class QNetworkReply;

enum class UpdateState {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Verifying,
    ReadyToInstall,
    Installing,
    Error
};

// Platform-specific applier implemented in viewer/qt/update_apply_win.cpp and viewer/linux/update_apply_linux.cpp
// (and future viewer/mac/update_apply_mac.cpp).
struct UpdateApplyResult {
    bool ok{};
    bool relaunch_requested{};
    QString error;
};

void cleanPlatformUpdateLeftovers() noexcept;
bool isPlatformSelfUpdateSupported();
UpdateApplyResult applyPlatformUpdate(const std::filesystem::path& archive_path,
                                      const std::filesystem::path& manifest_path,
                                      const p2premote::update::UpdateManifest& manifest,
                                      bool has_installed_service);

class UpdateClient final : public QObject {
    Q_OBJECT

public:
    explicit UpdateClient(QObject* parent = nullptr);
    ~UpdateClient() override;

    static QString currentVersion();
    static QString defaultFeedUrl();
    static QString embeddedPublicKey();

    bool isConfigured() const;
    bool isSelfUpdateSupported() const;

    UpdateState state() const noexcept { return state_; }
    const std::optional<p2premote::update::UpdateManifest>& available() const noexcept { return available_; }
    QString statusMessage() const { return status_message_; }
    std::int64_t downloadedBytes() const noexcept { return downloaded_bytes_; }
    std::int64_t totalBytes() const noexcept { return total_bytes_; }
    QString skippedVersion() const;

    void setBusyChecker(std::function<bool()> fn) { is_busy_ = std::move(fn); }
    void setServiceInstalledChecker(std::function<bool()> fn) { has_service_ = std::move(fn); }
    void setConfigOverride(QString feed_url, QString public_key); // For tests
    void applySettings(bool auto_check, bool auto_download, const QString& mirror_url);

    void startPeriodicChecks();
    void checkForUpdates(bool user_initiated);
    void downloadUpdate();
    void installUpdate();
    void skipAvailableVersion();
    void dismissBanner();

signals:
    void stateChanged();
    void progressChanged(qint64 downloaded, qint64 total);
    void relaunchRequested();

private:
    struct DeltaDownload {
        p2premote::update::UpdateDelta patch;
        std::filesystem::path base_archive;
    };

    QString resolveUrl(const QString& raw_url) const;
    std::optional<DeltaDownload> findDelta() const;
    bool fallbackToFullArchive();
    void fetchManifestAttempt(int attempt, bool user_initiated);
    void downloadArchiveAttempt(int attempt);
    void finishDownloadVerification();
    void setState(UpdateState next, QString message = {});
    void cancelActiveReply();

    QNetworkAccessManager network_;
    QPointer<QNetworkReply> active_reply_;
    QTimer startup_timer_;
    QTimer periodic_timer_;
    QTimer header_timer_;
    QTimer stall_timer_;
    QTimer retry_timer_;

    QString feed_override_;
    QString key_override_;
    QString mirror_url_;
    bool auto_check_{true};
    bool auto_download_{true};
    bool user_initiated_{false};
    bool banner_dismissed_{false};

    UpdateState state_{UpdateState::Idle};
    QString status_message_;
    std::optional<p2premote::update::UpdateManifest> available_;
    std::filesystem::path staged_archive_;
    std::filesystem::path staged_manifest_;
    std::optional<DeltaDownload> delta_;
    std::int64_t downloaded_bytes_{0};
    std::int64_t total_bytes_{0};

    std::unique_ptr<QFile> download_file_;
    bool header_received_{false};

    std::function<bool()> is_busy_;
    std::function<bool()> has_service_;
};
