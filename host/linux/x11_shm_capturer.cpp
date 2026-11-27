#include "x11_shm_capturer.h"

#include <X11/Xauth.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace p2premote::linux_host {
namespace {

constexpr char kCursorMarker = 'C';
constexpr unsigned kCursorMaxDim = 128;
constexpr std::size_t kCursorHeaderBytes = 9;

template<class T>
using XcbReply = std::unique_ptr<T, decltype(&std::free)>;

bool SupportsBgra(const xcb_setup_t* setup, const xcb_screen_t* screen) {
    for (auto format = xcb_setup_pixmap_formats_iterator(setup); format.rem; xcb_format_next(&format)) {
        if (format.data->depth != screen->root_depth) continue;
        if (format.data->bits_per_pixel != 32 || format.data->scanline_pad != 32) return false;
        for (auto depth = xcb_screen_allowed_depths_iterator(screen); depth.rem; xcb_depth_next(&depth)) {
            if (depth.data->depth != screen->root_depth) continue;
            for (auto visual = xcb_depth_visuals_iterator(depth.data); visual.rem; xcb_visualtype_next(&visual)) {
                if (visual.data->visual_id == screen->root_visual) {
                    return visual.data->red_mask == 0xff0000 && visual.data->green_mask == 0xff00 &&
                        visual.data->blue_mask == 0xff;
                }
            }
        }
    }
    return false;
}

int ReadActiveVt() {
    const int fd = open("/sys/class/tty/tty0/active", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[32];
    const auto size = read(fd, buf, sizeof(buf));
    close(fd);
    if (size <= 3 || std::memcmp(buf, "tty", 3) != 0) return 0;
    int vt{};
    return std::from_chars(buf + 3, buf + size, vt).ec == std::errc{} ? vt : 0;
}

std::vector<std::string> ReadCmdlineArgs(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    std::size_t start = 0;
    while (start < raw.size()) {
        const auto nul = raw.find('\0', start);
        const auto len = (nul == std::string::npos) ? (raw.size() - start) : (nul - start);
        if (len > 0) args.emplace_back(raw.data() + start, len);
        if (nul == std::string::npos) break;
        start = nul + 1;
    }
    return args;
}

unsigned ReadProcessUid(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("Uid:")) {
            std::istringstream iss(line.substr(4));
            unsigned real_uid = 0;
            if (iss >> real_uid) return real_uid;
            break;
        }
    }
    return 0;
}

std::string FindXorgDisplayForPid(int pid) {
    std::set<std::string> inodes;
    std::error_code ec;
    const std::filesystem::path fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    for (const auto& entry : std::filesystem::directory_iterator(fd_dir, ec)) {
        const auto target = std::filesystem::read_symlink(entry.path(), ec).string();
        if (ec) {
            ec.clear();
            continue;
        }
        if (target.starts_with("socket:[") && target.ends_with("]")) {
            inodes.insert(target.substr(8, target.size() - 9));
        }
    }

    if (!inodes.empty()) {
        std::ifstream unix_net("/proc/net/unix");
        std::string line;
        while (std::getline(unix_net, line)) {
            std::istringstream iss(line);
            std::string num, ref, proto, flags, type, st, inode, path;
            if (!(iss >> num >> ref >> proto >> flags >> type >> st >> inode >> path)) continue;
            if (!inodes.contains(inode)) continue;
            if (!path.empty() && path.front() == '@') path.erase(path.begin());
            constexpr std::string_view prefix = "/tmp/.X11-unix/X";
            if (path.starts_with(prefix)) {
                return ":" + path.substr(prefix.size());
            }
        }
    }
    return {};
}

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool IsSystemXorg(const std::filesystem::path& process) {
    std::error_code ec;
    auto path = std::filesystem::read_symlink(process / "exe", ec).string();
    // Package upgrades can replace Xorg while the active desktop still runs it.
    constexpr std::string_view deleted = " (deleted)";
    if (path.ends_with(deleted)) path.resize(path.size() - deleted.size());
    struct stat attributes{};
    return !ec && (path.starts_with("/usr/bin/") || path.starts_with("/usr/lib/")) &&
        (BaseName(path) == "Xorg" || BaseName(path) == "X") &&
        stat((process / "exe").c_str(), &attributes) == 0 && attributes.st_uid == 0 &&
        (attributes.st_mode & 0022) == 0;
}

