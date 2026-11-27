#include "plugin_platform.h"

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QUrl>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

#include <sys/statvfs.h>
#include <sys/wait.h>

namespace p2premote {

struct ClipboardBridge::State {
    std::mutex mutex;
    std::string text;
    std::vector<LocalClipboardFile> files;
    std::string_view issue;
    std::atomic_uint64_t change_revision{};
    std::atomic_uint64_t local_revision{};
    std::atomic_uint file_watchers{};
    bool clearing_files{}; // GUI thread only
    bool publishing_files{}; // GUI thread only
};

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::weak_ptr<ClipboardBridge::State> clipboard_state;
constexpr std::size_t clipboard_limit = 60 * 1024;
constexpr unsigned read_window = 256 * 1024;
constexpr auto read_timeout = 15s;
constexpr auto max_position = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::string_view mount_dir_prefix = "p2premote-clipboard-";

const auto uri_format = QStringLiteral("text/uri-list");
const auto gnome_format = QStringLiteral("x-special/gnome-copied-files");
const auto kde_format = QStringLiteral("application/x-kde-cutselection");
const auto remote_format = QStringLiteral("application/x-p2premote-files");

timespec currentTimespec() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

std::string runtimeMountBase() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && runtime[0] == '/') {
        struct stat st{};
        if (::stat(runtime, &st) == 0 && S_ISDIR(st.st_mode) && ::access(runtime, R_OK | W_OK | X_OK) == 0) {
            return QDir::cleanPath(QString::fromLocal8Bit(runtime)).toStdString();
        }
    }
    return QDir::cleanPath(QDir::tempPath()).toStdString();
}

std::vector<std::string> candidateMountBases() {
    std::vector<std::string> bases;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && runtime[0] == '/') {
        bases.push_back(QDir::cleanPath(QString::fromLocal8Bit(runtime)).toStdString());
    }
    const auto tmp = QDir::cleanPath(QDir::tempPath()).toStdString();
    if (std::ranges::find(bases, tmp) == bases.end()) bases.push_back(tmp);
    return bases;
}

bool isRemoteMountPath(const QString& path) {
    for (const auto& base : candidateMountBases()) {
        const auto prefix = QString::fromStdString(base + "/" + std::string(mount_dir_prefix));
        if (path.startsWith(prefix)) return true;
    }
    return false;
}

