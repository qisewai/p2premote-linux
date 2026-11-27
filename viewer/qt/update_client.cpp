#include "update_client.h"
#include "update_delta.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <fstream>

#ifndef P2PREMOTE_VERSION
#define P2PREMOTE_VERSION "0.2.0"
#endif

#ifndef P2PREMOTE_UPDATE_FEED_URL
#define P2PREMOTE_UPDATE_FEED_URL ""
#endif

#ifndef P2PREMOTE_UPDATE_PUBLIC_KEY
#define P2PREMOTE_UPDATE_PUBLIC_KEY ""
#endif

namespace {

constexpr int kStartupCheckDelayMs = 10 * 1000;
constexpr int kPeriodicCheckIntervalMs = 4 * 60 * 60 * 1000; // 4 hours
constexpr int kHeaderTimeoutMs = 30 * 1000;
constexpr int kStallTimeoutMs = 60 * 1000;
constexpr int kMaxAttempts = 3;

bool isAllowedUpdateUrl(const QUrl& url) {
    if (!url.isValid()) return false;
    if (url.scheme() == QStringLiteral("https")) return !url.host().isEmpty();
    if (url.scheme() == QStringLiteral("http")) {
        const QString host = url.host();
        if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return true;
        QHostAddress addr;
        return addr.setAddress(host) && addr.isLoopback();
    }
    return false;
}

bool isTransientHttpStatus(int status) noexcept {
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

int retryDelayMs(int attempt) noexcept {
    return attempt <= 1 ? 1500 : 4000;
}

std::filesystem::path toFsPath(const QString& qpath) {
#if defined(Q_OS_WIN)
    return std::filesystem::path(qpath.toStdWString());
#else
    return std::filesystem::path(qpath.toUtf8().constData());
#endif
}

} // namespace

UpdateClient::UpdateClient(QObject* parent) : QObject(parent) {
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    startup_timer_.setSingleShot(true);
    startup_timer_.setInterval(kStartupCheckDelayMs);
    connect(&startup_timer_, &QTimer::timeout, this, [this] {
        if (auto_check_) checkForUpdates(false);
    });

    periodic_timer_.setInterval(kPeriodicCheckIntervalMs);
    connect(&periodic_timer_, &QTimer::timeout, this, [this] {
        if (auto_check_) checkForUpdates(false);
    });

    header_timer_.setSingleShot(true);
    header_timer_.setInterval(kHeaderTimeoutMs);
    connect(&header_timer_, &QTimer::timeout, this, [this] {
        if (active_reply_ && !header_received_) {
            active_reply_->setProperty("p2pTimeoutReason", QStringLiteral("连接更新服务器超时"));
            active_reply_->abort();
        }
    });

    stall_timer_.setSingleShot(true);
    stall_timer_.setInterval(kStallTimeoutMs);
    connect(&stall_timer_, &QTimer::timeout, this, [this] {
        if (active_reply_) {
            active_reply_->setProperty("p2pTimeoutReason", QStringLiteral("下载更新超时，连接长时间无响应"));
            active_reply_->abort();
        }
    });

    retry_timer_.setSingleShot(true);
}

UpdateClient::~UpdateClient() {
    cancelActiveReply();
}

QString UpdateClient::currentVersion() {
    return QString::fromUtf8(P2PREMOTE_VERSION);
}

QString UpdateClient::defaultFeedUrl() {
    return QString::fromUtf8(P2PREMOTE_UPDATE_FEED_URL);
}

QString UpdateClient::embeddedPublicKey() {
    return QString::fromUtf8(P2PREMOTE_UPDATE_PUBLIC_KEY).trimmed();
}

bool UpdateClient::isConfigured() const {
    const QString key = key_override_.isEmpty() ? embeddedPublicKey() : key_override_;
    const QString feed = feed_override_.isEmpty() ? defaultFeedUrl() : feed_override_;
    return !feed.isEmpty() && p2premote::update::ParsePublicKeyBase64(key.toStdString()).has_value();
}

bool UpdateClient::isSelfUpdateSupported() const {
    return isConfigured() && isPlatformSelfUpdateSupported();
}

QString UpdateClient::skippedVersion() const {
    return QSettings().value(QStringLiteral("updater/skippedVersion")).toString().trimmed();
}

void UpdateClient::setConfigOverride(QString feed_url, QString public_key) {
    feed_override_ = std::move(feed_url);
    key_override_ = std::move(public_key);
}

void UpdateClient::applySettings(bool auto_check, bool auto_download, const QString& mirror_url) {
    auto_check_ = auto_check;
    auto_download_ = auto_download;
    mirror_url_ = mirror_url.trimmed();
    if (!auto_check_) {
        startup_timer_.stop();
        periodic_timer_.stop();
    } else if (isConfigured() && !periodic_timer_.isActive()) {
        periodic_timer_.start();
    }
}

void UpdateClient::startPeriodicChecks() {
    if (!auto_check_ || !isConfigured()) return;
    startup_timer_.start();
    periodic_timer_.start();
}

QString UpdateClient::resolveUrl(const QString& raw_url) const {
    if (mirror_url_.isEmpty()) return raw_url;
    // Mirror can be either a full manifest URL (for feed override) or a prefix such as "https://mirror.example/".
    if (mirror_url_.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) &&
        raw_url.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        return mirror_url_;
    }
    if (raw_url.startsWith(QStringLiteral("https://github.com/"), Qt::CaseInsensitive) ||
        raw_url.startsWith(QStringLiteral("https://objects.githubusercontent.com/"), Qt::CaseInsensitive)) {
        QString prefix = mirror_url_;
        if (!prefix.endsWith(u'/')) prefix.append(u'/');
        return prefix + raw_url;
    }
    return raw_url;
}