// libxcb copies the authentication record during connection setup.
xcb_connection_t* ConnectXorg(const ActiveXorgTarget& target, int& screen) {
    if (target.authority.empty()) return xcb_connect(target.display.c_str(), &screen);
    const int fd = open(target.authority.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return nullptr;
    struct stat attributes{};
    if (fstat(fd, &attributes) != 0 || !S_ISREG(attributes.st_mode) || attributes.st_size > 65536 ||
        (attributes.st_uid != 0 && attributes.st_uid != target.uid)) {
        close(fd);
        return nullptr;
    }
    const auto close_file = [](FILE* handle) { fclose(handle); };
    std::unique_ptr<FILE, decltype(close_file)> file(fdopen(fd, "rb"), close_file);
    if (!file) {
        close(fd);
        return nullptr;
    }
    const std::string number = target.display.substr(1);
    const auto connect = [&](const Xauth& auth) {
        xcb_auth_info_t info{auth.name_length, auth.name, auth.data_length, auth.data};
        return xcb_connect_to_display_with_auth_info(target.display.c_str(), &info, &screen);
    };
    std::unique_ptr<Xauth, decltype(&XauDisposeAuth)> fallback(nullptr, XauDisposeAuth);
    while (auto* record = XauReadAuth(file.get())) {
        std::unique_ptr<Xauth, decltype(&XauDisposeAuth)> auth(record, XauDisposeAuth);
        if (std::string_view(record->name, record->name_length) != "MIT-MAGIC-COOKIE-1" ||
            record->data_length != 16) continue;
        if (record->number_length && std::string_view(record->number, record->number_length) == number) {
            return connect(*record);
        }
        // GDM can leave the display number empty for a local authorization record.
        if (!fallback && !record->number_length && (record->family == FamilyLocal || record->family == FamilyWild)) {
            fallback = std::move(auth);
        }
    }
    return fallback ? connect(*fallback) : nullptr;
}

bool IsDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

void WriteLe16(std::string& out, std::size_t offset, unsigned value) noexcept {
    out[offset] = static_cast<char>(value & 0xffu);
    out[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
}

} // namespace

std::optional<ActiveXorgTarget> DiscoverActiveXorg() {
    const int active_vt = ReadActiveVt();
    if (active_vt == 0) return std::nullopt;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        const auto name = entry.path().filename().string();
        if (!IsDigits(name) || !IsSystemXorg(entry.path())) continue;
        const int pid = std::stoi(name);
        const auto args = ReadCmdlineArgs(entry.path() / "cmdline");
        if (args.empty()) continue;

        ActiveXorgTarget target;
        target.pid = pid;
        target.uid = ReadProcessUid(pid);
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (target.display.empty() && arg.starts_with(':') && IsDigits(arg.substr(1))) {
                target.display = arg;
            } else if (arg == "-auth" && i + 1 < args.size()) {
                target.authority = args[++i];
            } else if (arg.starts_with("vt") && IsDigits(arg.substr(2))) {
                std::from_chars(arg.data() + 2, arg.data() + arg.size(), target.vt);
            }
        }
        if (target.vt != active_vt) continue;
        if (target.display.empty()) {
            target.display = FindXorgDisplayForPid(pid);
        }
        if (target.display.starts_with(':') && IsDigits(target.display.substr(1))) return target;
    }
    return std::nullopt;
}

std::string EncodeXFixesCursor(unsigned width, unsigned height, unsigned hot_x, unsigned hot_y,
    const std::uint32_t* pixels) {
    if (!pixels || !width || !height || width > kCursorMaxDim || height > kCursorMaxDim) return {};
    if (hot_x >= width) hot_x = width - 1;
    if (hot_y >= height) hot_y = height - 1;

    const std::size_t mask_stride = ((width + 31u) / 32u) * 4u;
    const std::size_t mask_bytes = mask_stride * height;
    const std::size_t color_bytes = static_cast<std::size_t>(width) * height * 4u;

    std::string out(kCursorHeaderBytes + mask_bytes + color_bytes, '\0');
    out[0] = kCursorMarker;
    WriteLe16(out, 1, width);
    WriteLe16(out, 3, height);
    WriteLe16(out, 5, hot_x);
    WriteLe16(out, 7, hot_y);

    auto* mask = reinterpret_cast<std::uint8_t*>(out.data() + kCursorHeaderBytes);
    auto* color = reinterpret_cast<std::uint8_t*>(out.data() + kCursorHeaderBytes + mask_bytes);
    for (unsigned y = 0; y < height; ++y) {
        auto* mask_row = mask + y * mask_stride;
        auto* color_row = color + static_cast<std::size_t>(y) * width * 4u;
        for (unsigned x = 0; x < width; ++x) {
            const auto argb = pixels[static_cast<std::size_t>(y) * width + x];
            const std::uint8_t a = static_cast<std::uint8_t>((argb >> 24) & 0xffu);
            std::uint8_t r = static_cast<std::uint8_t>((argb >> 16) & 0xffu);
            std::uint8_t g = static_cast<std::uint8_t>((argb >> 8) & 0xffu);
            std::uint8_t b = static_cast<std::uint8_t>(argb & 0xffu);
            // XFixes returns premultiplied ARGB; un-premultiply for standard BGRA cursor pixels.
            if (a > 0 && a < 255) {
                r = static_cast<std::uint8_t>(std::min<unsigned>(255u, (static_cast<unsigned>(r) * 255u) / a));
                g = static_cast<std::uint8_t>(std::min<unsigned>(255u, (static_cast<unsigned>(g) * 255u) / a));
                b = static_cast<std::uint8_t>(std::min<unsigned>(255u, (static_cast<unsigned>(b) * 255u) / a));
            }
            color_row[x * 4 + 0] = b;
            color_row[x * 4 + 1] = g;
            color_row[x * 4 + 2] = r;
            color_row[x * 4 + 3] = a;
            if (a == 0) {
                mask_row[x >> 3] |= static_cast<std::uint8_t>(0x80u >> (x & 7u));
            }
        }
    }
    return out;
}