void runFusermountLazy(const std::string& mountpoint) noexcept {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        ::execlp("fusermount3", "fusermount3", "-uz", mountpoint.c_str(), static_cast<char*>(nullptr));
        ::execlp("fusermount", "fusermount", "-uz", mountpoint.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (pid > 0) {
        int status{};
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
}

void cleanupStaleMounts() noexcept {
    const pid_t self = ::getpid();
    for (const auto& base : candidateMountBases()) {
        DIR* dir = ::opendir(base.c_str());
        if (!dir) continue;
        while (const dirent* entry = ::readdir(dir)) {
            const std::string_view name(entry->d_name);
            if (!name.starts_with(mount_dir_prefix)) continue;
            const auto rest = name.substr(mount_dir_prefix.size());
            const auto dash = rest.find('-');
            if (dash == std::string_view::npos || dash == 0) continue;
            pid_t owner{};
            const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + dash, owner);
            if (ec != std::errc{} || ptr != rest.data() + dash || owner <= 0 || owner == self) continue;
            if (::kill(owner, 0) == -1 && errno == ESRCH) {
                const auto full = base + "/" + std::string(name);
                if (::rmdir(full.c_str()) != 0) {
                    runFusermountLazy(full);
                    ::rmdir(full.c_str());
                }
            }
        }
        ::closedir(dir);
    }
}

bool isWebOrNonFileUri(const QUrl& url) {
    if (!url.isValid()) return false;
    const auto scheme = url.scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https") ||
        scheme == QStringLiteral("mailto") || scheme == QStringLiteral("ftp") ||
        scheme == QStringLiteral("ftps") || scheme == QStringLiteral("about") ||
        scheme == QStringLiteral("javascript") || scheme == QStringLiteral("data") ||
        scheme == QStringLiteral("tel");
}

struct LocalSelection {
    std::vector<LocalClipboardFile> files;
    std::string_view issue;
    bool own_remote{};
};

LocalSelection readLocalFiles(const QMimeData& mime) {
    QList<QUrl> urls;
    if (mime.hasUrls()) {
        urls = mime.urls();
    } else {
        const auto data = mime.data(gnome_format);
        if (data.size() > static_cast<qsizetype>(clipboard_limit)) return {{}, "file_list_unreadable", false};
        const auto lines = data.split('\n');
        const auto action = lines.empty() ? QByteArray{} : lines.front().trimmed();
        if (action != "copy" && action != "cut") return {{}, "file_list_unreadable", false};
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const auto line = lines[i].trimmed();
            if (!line.isEmpty()) urls.push_back(QUrl::fromEncoded(line, QUrl::StrictMode));
        }
    }
    if (urls.size() > static_cast<qsizetype>(max_clipboard_files)) return {{}, "file_list_unreadable", false};
    LocalSelection selection;
    std::set<QString> seen_names;
    for (const auto& url : urls) {
        if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host() != QStringLiteral("localhost")) ||
            url.hasQuery() || url.hasFragment()) return {{}, "external_files", false};
        const auto path = QDir::cleanPath(url.host().isEmpty() ? url.toLocalFile() : url.path());
        if (isRemoteMountPath(path)) return {{}, {}, true};
        const auto name = path.section(u'/', -1);
        if (!path.startsWith(u'/') || path.startsWith(QStringLiteral("//")) || path.contains(QChar::Null) ||
            name.isEmpty() || name.contains(u'\\') || !seen_names.emplace(name.toCaseFolded()).second)
            return {{}, "file_list_unreadable", false};
        selection.files.push_back({path.toStdString(), name.toStdString()});
    }
    return selection;
}

void readClipboard(ClipboardBridge::State& state) {
    const auto* mime = QApplication::clipboard()->mimeData();
    const bool remote = mime && mime->hasFormat(remote_format);
    bool has_file_urls = false;
    if (mime && !remote && mime->hasUrls()) {
        const auto urls = mime->urls();
        has_file_urls = !urls.isEmpty() &&
            std::ranges::any_of(urls, [](const QUrl& url) { return !isWebOrNonFileUri(url); });
    }
    const bool file_data = mime && (remote || mime->hasFormat(gnome_format) || has_file_urls);
    const auto text = !file_data && mime && mime->hasText() ? mime->text().toUtf8() : QByteArray{};
    LocalSelection selection;
    if (file_data && !remote) {
        selection = readLocalFiles(*mime);
        if (!state.file_watchers.load()) {
            selection.files.clear();
            selection.issue = {};
        }
    }
    const bool own_remote = remote || selection.own_remote;
    const std::lock_guard lock(state.mutex);
    state.text = static_cast<std::size_t>(text.size()) <= clipboard_limit ? text.toStdString() : std::string{};
    state.files = std::move(selection.files);
    state.issue = selection.issue;
    if (!state.clearing_files) ++state.change_revision;
    if (!state.clearing_files && !state.publishing_files && !own_remote) ++state.local_revision;
}

void clearClipboard(ClipboardBridge::State& state, QClipboard& clipboard) {
    state.clearing_files = true;
    clipboard.clear();
    state.clearing_files = false;
}

struct Publication {
    std::atomic_bool active{true};
};