void UpdateClient::setState(UpdateState next, QString message) {
    state_ = next;
    status_message_ = std::move(message);
    emit stateChanged();
}

void UpdateClient::cancelActiveReply() {
    header_timer_.stop();
    stall_timer_.stop();
    retry_timer_.stop();
    if (active_reply_) {
        auto* reply = active_reply_.data();
        active_reply_.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (download_file_) {
        download_file_->close();
        download_file_.reset();
    }
}

void UpdateClient::checkForUpdates(bool user_initiated) {
    if (state_ == UpdateState::Downloading || state_ == UpdateState::Verifying || state_ == UpdateState::Installing) {
        return;
    }
    user_initiated_ = user_initiated;
    if (user_initiated) banner_dismissed_ = false;
    if (!isConfigured()) {
        if (user_initiated) {
            setState(UpdateState::Error, QStringLiteral("当前构建未启用签名自动更新（缺少内置更新公钥或发布地址）。"));
        }
        return;
    }
    cancelActiveReply();
    setState(UpdateState::Checking, QStringLiteral("正在检查更新…"));
    fetchManifestAttempt(1, user_initiated);
}

void UpdateClient::fetchManifestAttempt(int attempt, bool user_initiated) {
    const QString raw_feed = feed_override_.isEmpty() ? defaultFeedUrl() : feed_override_;
    const QUrl url(resolveUrl(raw_feed));
    if (!isAllowedUpdateUrl(url)) {
        if (user_initiated) {
            setState(UpdateState::Error, QStringLiteral("更新地址必须使用 HTTPS。"));
        } else {
            setState(UpdateState::Idle);
        }
        return;
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "p2premote-updater");
    req.setRawHeader("Cache-Control", "no-cache");

    header_received_ = false;
    auto* reply = network_.get(req);
    active_reply_ = reply;
    header_timer_.start();

    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (reply != active_reply_) return;
        header_received_ = true;
        header_timer_.stop();
        stall_timer_.start();
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != active_reply_) return;
        header_received_ = true;
        header_timer_.stop();
        stall_timer_.start();
        if (reply->bytesAvailable() > static_cast<qint64>(p2premote::update::kMaxManifestBytes)) {
            reply->setProperty("p2pTimeoutReason", QStringLiteral("更新清单体积超出限制"));
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt, user_initiated] {
        if (reply != active_reply_) {
            reply->deleteLater();
            return;
        }
        header_timer_.stop();
        stall_timer_.stop();
        active_reply_.clear();

        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto net_err = reply->error();
        const QString reason = reply->property("p2pTimeoutReason").toString();
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        if (net_err != QNetworkReply::NoError || http_status != 200) {
            if (attempt < kMaxAttempts && (net_err != QNetworkReply::OperationCanceledError || !reason.isEmpty()) &&
                (http_status == 0 || isTransientHttpStatus(http_status))) {
                retry_timer_.disconnect(this);
                connect(&retry_timer_, &QTimer::timeout, this, [this, attempt, user_initiated] {
                    fetchManifestAttempt(attempt + 1, user_initiated);
                });
                retry_timer_.start(retryDelayMs(attempt));
                return;
            }
            if (user_initiated) {
                const QString detail = !reason.isEmpty() ? reason
                    : (http_status > 0 ? QStringLiteral("服务器返回 HTTP %1").arg(http_status)
                                       : QStringLiteral("无法连接更新服务器"));
                setState(UpdateState::Error, QStringLiteral("检查更新失败：%1").arg(detail));
            } else {
                setState(UpdateState::Idle);
            }
            return;
        }

        std::string parse_err;
        auto parsed = p2premote::update::ParseUpdateManifest(
            std::string_view(body.constData(), static_cast<std::size_t>(body.size())), &parse_err);
        if (!parsed) {
            if (user_initiated) {
                setState(UpdateState::Error, QStringLiteral("更新清单格式无效：%1").arg(QString::fromUtf8(parse_err)));
            } else {
                setState(UpdateState::Idle);
            }
            return;
        }
        const QString pubkey = key_override_.isEmpty() ? embeddedPublicKey() : key_override_;
        if (!p2premote::update::VerifyUpdateManifest(*parsed, P2PREMOTE_UPDATE_TARGET, pubkey.toStdString())) {
            setState(user_initiated ? UpdateState::Error : UpdateState::Idle,
                     user_initiated ? QStringLiteral("更新清单签名无效或平台不匹配。") : QString{});
            return;
        }

        if (p2premote::update::CompareVersions(parsed->version, currentVersion().toStdString()) <= 0) {
            available_.reset();
            setState(UpdateState::UpToDate, QStringLiteral("当前已是最新版本（v%1）").arg(currentVersion()));
            return;
        }

        const QString next_ver = QString::fromStdString(parsed->version);
        if (!user_initiated && !skippedVersion().isEmpty() && skippedVersion() == next_ver) {
            setState(UpdateState::Idle);
            return;
        }

        available_ = std::move(*parsed);
        downloaded_bytes_ = 0;
        total_bytes_ = available_->size;

        // Reuse an already verified staged archive if present.
        const QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const QDir ver_dir(QDir(cache_root).filePath(QStringLiteral("updates/%1").arg(next_ver)));
        const auto existing_archive = toFsPath(ver_dir.filePath(QStringLiteral("update.tar.gz")));
        const auto existing_manifest = toFsPath(ver_dir.filePath(QStringLiteral("manifest.json")));
        if (std::filesystem::exists(existing_archive) &&
            p2premote::update::VerifyArchiveFile(existing_archive, available_->size, available_->signature,
                                                 pubkey.toStdString(), nullptr)) {
            staged_archive_ = existing_archive;
            staged_manifest_ = existing_manifest;
            downloaded_bytes_ = available_->size;
            finishDownloadVerification();
            return;
        }

        setState(UpdateState::Available, QStringLiteral("发现新版本 v%1").arg(next_ver));
        if (!user_initiated && auto_download_) {
            downloadUpdate();
        }
    });
}