X11ShmCapturer::~X11ShmCapturer() {
    Disconnect();
}

void X11ShmCapturer::DetachShm() noexcept {
    if (display_ && shmseg_) xcb_shm_detach(display_, shmseg_);
    if (shmaddr_) shmdt(shmaddr_);
    if (shmid_ >= 0) shmctl(shmid_, IPC_RMID, nullptr);
    shmseg_ = 0;
    shmaddr_ = nullptr;
    shmid_ = -1;
    width_ = height_ = stride_ = 0;
}

void X11ShmCapturer::Disconnect() noexcept {
    frame_vt_.store(0, std::memory_order_release);
    DetachShm();
    if (display_) xcb_disconnect(display_);
    display_ = nullptr;
    root_ = 0;
    has_xfixes_ = false;
    current_target_ = {};
    last_cursor_serial_ = 0;
    cursor_sent_ = false;
}

bool X11ShmCapturer::AttachShm(int width, int height) {
    DetachShm();
    if (width <= 0 || height <= 0) return false;
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    shmid_ = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (shmid_ < 0) return false;
    auto* addr = shmat(shmid_, nullptr, 0);
    if (addr == reinterpret_cast<void*>(-1)) {
        DetachShm();
        return false;
    }
    shmaddr_ = addr;
    if (current_target_.uid != 0 && geteuid() == 0) {
        shmid_ds attributes{};
        if (shmctl(shmid_, IPC_STAT, &attributes) != 0) {
            DetachShm();
            return false;
        }
        attributes.shm_perm.uid = current_target_.uid;
        if (shmctl(shmid_, IPC_SET, &attributes) != 0) {
            DetachShm();
            return false;
        }
    }

    const auto segment = xcb_generate_id(display_);
    const auto request = xcb_shm_attach_checked(display_, segment, static_cast<std::uint32_t>(shmid_), 0);
    XcbReply<xcb_generic_error_t> error(xcb_request_check(display_, request), std::free);
    if (error || xcb_connection_has_error(display_)) {
        DetachShm();
        return false;
    }
    if (shmctl(shmid_, IPC_RMID, nullptr) == 0) shmid_ = -1;
    shmseg_ = segment;
    width_ = width;
    height_ = height;
    stride_ = width * 4;
    return true;
}