class ClipboardMimeData : public QMimeData {
public:
    ClipboardMimeData(std::shared_ptr<Publication> publication, const QList<QUrl>& urls)
        : publication(std::move(publication)) {
        QByteArray uri_list;
        QByteArray gnome_list("copy");
        for (const auto& url : urls) {
            const auto encoded = url.toEncoded();
            uri_list += encoded + "\r\n";
            gnome_list += "\n" + encoded;
        }
        setData(uri_format, uri_list);
        setData(gnome_format, gnome_list);
        setData(kde_format, "0");
        setData(remote_format, "1");
    }

    const std::shared_ptr<Publication> publication;
};

struct VirtualNode {
    unsigned index{};
    std::uint64_t size{};
    bool directory{};
    ino_t ino{};
    nlink_t child_dirs{};
    std::vector<std::string> children;
};

struct VirtualOffer {
    std::uint32_t id{};
    timespec mtime{};
    std::uint64_t total_bytes{};
    std::unordered_map<std::string, VirtualNode> nodes;
    std::vector<std::string> root_names;
};

std::shared_ptr<VirtualOffer> describeOffer(std::uint32_t offer_id, const std::vector<ClipboardFile>& files) {
    if (!offer_id || files.empty() || files.size() > max_clipboard_files) return nullptr;
    auto offer = std::make_shared<VirtualOffer>();
    offer->id = offer_id;
    offer->mtime = currentTimespec();

    const auto make_ino = [offer_id](std::uint64_t seq) noexcept -> ino_t {
        return static_cast<ino_t>((static_cast<std::uint64_t>(offer_id) << 16) | seq);
    };
    offer->nodes.emplace("", VirtualNode{0, 0, true, make_ino(0), 0, {}});

    std::map<QString, QString> names;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        if (!file.directory && file.size > max_position) return nullptr;
        QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::ConvertInitialBom);
        QString name = decoder(QByteArrayView(file.name));
        if (decoder.hasError() || name.size() >= 260 || name.contains(u'/') ||
            std::ranges::any_of(name, [](QChar ch) { return ch.unicode() < 32; })) return nullptr;
        name.replace(u'\\', u'/');
        const auto slash = name.lastIndexOf(u'/');
        const auto leaf = name.sliced(slash + 1);
        if (leaf.isEmpty() || leaf == QStringLiteral(".") || leaf == QStringLiteral("..") ||
            leaf.endsWith(u'.') || leaf.endsWith(u' ') || leaf.toUtf8().size() > 255) return nullptr;
        QString parent_q;
        if (slash >= 0) {
            parent_q = name.first(slash);
            const auto parent = names.find(parent_q.toCaseFolded());
            // Require the exact path to a parent directory already created in this tree.
            if (parent == names.end() || parent->second.isEmpty() || parent->second != parent_q) return nullptr;
        }
        if (!names.emplace(name.toCaseFolded(), file.directory ? name : QString{}).second) return nullptr;

        const auto path = name.toStdString();
        const auto parent_path = parent_q.toStdString();
        const auto leaf_str = leaf.toStdString();
        const auto size = file.directory ? 0 : file.size;
        offer->total_bytes += std::min<std::uint64_t>(size, max_position - offer->total_bytes);

        auto& parent_node = offer->nodes.at(parent_path);
        parent_node.children.push_back(leaf_str);
        if (file.directory) ++parent_node.child_dirs;
        if (slash < 0) offer->root_names.push_back(leaf_str);

        offer->nodes.emplace(path, VirtualNode{
            static_cast<unsigned>(i + 1),
            size,
            file.directory,
            make_ino(i + 1),
            0,
            {}
        });
    }
    return offer;
}

bool parseOfferPath(const char* raw_path, std::uint32_t& offer_id, std::string_view& rel) noexcept {
    if (!raw_path || raw_path[0] != '/') return false;
    const std::string_view path(raw_path + 1);
    if (path.empty()) return false;
    const auto slash = path.find('/');
    const auto head = slash == std::string_view::npos ? path : path.substr(0, slash);
    if (head.empty() || (head.size() > 1 && head[0] == '0')) return false;
    std::uint32_t parsed{};
    const auto [ptr, ec] = std::from_chars(head.data(), head.data() + head.size(), parsed);
    if (ec != std::errc{} || ptr != head.data() + head.size() || parsed == 0) return false;
    offer_id = parsed;
    rel = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    return true;
}