std::optional<UpdateClient::DeltaDownload> UpdateClient::findDelta() const {
    const auto current = currentVersion().toStdString();
    const auto key = (key_override_.isEmpty() ? embeddedPublicKey() : key_override_).toStdString();
    const QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const auto path = toFsPath(cache.filePath(QStringLiteral("updates/%1/update.tar.gz").arg(currentVersion())));
    for (const auto& patch : available_->deltas) {
        if (patch.from != current || patch.size >= available_->size ||
            !isAllowedUpdateUrl(QUrl(resolveUrl(QString::fromStdString(patch.url))))) continue;
        for (const auto& base : available_->previous) {
            if (base.version == current &&
                p2premote::update::VerifyArchiveFile(path, base.size, base.signature, key)) {
                return DeltaDownload{patch, path};
            }
        }
    }
    return std::nullopt;
}

bool UpdateClient::fallbackToFullArchive() {
    if (!delta_) return false;
    delta_.reset();
    downloaded_bytes_ = 0;
    total_bytes_ = available_->size;
    setState(UpdateState::Downloading, QStringLiteral("增量更新不可用，正在下载完整更新包…"));
    emit progressChanged(downloaded_bytes_, total_bytes_);
    downloadArchiveAttempt(1);
    return true;
}

void UpdateClient::downloadUpdate() {
    if (!available_) return;
    if (state_ == UpdateState::Downloading || state_ == UpdateState::Verifying || state_ == UpdateState::Installing) {
        return;
    }
    cancelActiveReply();
    delta_ = findDelta();
    downloaded_bytes_ = 0;
    total_bytes_ = delta_ ? delta_->patch.size : available_->size;
    setState(UpdateState::Downloading,
             (delta_ ? QStringLiteral("正在下载 v%1 的增量更新…") : QStringLiteral("正在下载 v%1…"))
                 .arg(QString::fromStdString(available_->version)));
    downloadArchiveAttempt(1);
}