bool X11ShmCapturer::EnsureConnected() {
    const int active_vt = ReadActiveVt();
    if (display_ && (current_target_.vt != active_vt || xcb_connection_has_error(display_) ||
                     kill(current_target_.pid, 0) != 0)) {
        Disconnect();
        last_discovery_ = {};
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_discovery_ < std::chrono::milliseconds(500)) return display_ != nullptr;
    last_discovery_ = now;
    if (active_vt == 0) return false;

    const auto next = DiscoverActiveXorg();
    if (!next || next->vt != active_vt) {
        Disconnect();
        return false;
    }
    if (display_ && *next == current_target_) return true;
    Disconnect();

    int screen_index = 0;
    display_ = ConnectXorg(*next, screen_index);
    if (!display_ || xcb_connection_has_error(display_)) {
        Disconnect();
        return false;
    }
    current_target_ = *next;
    const auto* setup = xcb_get_setup(display_);
    auto screen = xcb_setup_roots_iterator(setup);
    while (screen.rem && screen_index-- > 0) xcb_screen_next(&screen);
    if (!screen.rem || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST ||
        !SupportsBgra(setup, screen.data)) {
        Disconnect();
        return false;
    }

    root_ = screen.data->root;
    if (!xcb_get_extension_data(display_, &xcb_shm_id)->present) {
        Disconnect();
        return false;
    }
    const auto shm_version = xcb_shm_query_version(display_);
    XcbReply<xcb_shm_query_version_reply_t> shm(
        xcb_shm_query_version_reply(display_, shm_version, nullptr), std::free);
    if (xcb_get_extension_data(display_, &xcb_xfixes_id)->present) {
        const auto request = xcb_xfixes_query_version(display_, 4, 0);
        XcbReply<xcb_xfixes_query_version_reply_t> fixes(
            xcb_xfixes_query_version_reply(display_, request, nullptr), std::free);
        has_xfixes_ = fixes && fixes->major_version >= 2;
    }
    if (!shm || !AttachShm(screen.data->width_in_pixels, screen.data->height_in_pixels)) {
        Disconnect();
        return false;
    }
    last_geom_check_ = now;
    return true;
}

std::optional<CapturedFrame> X11ShmCapturer::GrabFrame() {
    if (!EnsureConnected()) return std::nullopt;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_geom_check_ >= std::chrono::milliseconds(500)) {
        last_geom_check_ = now;
        const auto request = xcb_get_geometry(display_, root_);
        XcbReply<xcb_get_geometry_reply_t> geometry(
            xcb_get_geometry_reply(display_, request, nullptr), std::free);
        if (!geometry || ((geometry->width != width_ || geometry->height != height_) &&
                         !AttachShm(geometry->width, geometry->height))) {
            Disconnect();
            return std::nullopt;
        }
    }

    const auto request = xcb_shm_get_image(display_, root_, 0, 0, width_, height_,
        ~std::uint32_t{}, XCB_IMAGE_FORMAT_Z_PIXMAP, shmseg_, 0);
    XcbReply<xcb_shm_get_image_reply_t> frame(
        xcb_shm_get_image_reply(display_, request, nullptr), std::free);
    if (!frame || frame->size != static_cast<std::size_t>(stride_) * height_) {
        Disconnect();
        return std::nullopt;
    }
    if (ReadActiveVt() != current_target_.vt) {
        frame_vt_.store(0, std::memory_order_release);
        return std::nullopt;
    }
    return CapturedFrame{width_, height_, stride_, static_cast<const std::uint8_t*>(shmaddr_)};
}

void X11ShmCapturer::MarkFrameSent() noexcept {
    frame_vt_.store(current_target_.vt, std::memory_order_release);
}

unsigned X11ShmCapturer::ActiveFrameVt() const {
    const auto vt = frame_vt_.load(std::memory_order_acquire);
    return vt != 0 && vt == ReadActiveVt() ? static_cast<unsigned>(vt) : 0;
}

std::optional<CursorUpdate> X11ShmCapturer::PollCursor(bool force) {
    if (!display_ || !has_xfixes_ || !InputAvailable()) return std::nullopt;
    const auto request = xcb_xfixes_get_cursor_image(display_);
    XcbReply<xcb_xfixes_get_cursor_image_reply_t> cursor(
        xcb_xfixes_get_cursor_image_reply(display_, request, nullptr), std::free);
    if (!cursor) return std::nullopt;

    if (!force && cursor_sent_ && cursor->cursor_serial == last_cursor_serial_) return std::nullopt;
    if (xcb_xfixes_get_cursor_image_cursor_image_length(cursor.get()) <
        static_cast<std::int64_t>(cursor->width) * cursor->height) return std::nullopt;
    auto packet = EncodeXFixesCursor(cursor->width, cursor->height, cursor->xhot, cursor->yhot,
        xcb_xfixes_get_cursor_image_cursor_image(cursor.get()));
    if (packet.empty()) return std::nullopt;
    last_cursor_serial_ = cursor->cursor_serial;
    cursor_sent_ = true;
    return CursorUpdate{std::move(packet)};
}

std::string X11ShmCapturer::Describe() const {
    if (!display_) return "X11Shm(disconnected)";
    return "X11Shm(display=" + current_target_.display + ", vt=" + std::to_string(current_target_.vt) +
        ", " + std::to_string(width_) + "x" + std::to_string(height_) + ")";
}

} // namespace p2premote::linux_host