struct OpenHandle {
    std::uint32_t offer{};
    unsigned index{};
    std::uint64_t size{};
    std::mutex mutex;
    std::uint64_t cache_start{};
    std::string cache;
};

} // namespace

struct FileClipboard::State {
    explicit State(std::function<void()> wake)
        : wake(std::move(wake)), uid(::getuid()), gid(::getgid()), mount_time(currentTimespec()) {}

    struct Range {
        ClipboardReadRequest request;
        std::string data;
        bool delivered{};
        bool failed{};
    };

    void Wake() noexcept {
        if (wake) wake();
    }

    // Called with mutex held.
    void ReportError(std::string_view code) noexcept {
        error = code;
        Wake();
    }

    std::shared_ptr<const VirtualOffer> ActiveOffer(std::uint32_t expected_id = 0) const {
        const std::lock_guard lock(mutex);
        if (!available || !active_offer) return nullptr;
        if (expected_id && active_offer->id != expected_id) return nullptr;
        return active_offer;
    }

    std::optional<std::string> Read(std::uint32_t offer, unsigned index, std::uint64_t offset, unsigned size) {
        std::unique_lock rpc_lock(rpc_mutex);
        std::unique_lock lock(mutex);
        if (!available || !active_offer || active_offer->id != offer || range) return std::nullopt;
        if (++next_read == 0) ++next_read;
        Range next{{next_read, offer, index, offset, size}, {}, false, false};
        next.data.reserve(size);
        range.emplace(std::move(next));
        Wake();

        auto deadline = Clock::now() + read_timeout;
        std::size_t received{};
        while (available && active_offer && active_offer->id == offer && !range->failed && range->data.size() < size) {
            if (received != range->data.size()) {
                received = range->data.size();
                deadline = Clock::now() + read_timeout;
            }
            if (Clock::now() >= deadline) {
                ReportError("transfer_timeout");
                break;
            }
            response_cv.wait_until(lock, deadline);
        }
        std::optional<std::string> result;
        if (available && active_offer && active_offer->id == offer && !range->failed && range->data.size() == size) {
            result = std::move(range->data);
        }
        range.reset();
        return result;
    }

    std::function<void()> wake;
    const uid_t uid;
    const gid_t gid;
    const timespec mount_time;
    std::mutex rpc_mutex;
    mutable std::mutex mutex;
    std::condition_variable response_cv;
    bool available{true};
    std::shared_ptr<const VirtualOffer> active_offer;
    std::shared_ptr<Publication> publication;
    std::uint32_t next_read{};
    std::optional<Range> range;
    std::string_view error;
};