void UpdateClient::downloadArchiveAttempt(int attempt) {
    if (!available_) return;
    const QUrl url(resolveUrl(QString::fromStdString(delta_ ? delta_->patch.url : available_->url)));
    if (!isAllowedUpdateUrl(url)) {
        if (fallbackToFullArchive()) return;
        setState(UpdateState::Error, QStringLiteral("更新包下载地址必须使用 HTTPS。"));
        return;
    }

    const QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString ver_str = QString::fromStdString(available_->version);
    const QDir ver_dir(QDir(cache_root).filePath(QStringLiteral("updates/%1").arg(ver_str)));
    if (!QDir().mkpath(ver_dir.absolutePath())) {
        setState(UpdateState::Error, QStringLiteral("无法创建本地更新缓存目录。"));
        return;
    }
    const QString part_path = ver_dir.filePath(delta_ ? QStringLiteral("update.delta.part") : QStringLiteral("update.tar.gz.part"));
    download_file_ = std::make_unique<QFile>(part_path);
    if (!download_file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        download_file_.reset();
        if (fallbackToFullArchive()) return;
        setState(UpdateState::Error, QStringLiteral("无法写入临时更新文件。"));
        return;
    }
    downloaded_bytes_ = 0;

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "p2premote-updater");

    header_received_ = false;
    auto* reply = network_.get(req);
    active_reply_ = reply;
    header_timer_.start();

    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (reply != active_reply_) return;
        header_received_ = true;
        header_timer_.stop();
        stall_timer_.start();
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != active_reply_ || !download_file_ || !available_) return;
        header_received_ = true;
        header_timer_.stop();
        stall_timer_.start();
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) return;
        downloaded_bytes_ += chunk.size();
        if (downloaded_bytes_ > total_bytes_) {
            reply->setProperty("p2pTimeoutReason", QStringLiteral("下载体积超出清单声明大小"));
            reply->abort();
            return;
        }
        if (download_file_->write(chunk) != chunk.size()) {
            reply->setProperty("p2pTimeoutReason", QStringLiteral("写入磁盘失败"));
            reply->abort();
            return;
        }
        emit progressChanged(downloaded_bytes_, total_bytes_);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt, ver_dir, part_path] {
        if (reply != active_reply_) {
            reply->deleteLater();
            return;
        }
        header_timer_.stop();
        stall_timer_.stop();
        active_reply_.clear();

        if (download_file_) {
            download_file_->flush();
            download_file_->close();
            download_file_.reset();
        }

        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto net_err = reply->error();
        const QString reason = reply->property("p2pTimeoutReason").toString();
        reply->deleteLater();

        if (net_err != QNetworkReply::NoError || http_status != 200) {
            QFile::remove(part_path);
            if (fallbackToFullArchive()) return;
            if (attempt < kMaxAttempts && (http_status == 0 || isTransientHttpStatus(http_status))) {
                retry_timer_.disconnect(this);
                connect(&retry_timer_, &QTimer::timeout, this, [this, attempt] {
                    downloadArchiveAttempt(attempt + 1);
                });
                retry_timer_.start(retryDelayMs(attempt));
                return;
            }
            const QString detail = !reason.isEmpty() ? reason
                : (http_status > 0 ? QStringLiteral("HTTP %1").arg(http_status) : QStringLiteral("网络连接中断"));
            setState(UpdateState::Error, QStringLiteral("下载更新失败：%1").arg(detail));
            return;
        }

        const QString final_archive = ver_dir.filePath(QStringLiteral("update.tar.gz"));
        const QString final_manifest = ver_dir.filePath(QStringLiteral("manifest.json"));
        QString replacement = part_path;
        if (delta_) {
            const auto key = (key_override_.isEmpty() ? embeddedPublicKey() : key_override_).toStdString();
            replacement = ver_dir.filePath(QStringLiteral("update.reconstructed.part"));
            const auto result = toFsPath(replacement);
            if (!p2premote::update::VerifyArchiveFile(toFsPath(part_path), delta_->patch.size,
                                                     delta_->patch.signature, key) ||
                !p2premote::update::ApplyBsdiffPatch(delta_->base_archive, toFsPath(part_path), result, available_->size) ||
                !p2premote::update::VerifyArchiveFile(result, available_->size, available_->signature, key)) {
                QFile::remove(part_path);
                QFile::remove(replacement);
                fallbackToFullArchive();
                return;
            }
            QFile::remove(part_path);
        }
        QFile::remove(final_archive);
        if (!QFile::rename(replacement, final_archive)) {
            QFile::remove(replacement);
            if (fallbackToFullArchive()) return;
            setState(UpdateState::Error, QStringLiteral("无法保存下载完成的更新包。"));
            return;
        }
        staged_archive_ = toFsPath(final_archive);
        staged_manifest_ = toFsPath(final_manifest);
        delta_.reset();
        finishDownloadVerification();
    });
}