namespace {

FileClipboard::State* fuseState() noexcept {
    const auto* ctx = fuse_get_context();
    return ctx ? static_cast<FileClipboard::State*>(ctx->private_data) : nullptr;
}

void* fuseInit(struct fuse_conn_info*, struct fuse_config* cfg) {
    if (cfg) {
        cfg->entry_timeout = 0;
        cfg->attr_timeout = 0;
        cfg->negative_timeout = 0;
        cfg->use_ino = 1;
    }
    return fuse_get_context()->private_data;
}

int fuseGetattr(const char* path, struct stat* st, struct fuse_file_info*) {
    auto* state = fuseState();
    if (!state || !path || !st) return -EIO;
    std::memset(st, 0, sizeof(*st));
    st->st_uid = state->uid;
    st->st_gid = state->gid;
    if (std::strcmp(path, "/") == 0) {
        const auto offer = state->ActiveOffer();
        st->st_mode = S_IFDIR | 0500;
        st->st_ino = 1;
        st->st_nlink = offer ? 3 : 2;
        st->st_atim = st->st_mtim = st->st_ctim = state->mount_time;
        return 0;
    }
    std::uint32_t offer_id{};
    std::string_view rel;
    if (!parseOfferPath(path, offer_id, rel)) return -ENOENT;
    const auto offer = state->ActiveOffer(offer_id);
    if (!offer) return -ENOENT;
    const auto it = offer->nodes.find(std::string(rel));
    if (it == offer->nodes.end()) return -ENOENT;
    const auto& node = it->second;
    st->st_ino = node.ino;
    st->st_atim = st->st_mtim = st->st_ctim = offer->mtime;
    if (node.directory) {
        st->st_mode = S_IFDIR | 0500;
        st->st_nlink = 2 + node.child_dirs;
    } else {
        st->st_mode = S_IFREG | 0400;
        st->st_nlink = 1;
        st->st_size = static_cast<off_t>(node.size);
        st->st_blocks = static_cast<blkcnt_t>((node.size + 511) / 512);
    }
    return 0;
}

int fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t,
    struct fuse_file_info*, enum fuse_readdir_flags) {
    auto* state = fuseState();
    if (!state || !path) return -EIO;
    const auto fill_flags = static_cast<fuse_fill_dir_flags>(0);
    if (std::strcmp(path, "/") == 0) {
        filler(buf, ".", nullptr, 0, fill_flags);
        filler(buf, "..", nullptr, 0, fill_flags);
        if (const auto offer = state->ActiveOffer()) {
            struct stat st{};
            st.st_mode = S_IFDIR | 0500;
            st.st_ino = offer->nodes.at("").ino;
            const auto name = std::to_string(offer->id);
            filler(buf, name.c_str(), &st, 0, fill_flags);
        }
        return 0;
    }
    std::uint32_t offer_id{};
    std::string_view rel;
    if (!parseOfferPath(path, offer_id, rel)) return -ENOENT;
    const auto offer = state->ActiveOffer(offer_id);
    if (!offer) return -ENOENT;
    const auto it = offer->nodes.find(std::string(rel));
    if (it == offer->nodes.end()) return -ENOENT;
    if (!it->second.directory) return -ENOTDIR;
    filler(buf, ".", nullptr, 0, fill_flags);
    filler(buf, "..", nullptr, 0, fill_flags);
    for (const auto& child : it->second.children) {
        const auto child_key = rel.empty() ? child : std::string(rel) + "/" + child;
        const auto child_it = offer->nodes.find(child_key);
        if (child_it == offer->nodes.end()) continue;
        struct stat st{};
        st.st_ino = child_it->second.ino;
        st.st_mode = child_it->second.directory ? (S_IFDIR | 0500) : (S_IFREG | 0400);
        filler(buf, child.c_str(), &st, 0, fill_flags);
    }
    return 0;
}

int fuseOpen(const char* path, struct fuse_file_info* fi) {
    auto* state = fuseState();
    if (!state || !path || !fi) return -EIO;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    std::uint32_t offer_id{};
    std::string_view rel;
    if (!parseOfferPath(path, offer_id, rel)) return -ENOENT;
    const auto offer = state->ActiveOffer(offer_id);
    if (!offer) return -ENOENT;
    const auto it = offer->nodes.find(std::string(rel));
    if (it == offer->nodes.end()) return -ENOENT;
    if (it->second.directory) return -EISDIR;
    auto* handle = new (std::nothrow) OpenHandle{offer->id, it->second.index, it->second.size, {}, 0, {}};
    if (!handle) return -ENOMEM;
    fi->fh = reinterpret_cast<std::uint64_t>(handle);
    return 0;
}

int fuseRead(const char*, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    auto* state = fuseState();
    if (!state || !fi || !fi->fh) return -EIO;
    if (offset < 0) return -EINVAL;
    auto* handle = reinterpret_cast<OpenHandle*>(fi->fh);
    const auto start = static_cast<std::uint64_t>(offset);
    if (start >= handle->size || size == 0) return 0;
    const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(size, handle->size - start));
    const std::lock_guard handle_lock(handle->mutex);
    std::size_t done{};
    while (done < to_read) {
        const auto pos = start + done;
        if (pos < handle->cache_start || pos - handle->cache_start >= handle->cache.size()) {
            const auto window = static_cast<unsigned>(std::min<std::uint64_t>(read_window, handle->size - pos));
            auto data = state->Read(handle->offer, handle->index, pos, window);
            if (!data) return -EIO;
            handle->cache_start = pos;
            handle->cache = std::move(*data);
        }
        const auto cache_off = static_cast<std::size_t>(pos - handle->cache_start);
        const auto chunk = std::min(to_read - done, handle->cache.size() - cache_off);
        std::memcpy(buf + done, handle->cache.data() + cache_off, chunk);
        done += chunk;
    }
    return static_cast<int>(done);
}

int fuseRelease(const char*, struct fuse_file_info* fi) {
    if (fi && fi->fh) {
        delete reinterpret_cast<OpenHandle*>(fi->fh);
        fi->fh = 0;
    }
    return 0;
}

int fuseStatfs(const char*, struct statvfs* st) {
    auto* state = fuseState();
    if (!state || !st) return -EIO;
    std::memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_namemax = 255;
    std::uint64_t bytes = 4096;
    if (const auto offer = state->ActiveOffer()) {
        bytes = std::max<std::uint64_t>(bytes, offer->total_bytes);
    }
    st->f_blocks = static_cast<fsblkcnt_t>((bytes + 4095) / 4096);
    st->f_bfree = 0;
    st->f_bavail = 0;
    return 0;
}

} // namespace

struct FileClipboard::Mount {
    ~Mount() { stop(); }

    bool start(const std::shared_ptr<State>& state) {
        if (fuse) return true;
        std::string tmpl = runtimeMountBase() + "/" + std::string(mount_dir_prefix) +
            std::to_string(::getpid()) + "-XXXXXX";
        if (!::mkdtemp(tmpl.data())) return false;
        ::chmod(tmpl.c_str(), 0700);
        mountpoint = std::move(tmpl);

        char arg0[] = "p2premote-clipboard";
        char arg1[] = "-o";
        char arg2[] = "ro,nosuid,nodev,default_permissions,fsname=p2premote-clipboard";
        char* argv[] = {arg0, arg1, arg2};
        struct fuse_args args = FUSE_ARGS_INIT(3, argv);

        struct fuse_operations ops{};
        ops.init = fuseInit;
        ops.getattr = fuseGetattr;
        ops.readdir = fuseReaddir;
        ops.open = fuseOpen;
        ops.read = fuseRead;
        ops.release = fuseRelease;
        ops.statfs = fuseStatfs;

        fuse = fuse_new(&args, &ops, sizeof(ops), state.get());
        fuse_opt_free_args(&args);
        if (!fuse) {
            ::rmdir(mountpoint.c_str());
            mountpoint.clear();
            return false;
        }
        if (fuse_mount(fuse, mountpoint.c_str()) != 0) {
            fuse_destroy(fuse);
            fuse = nullptr;
            ::rmdir(mountpoint.c_str());
            mountpoint.clear();
            return false;
        }
        worker = std::thread([f = fuse] { fuse_loop_mt(f, 0); });
        return true;
    }

    void stop() noexcept {
        if (!fuse) return;
        fuse_exit(fuse);
        runFusermountLazy(mountpoint);
        if (worker.joinable()) worker.join();
        fuse_unmount(fuse);
        fuse_destroy(fuse);
        fuse = nullptr;
        if (!mountpoint.empty()) {
            ::rmdir(mountpoint.c_str());
            mountpoint.clear();
        }
    }