void UpdateClient::finishDownloadVerification() {
    if (!available_) return;
    setState(UpdateState::Verifying, QStringLiteral("正在校验更新包签名…"));

    const QString pubkey = key_override_.isEmpty() ? embeddedPublicKey() : key_override_;
    std::string verify_err;
    if (!p2premote::update::VerifyArchiveFile(staged_archive_, available_->size, available_->signature,
                                              pubkey.toStdString(), &verify_err)) {
        std::error_code ec;
        std::filesystem::remove(staged_archive_, ec);
        staged_archive_.clear();
        staged_manifest_.clear();
        setState(UpdateState::Error,
                 QStringLiteral("更新包安全校验失败，已拒绝安装（%1）。").arg(QString::fromUtf8(verify_err)));
        return;
    }

    // Persist verified manifest next to the archive for privileged system-service re-verification.
    {
        std::ofstream mf(staged_manifest_, std::ios::binary | std::ios::trunc);
        if (!mf || !(mf << p2premote::update::SerializeUpdateManifest(*available_))) {
            setState(UpdateState::Error, QStringLiteral("无法保存更新清单文件。"));
            return;
        }
    }

    setState(UpdateState::ReadyToInstall,
             QStringLiteral("新版本 v%1 已就绪，点击即可重启更新")
                 .arg(QString::fromStdString(available_->version)));
}

void UpdateClient::installUpdate() {
    if (state_ != UpdateState::ReadyToInstall || !available_ || staged_archive_.empty()) return;
    if (is_busy_ && is_busy_()) {
        setState(UpdateState::ReadyToInstall,
                 QStringLiteral("当前正在远程控制会话中，请断开会话后再点击重启更新。"));
        return;
    }

    setState(UpdateState::Installing, QStringLiteral("正在应用更新，请稍候…"));
    const bool has_service = has_service_ ? has_service_() : false;
    const auto res = applyPlatformUpdate(staged_archive_, staged_manifest_, *available_, has_service);
    if (!res.ok) {
        setState(UpdateState::Error,
                 res.error.isEmpty() ? QStringLiteral("安装更新失败，原版本已保留。") : res.error);
        return;
    }
    if (res.relaunch_requested) {
        emit relaunchRequested();
    }
}

void UpdateClient::skipAvailableVersion() {
    if (!available_) return;
    QSettings().setValue(QStringLiteral("updater/skippedVersion"),
                         QString::fromStdString(available_->version));
    banner_dismissed_ = true;
    setState(UpdateState::Idle);
}

void UpdateClient::dismissBanner() {
    banner_dismissed_ = true;
    if (state_ == UpdateState::Available || state_ == UpdateState::Error) {
        setState(UpdateState::Idle);
    }
}