    std::string mountpoint;
    struct fuse* fuse{};
    std::thread worker;
};

void initializePluginClipboard() {
    cleanupStaleMounts();
    auto* clipboard = QApplication::clipboard();
    auto state = std::make_shared<ClipboardBridge::State>();
    clipboard_state = state;
    QObject::connect(clipboard, &QClipboard::dataChanged, clipboard, [state] { readClipboard(*state); });
    readClipboard(*state);
}

ClipboardBridge::ClipboardBridge()
    : state_(clipboard_state.lock()), write_generation_(std::make_shared<std::atomic_uint64_t>()) {}

ClipboardBridge::~ClipboardBridge() {
    CancelWrite();
    if (watch_files_ && state_) --state_->file_watchers;
}

bool ClipboardBridge::Available() const noexcept { return static_cast<bool>(state_); }

void ClipboardBridge::WatchFiles() noexcept {
    if (!state_ || watch_files_) return;
    revision_ = state_->change_revision.load();
    ++state_->file_watchers;
    watch_files_ = true;
}

bool ClipboardBridge::SetText(std::string_view text) {
    if (!state_ || text.size() > clipboard_limit) return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString value = decoder(QByteArrayView(text.data(), text.size()));
    if (decoder.hasError()) return false;
    auto token = write_generation_;
    const auto generation = token->load();
    return QMetaObject::invokeMethod(qApp, [token = std::move(token), generation, value] {
        if (token->load() == generation) QApplication::clipboard()->setText(value);
    }, Qt::QueuedConnection);
}

bool ClipboardBridge::Poll(std::uint64_t now) {
    if (!state_ || now < poll_at_) return false;
    poll_at_ = now + 100;
    const std::lock_guard lock(state_->mutex);
    const auto revision = state_->change_revision.load();
    const bool files_changed = watch_files_ && revision != revision_;
    if (files_changed) {
        files_ = state_->files;
        file_issue_ = state_->issue;
        revision_ = revision;
    }
    const bool text_changed = text_ != state_->text;
    if (text_changed) text_ = state_->text;
    return text_changed || files_changed;
}

unsigned long ClipboardBridge::Wait(std::uint64_t now) const noexcept {
    return poll_at_ > now ? static_cast<unsigned long>(poll_at_ - now) : 0;
}

void ClipboardBridge::CancelWrite() noexcept { ++*write_generation_; }

FileClipboard::FileClipboard(std::function<void()> wake)
    : clipboard_(clipboard_state.lock()), state_(std::make_shared<State>(std::move(wake))),
      mount_(std::make_unique<Mount>()) {}

FileClipboard::~FileClipboard() {
    Clear();
    {
        const std::lock_guard lock(state_->mutex);
        state_->available = false;
        if (state_->range) state_->range->failed = true;
    }
    state_->response_cv.notify_all();
    mount_->stop();
}

bool FileClipboard::Available() const noexcept { return static_cast<bool>(clipboard_); }

std::string_view FileClipboard::Publish(std::uint32_t offer_id, const std::vector<ClipboardFile>& files) {
    if (!clipboard_ || !offer_id) return "publish_failed";
    auto offer = describeOffer(offer_id, files);
    if (!offer) return "manifest_invalid";
    if (!mount_->start(state_)) return "mount_unavailable";

    QList<QUrl> roots;
    roots.reserve(static_cast<qsizetype>(offer->root_names.size()));
    const auto offer_prefix = mount_->mountpoint + "/" + std::to_string(offer_id) + "/";
    for (const auto& name : offer->root_names) {
        roots.push_back(QUrl::fromLocalFile(QString::fromStdString(offer_prefix + name)));
    }

    const auto local_revision = clipboard_->local_revision.load();
    auto publication = std::make_shared<Publication>();
    {
        const std::lock_guard lock(state_->mutex);
        if (!state_->available) return "publish_failed";
        if (state_->publication) state_->publication->active = false;
        if (state_->range) state_->range->failed = true;
        state_->active_offer = std::move(offer);
        state_->publication = publication;
        state_->error = {};
    }
    state_->response_cv.notify_all();

    const bool queued = qApp && QMetaObject::invokeMethod(qApp,
        [clip_state = clipboard_, state = state_, publication = std::move(publication),
         offer_id, roots = std::move(roots), local_revision] {
            if (!publication->active.load()) return;
            if (clip_state->local_revision.load() != local_revision) {
                {
                    const std::lock_guard lock(state->mutex);
                    if (state->active_offer && state->active_offer->id == offer_id) {
                        state->active_offer.reset();
                        if (state->range) state->range->failed = true;
                        state->ReportError("clipboard_changed");
                    }
                }
                state->response_cv.notify_all();
                return;
            }
            auto* clipboard = QApplication::clipboard();
            auto* mime = new ClipboardMimeData(publication, roots);
            clip_state->publishing_files = true;
            clipboard->setMimeData(mime);
            clip_state->publishing_files = false;
            const auto* current = clipboard->mimeData();
            const bool published = current == mime ||
                (current && (current->hasFormat(remote_format) || current->hasUrls()));
            if (!published) {
                {
                    const std::lock_guard lock(state->mutex);
                    if (state->active_offer && state->active_offer->id == offer_id) {
                        state->active_offer.reset();
                        if (state->range) state->range->failed = true;
                        state->ReportError("publish_failed");
                    }
                }
                state->response_cv.notify_all();
            } else if (!publication->active.load() && current == mime) {
                clearClipboard(*clip_state, *clipboard);
            }
        }, Qt::QueuedConnection);
    if (!queued) {
        Clear(offer_id);
        return "publish_failed";
    }
    return {};
}

void FileClipboard::Clear(std::uint32_t offer) {
    std::shared_ptr<Publication> publication;
    {
        const std::lock_guard lock(state_->mutex);
        if (!state_->active_offer || (offer && state_->active_offer->id != offer)) return;
        publication = std::move(state_->publication);
        if (publication) publication->active = false;
        state_->active_offer.reset();
        if (state_->range) state_->range->failed = true;
    }
    state_->response_cv.notify_all();
    if (qApp && !QCoreApplication::closingDown() && publication) {
        QMetaObject::invokeMethod(qApp, [state = clipboard_, publication = std::move(publication)] {
            auto* clipboard = QApplication::clipboard();
            const auto* mime = dynamic_cast<const ClipboardMimeData*>(clipboard->mimeData());
            if (mime && mime->publication == publication) clearClipboard(*state, *clipboard);
        }, Qt::QueuedConnection);
    }
}

std::string_view FileClipboard::TakeError() {
    const std::lock_guard lock(state_->mutex);
    return std::exchange(state_->error, std::string_view{});
}

std::optional<ClipboardReadRequest> FileClipboard::TakeRequest() {
    const std::lock_guard lock(state_->mutex);
    if (!state_->range || state_->range->delivered || !state_->active_offer ||
        state_->range->request.offer != state_->active_offer->id) return std::nullopt;
    state_->range->delivered = true;
    return state_->range->request;
}

bool FileClipboard::Reply(std::uint32_t id, std::optional<std::string_view> data) {
    const std::lock_guard lock(state_->mutex);
    const auto& range = state_->range;
    if (!range || range->request.id != id || !state_->active_offer ||
        range->request.offer != state_->active_offer->id || range->failed ||
        range->data.size() == range->request.size) return false;
    if (!data || data->empty() || data->size() > range->request.size - range->data.size()) {
        state_->range->failed = true;
        state_->response_cv.notify_all();
        return !data;
    }
    state_->range->data.append(*data);
    state_->response_cv.notify_all();
    return true;
}

} // namespace p2premote
