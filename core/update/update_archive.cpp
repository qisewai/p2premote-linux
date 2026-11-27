#include "update_archive.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace p2premote::update {
namespace {

unsigned long CurrentPid() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::vector<std::string_view> Split(std::string_view sv, char delim) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= sv.size()) {
        const std::size_t pos = sv.find(delim, start);
        if (pos == std::string_view::npos) {
            out.push_back(sv.substr(start));
            break;
        }
        out.push_back(sv.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool ParseUnsigned(std::string_view sv, std::uint64_t& value) noexcept {
    if (sv.empty()) return false;
    for (char c : sv) {
        if (c < '0' || c > '9') return false;
    }
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    return res.ec == std::errc{} && res.ptr == sv.data() + sv.size();
}

int CompareFields(std::string_view a, std::string_view b) noexcept {
    const auto af = Split(a, '.');
    const auto bf = Split(b, '.');
    const std::size_t count = std::max(af.size(), bf.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view x = i < af.size() ? af[i] : std::string_view{};
        const std::string_view y = i < bf.size() ? bf[i] : std::string_view{};
        if (x == y) continue;
        if (x.empty()) return -1;
        if (y.empty()) return 1;
        std::uint64_t xn{}, yn{};
        const bool xnum = ParseUnsigned(x, xn);
        const bool ynum = ParseUnsigned(y, yn);
        if (xnum && ynum) {
            if (xn < yn) return -1;
            if (xn > yn) return 1;
            continue;
        }
        if (xnum) return -1; // numeric identifiers sort before non-numeric
        if (ynum) return 1;
        return x < y ? -1 : 1;
    }
    return 0;
}

std::string Trim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return std::string(sv);
}

// ============================================================================
// Minimal strict JSON reader/writer for UpdateManifest
// ============================================================================

struct JsonValue {
    enum class Kind { Null, Bool, Int, String, Array, Object };
    Kind kind{Kind::Null};
    bool bool_val{};
    std::int64_t int_val{};
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue* Get(std::string_view key) const noexcept {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [k, v] : obj_val) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : src_(src) {}

    bool Parse(JsonValue& out, std::string* error) {
        SkipWs();
        if (!ParseValue(out, 0, error)) return false;
        SkipWs();
        if (pos_ != src_.size()) {
            if (error) *error = "trailing characters in JSON";
            return false;
        }
        return true;
    }

private:
    void SkipWs() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }

    bool ParseValue(JsonValue& out, int depth, std::string* error) {
        if (depth > 32) {
            if (error) *error = "JSON nesting too deep";
            return false;
        }
        SkipWs();
        if (pos_ >= src_.size()) {
            if (error) *error = "unexpected end of JSON";
            return false;
        }
        const char c = src_[pos_];
        if (c == '{') return ParseObject(out, depth + 1, error);
        if (c == '[') return ParseArray(out, depth + 1, error);
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return ParseString(out.str_val, error);
        }
        if (c == 't' && src_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = true;
            return true;
        }
        if (c == 'f' && src_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = false;
            return true;
        }
        if (c == 'n' && src_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out, error);
        if (error) *error = "unexpected token in JSON";
        return false;
    }

    bool ParseObject(JsonValue& out, int depth, std::string* error) {
        ++pos_; // '{'
        out.kind = JsonValue::Kind::Object;
        out.obj_val.clear();
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                if (error) *error = "expected string key in JSON object";
                return false;
            }
            std::string key;
            if (!ParseString(key, error)) return false;
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                if (error) *error = "expected ':' in JSON object";
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!ParseValue(val, depth, error)) return false;
            out.obj_val.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (pos_ >= src_.size()) {
                if (error) *error = "unterminated JSON object";
                return false;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (src_[pos_] != ',') {
                if (error) *error = "expected ',' or '}' in JSON object";
                return false;
            }
            ++pos_;
        }
    }

    bool ParseArray(JsonValue& out, int depth, std::string* error) {
        ++pos_; // '['
        out.kind = JsonValue::Kind::Array;
        out.arr_val.clear();
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!ParseValue(elem, depth, error)) return false;
            out.arr_val.push_back(std::move(elem));
            SkipWs();
            if (pos_ >= src_.size()) {
                if (error) *error = "unterminated JSON array";
                return false;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (src_[pos_] != ',') {
                if (error) *error = "expected ',' or ']' in JSON array";
                return false;
            }
            ++pos_;
        }
    }

    static void AppendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7f) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool ParseHex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > src_.size()) return false;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool ParseString(std::string& out, std::string* error) {
        ++pos_; // '"'
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                if (error) *error = "unescaped control character in JSON string";
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= src_.size()) {
                if (error) *error = "unterminated escape in JSON string";
                return false;
            }
            const char e = src_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp{};
                    if (!ParseHex4(cp)) {
                        if (error) *error = "invalid unicode escape in JSON string";
                        return false;
                    }
                    if (cp >= 0xd800 && cp <= 0xdbff && pos_ + 2 <= src_.size() &&
                        src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        std::uint32_t low{};
                        if (!ParseHex4(low) || low < 0xdc00 || low > 0xdfff) {
                            if (error) *error = "invalid surrogate pair in JSON string";
                            return false;
                        }
                        cp = 0x10000 + (((cp - 0xd800) << 10) | (low - 0xdc00));
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    if (error) *error = "unknown escape sequence in JSON string";
                    return false;
            }
        }
        if (error) *error = "unterminated JSON string";
        return false;
    }

    bool ParseNumber(JsonValue& out, std::string* error) {
        const std::size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        const std::size_t int_end = pos_;
        if (pos_ < src_.size() && (src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
            while (pos_ < src_.size()) {
                const char c = src_[pos_];
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') ++pos_;
                else break;
            }
        }
        std::int64_t val{};
        const auto res = std::from_chars(src_.data() + start, src_.data() + int_end, val);
        if (res.ec != std::errc{} || res.ptr != src_.data() + int_end) {
            if (error) *error = "invalid number in JSON";
            return false;
        }
        out.kind = JsonValue::Kind::Int;
        out.int_val = val;
        return true;
    }

    std::string_view src_;
    std::size_t pos_{0};
};

void AppendJsonString(std::string& out, std::string_view sv) {
    out.push_back('"');
    for (unsigned char c : sv) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

// ============================================================================
// POSIX tar + gzip helpers
// ============================================================================

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(TarHeader) == 512);

bool IsZeroBlock(const std::uint8_t block[512]) noexcept {
    for (int i = 0; i < 512; ++i) {
        if (block[i] != 0) return false;
    }
    return true;
}

bool ParseTarOctal(const char* field, std::size_t len, std::uint64_t& out) noexcept {
    // Handle GNU base-256 extension (high bit set)
    if (len > 0 && (static_cast<unsigned char>(field[0]) & 0x80) != 0) {
        std::uint64_t v = static_cast<unsigned char>(field[0]) & 0x7f;
        for (std::size_t i = 1; i < len; ++i) {
            v = (v << 8) | static_cast<unsigned char>(field[i]);
        }
        out = v;
        return true;
    }
    std::size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;
    std::uint64_t v = 0;
    bool any = false;
    while (i < len && field[i] >= '0' && field[i] <= '7') {
        v = (v << 3) | static_cast<std::uint64_t>(field[i] - '0');
        any = true;
        ++i;
    }
    while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;
    if (i != len) return false;
    out = any ? v : 0;
    return true;
}

std::string ReadTarField(const char* field, std::size_t len) {
    std::size_t n = 0;
    while (n < len && field[n] != '\0') ++n;
    return std::string(field, n);
}

void WriteTarOctal(char* dst, std::size_t len, std::uint64_t value) noexcept {
    std::memset(dst, '0', len - 1);
    dst[len - 1] = '\0';
    std::size_t pos = len - 1;
    while (pos > 0 && value > 0) {
        --pos;
        dst[pos] = static_cast<char>('0' + (value & 7U));
        value >>= 3;
    }
}

void FinalizeTarChecksum(TarHeader& hdr) noexcept {
    std::memset(hdr.chksum, ' ', sizeof(hdr.chksum));
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&hdr);
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < sizeof(hdr); ++i) sum += raw[i];
    WriteTarOctal(hdr.chksum, 7, sum);
    hdr.chksum[7] = ' ';
}

bool VerifyTarChecksum(const TarHeader& hdr) noexcept {
    std::uint64_t expected{};
    if (!ParseTarOctal(hdr.chksum, sizeof(hdr.chksum), expected)) return false;
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&hdr);
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < sizeof(hdr); ++i) {
        if (i >= offsetof(TarHeader, chksum) && i < offsetof(TarHeader, chksum) + sizeof(hdr.chksum)) {
            sum += static_cast<unsigned char>(' ');
        } else {
            sum += raw[i];
        }
    }
    return sum == expected;
}

// Validates and normalizes an archive-relative POSIX path (rejects absolute, "..", backslashes, NUL).
bool CleanSafeRelativePath(std::string_view raw, std::string& cleaned) {
    cleaned.clear();
    if (raw.empty()) return false;
    if (raw.find('\0') != std::string_view::npos || raw.find('\\') != std::string_view::npos) return false;
    while (raw.starts_with("./")) raw.remove_prefix(2);
    while (!raw.empty() && raw.back() == '/') raw.remove_suffix(1);
    if (raw.empty() || raw == "." || raw.front() == '/') return false;
    if (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':') return false;

    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= raw.size()) {
        const std::size_t slash = raw.find('/', start);
        const std::string_view part = (slash == std::string_view::npos)
            ? raw.substr(start) : raw.substr(start, slash - start);
        if (part.empty() || part == ".") {
            if (slash == std::string_view::npos) break;
            start = slash + 1;
            continue;
        }
        if (part == "..") return false;
        parts.push_back(part);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) return false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) cleaned.push_back('/');
        cleaned.append(parts[i]);
    }
    return true;
}

// Validates that a symlink target relative to entry_path stays within archive root.
bool IsSafeSymlinkTarget(std::string_view entry_path, std::string_view link_target) {
    if (link_target.empty() || link_target.front() == '/' ||
        link_target.find('\0') != std::string_view::npos ||
        link_target.find('\\') != std::string_view::npos) {
        return false;
    }
    if (link_target.size() >= 2 && std::isalpha(static_cast<unsigned char>(link_target[0])) && link_target[1] == ':') {
        return false;
    }
    std::vector<std::string_view> stack;
    const auto last_slash = entry_path.rfind('/');
    if (last_slash != std::string_view::npos) {
        for (const auto p : Split(entry_path.substr(0, last_slash), '/')) {
            if (!p.empty() && p != ".") stack.push_back(p);
        }
    }
    for (const auto p : Split(link_target, '/')) {
        if (p.empty() || p == ".") continue;
        if (p == "..") {
            if (stack.empty()) return false; // escapes root
            stack.pop_back();
        } else {
            stack.push_back(p);
        }
    }
    return true;
}

class GzipStreamWriter {
public:
    bool Open(const std::filesystem::path& path, std::string* error) {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_) {
            if (error) *error = "could not open archive file for writing";
            return false;
        }
        zs_ = {};
        if (deflateInit2(&zs_, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            if (error) *error = "zlib deflateInit2 failed";
            return false;
        }
        initialized_ = true;
        return true;
    }

    ~GzipStreamWriter() {
        if (initialized_) deflateEnd(&zs_);
    }

    bool Write(const void* data, std::size_t len, std::string* error) {
        zs_.next_in = reinterpret_cast<Bytef*>(const_cast<void*>(data));
        zs_.avail_in = static_cast<uInt>(len);
        std::array<std::uint8_t, 32768> out{};
        while (zs_.avail_in > 0) {
            zs_.next_out = out.data();
            zs_.avail_out = static_cast<uInt>(out.size());
            const int rc = deflate(&zs_, Z_NO_FLUSH);
            if (rc != Z_OK) {
                if (error) *error = "zlib deflate failed";
                return false;
            }
            const std::size_t produced = out.size() - zs_.avail_out;
            if (produced > 0 && !Emit(out.data(), produced, error)) return false;
        }
        return true;
    }

    bool Close(std::int64_t& out_size, Sha256Digest& out_sha, std::string* error) {
        std::array<std::uint8_t, 32768> out{};
        zs_.next_in = nullptr;
        zs_.avail_in = 0;
        while (true) {
            zs_.next_out = out.data();
            zs_.avail_out = static_cast<uInt>(out.size());
            const int rc = deflate(&zs_, Z_FINISH);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                if (error) *error = "zlib deflate finish failed";
                return false;
            }
            const std::size_t produced = out.size() - zs_.avail_out;
            if (produced > 0 && !Emit(out.data(), produced, error)) return false;
            if (rc == Z_STREAM_END) break;
        }
        deflateEnd(&zs_);
        initialized_ = false;
        file_.flush();
        if (!file_) {
            if (error) *error = "failed writing archive file";
            return false;
        }
        file_.close();
        out_size = written_;
        out_sha = sha_.Final();
        return true;
    }

private:
    bool Emit(const std::uint8_t* bytes, std::size_t len, std::string* error) {
        file_.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
        if (!file_) {
            if (error) *error = "disk write failed while writing archive";
            return false;
        }
        sha_.Update(bytes, len);
        written_ += static_cast<std::int64_t>(len);
        return true;
    }

    std::ofstream file_;
    z_stream zs_{};
    bool initialized_{false};
    Sha256 sha_;
    std::int64_t written_{0};
};

class GzipStreamReader {
public:
    bool Open(const std::filesystem::path& path, std::string* error) {
        file_.open(path, std::ios::binary);
        if (!file_) {
            if (error) *error = "could not open archive file";
            return false;
        }
        zs_ = {};
        if (inflateInit2(&zs_, 16 + MAX_WBITS) != Z_OK) {
            if (error) *error = "zlib inflateInit2 failed";
            return false;
        }
        initialized_ = true;
        return true;
    }

    ~GzipStreamReader() {
        if (initialized_) inflateEnd(&zs_);
    }

    // Reads exactly len uncompressed bytes, returns false on error or premature EOF.
    bool ReadExact(void* dst, std::size_t len, bool& eof, std::string* error) {
        eof = false;
        auto* out = static_cast<std::uint8_t*>(dst);
        std::size_t got = 0;
        while (got < len) {
            if (zs_.avail_in == 0 && !stream_end_) {
                file_.read(reinterpret_cast<char*>(in_buf_.data()), static_cast<std::streamsize>(in_buf_.size()));
                const auto read_n = file_.gcount();
                if (read_n <= 0) {
                    if (got == 0 && stream_end_) {
                        eof = true;
                        return false;
                    }
                    if (error) *error = "unexpected end of gzip archive";
                    return false;
                }
                zs_.next_in = in_buf_.data();
                zs_.avail_in = static_cast<uInt>(read_n);
            }
            if (stream_end_) {
                if (got == 0) {
                    eof = true;
                    return false;
                }
                if (error) *error = "truncated tar stream inside gzip archive";
                return false;
            }
            zs_.next_out = out + got;
            zs_.avail_out = static_cast<uInt>(len - got);
            const int rc = inflate(&zs_, Z_NO_FLUSH);
            got = len - zs_.avail_out;
            if (rc == Z_STREAM_END) {
                stream_end_ = true;
            } else if (rc != Z_OK) {
                if (error) *error = "corrupt gzip data in update archive";
                return false;
            }
        }
        return true;
    }

private:
    std::ifstream file_;
    z_stream zs_{};
    bool initialized_{false};
    bool stream_end_{false};
    std::array<std::uint8_t, 32768> in_buf_{};
};

std::string MakePaxRecord(std::string_view key, std::string_view val) {
    // Format: "<len> <key>=<val>\n"
    const std::size_t body_len = 1 + key.size() + 1 + val.size() + 1;
    std::size_t total = body_len + 1;
    while (true) {
        const std::string len_str = std::to_string(total);
        if (len_str.size() + body_len == total) {
            return len_str + " " + std::string(key) + "=" + std::string(val) + "\n";
        }
        total = len_str.size() + body_len;
    }
}

void ParsePaxRecords(std::string_view data, std::string& path_override, std::string& link_override,
                     std::optional<std::uint64_t>& size_override) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t sp = data.find(' ', pos);
        if (sp == std::string_view::npos) break;
        std::uint64_t rec_len{};
        if (!ParseUnsigned(data.substr(pos, sp - pos), rec_len) || rec_len == 0 || pos + rec_len > data.size()) {
            break;
        }
        std::string_view kv = data.substr(sp + 1, static_cast<std::size_t>(rec_len) - (sp - pos) - 1);
        if (!kv.empty() && kv.back() == '\n') kv.remove_suffix(1);
        const std::size_t eq = kv.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = kv.substr(0, eq);
            const std::string_view val = kv.substr(eq + 1);
            if (key == "path") path_override = std::string(val);
            else if (key == "linkpath") link_override = std::string(val);
            else if (key == "size") {
                std::uint64_t parsed{};
                if (ParseUnsigned(val, parsed)) size_override = parsed;
            }
        }
        pos += static_cast<std::size_t>(rec_len);
    }
}

bool WriteTarEntry(GzipStreamWriter& gw, std::string_view rel_path, char typeflag,
                   std::uint32_t mode, std::uint64_t size, std::string_view linkname,
                   const std::filesystem::path* file_path, std::string* error) {
    std::string pax;
    if (rel_path.size() > 99) pax += MakePaxRecord("path", rel_path);
    if (linkname.size() > 99) pax += MakePaxRecord("linkpath", linkname);
    if (size > 077777777777ULL) pax += MakePaxRecord("size", std::to_string(size));
    if (!pax.empty()) {
        TarHeader ph{};
        std::memcpy(ph.name, "paxheader", 9);
        WriteTarOctal(ph.mode, sizeof(ph.mode), 0644);
        WriteTarOctal(ph.uid, sizeof(ph.uid), 0);
        WriteTarOctal(ph.gid, sizeof(ph.gid), 0);
        WriteTarOctal(ph.size, sizeof(ph.size), pax.size());
        WriteTarOctal(ph.mtime, sizeof(ph.mtime), 0);
        ph.typeflag = 'x';
        std::memcpy(ph.magic, "ustar", 6);
        ph.version[0] = '0';
        ph.version[1] = '0';
        FinalizeTarChecksum(ph);
        if (!gw.Write(&ph, sizeof(ph), error)) return false;
        if (!gw.Write(pax.data(), pax.size(), error)) return false;
        const std::size_t rem = pax.size() % 512;
        if (rem != 0) {
            const std::array<std::uint8_t, 512> zeros{};
            if (!gw.Write(zeros.data(), 512 - rem, error)) return false;
        }
    }

    TarHeader hdr{};
    std::memcpy(hdr.name, rel_path.data(), std::min<std::size_t>(rel_path.size(), sizeof(hdr.name) - 1));
    WriteTarOctal(hdr.mode, sizeof(hdr.mode), mode & 0777U);
    WriteTarOctal(hdr.uid, sizeof(hdr.uid), 0);
    WriteTarOctal(hdr.gid, sizeof(hdr.gid), 0);
    WriteTarOctal(hdr.size, sizeof(hdr.size), typeflag == '0' ? size : 0);
    WriteTarOctal(hdr.mtime, sizeof(hdr.mtime), 0);
    hdr.typeflag = typeflag;
    if (!linkname.empty()) {
        std::memcpy(hdr.linkname, linkname.data(), std::min<std::size_t>(linkname.size(), sizeof(hdr.linkname) - 1));
    }
    std::memcpy(hdr.magic, "ustar", 6);
    hdr.version[0] = '0';
    hdr.version[1] = '0';
    FinalizeTarChecksum(hdr);
    if (!gw.Write(&hdr, sizeof(hdr), error)) return false;

    if (typeflag == '0' && file_path && size > 0) {
        std::ifstream in(*file_path, std::ios::binary);
        if (!in) {
            if (error) *error = "could not open file for archiving: " + file_path->string();
            return false;
        }
        std::array<std::uint8_t, 32768> buf{};
        std::uint64_t remaining = size;
        while (remaining > 0) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buf.size()));
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got != want) {
                if (error) *error = "short read while archiving: " + file_path->string();
                return false;
            }
            if (!gw.Write(buf.data(), got, error)) return false;
            remaining -= got;
        }
        const std::size_t rem = static_cast<std::size_t>(size % 512);
        if (rem != 0) {
            const std::array<std::uint8_t, 512> zeros{};
            if (!gw.Write(zeros.data(), 512 - rem, error)) return false;
        }
    }
    return true;
}

} // namespace

int CompareVersions(std::string_view a, std::string_view b) noexcept {
    if (a.starts_with('v') || a.starts_with('V')) a.remove_prefix(1);
    if (b.starts_with('v') || b.starts_with('V')) b.remove_prefix(1);
    if (const auto plus = a.find('+'); plus != std::string_view::npos) a = a.substr(0, plus);
    if (const auto plus = b.find('+'); plus != std::string_view::npos) b = b.substr(0, plus);

    std::string_view ar = a, apre;
    if (const auto dash = a.find('-'); dash != std::string_view::npos) {
        ar = a.substr(0, dash);
        apre = a.substr(dash + 1);
    }
    std::string_view br = b, bpre;
    if (const auto dash = b.find('-'); dash != std::string_view::npos) {
        br = b.substr(0, dash);
        bpre = b.substr(dash + 1);
    }
    if (const int c = CompareFields(ar, br); c != 0) return c;
    if (apre == bpre) return 0;
    if (apre.empty()) return 1;
    if (bpre.empty()) return -1;
    return CompareFields(apre, bpre);
}

std::string ExtractReleaseNotes(std::string_view changelog, std::string_view version) {
    if (version.starts_with('v') || version.starts_with('V')) version.remove_prefix(1);
    std::vector<std::string> lines;
    bool in_section = false;
    std::size_t pos = 0;
    while (pos <= changelog.size()) {
        std::size_t nl = changelog.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? changelog.substr(pos) : changelog.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("## ")) {
            if (in_section) break;
            std::string_view rest = line.substr(3);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            std::size_t end = rest.find_first_of(" \t");
            std::string_view head = (end == std::string_view::npos) ? rest : rest.substr(0, end);
            if (head.starts_with('[')) {
                head.remove_prefix(1);
                if (const auto rb = head.find(']'); rb != std::string_view::npos) head = head.substr(0, rb);
            }
            if (head.starts_with('v') || head.starts_with('V')) head.remove_prefix(1);
            in_section = (head == version);
        } else if (in_section) {
            lines.emplace_back(line);
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) joined.push_back('\n');
        joined += lines[i];
    }
    return Trim(joined);
}

std::optional<UpdateManifest> ParseUpdateManifest(std::string_view json, std::string* error) {
    if (json.size() > kMaxManifestBytes) {
        if (error) *error = "manifest exceeds size limit";
        return std::nullopt;
    }
    JsonValue root;
    JsonParser parser(json);
    if (!parser.Parse(root, error) || root.kind != JsonValue::Kind::Object) {
        if (error && error->empty()) *error = "manifest root must be a JSON object";
        return std::nullopt;
    }

    auto read_str = [&](const JsonValue& obj, std::string_view key, std::string& dst, bool required) -> bool {
        const auto* v = obj.Get(key);
        if (!v) return !required;
        if (v->kind != JsonValue::Kind::String) return false;
        dst = v->str_val;
        return !required || !dst.empty();
    };
    auto read_int = [&](const JsonValue& obj, std::string_view key, std::int64_t& dst, bool required) -> bool {
        const auto* v = obj.Get(key);
        if (!v) return !required;
        if (v->kind != JsonValue::Kind::Int) return false;
        dst = v->int_val;
        return true;
    };

    UpdateManifest m;
    if (!read_str(root, "version", m.version, true) ||
        !read_str(root, "url", m.url, true) ||
        !read_str(root, "signature", m.signature, true) ||
        !read_int(root, "size", m.size, true) ||
        !read_str(root, "notes", m.notes, false) ||
        !read_str(root, "date", m.date, false) ||
        !read_str(root, "target", m.target, false) ||
        !read_str(root, "manifest_signature", m.manifest_signature, false)) {
        if (error) *error = "incomplete or malformed update manifest";
        return std::nullopt;
    }
    if (m.size <= 0 || static_cast<std::uint64_t>(m.size) > kMaxArchiveBytes) {
        if (error) *error = "invalid archive size in manifest";
        return std::nullopt;
    }
    if (const auto* deltas = root.Get("deltas"); deltas && deltas->kind == JsonValue::Kind::Array) {
        for (const auto& item : deltas->arr_val) {
            if (item.kind != JsonValue::Kind::Object) continue;
            UpdateDelta d;
            if (read_str(item, "from", d.from, true) &&
                read_str(item, "url", d.url, true) &&
                read_int(item, "size", d.size, true) &&
                read_str(item, "signature", d.signature, true) &&
                d.size > 0 && static_cast<std::uint64_t>(d.size) <= kMaxArchiveBytes) {
                m.deltas.push_back(std::move(d));
            }
        }
    }
    if (const auto* prev = root.Get("previous"); prev && prev->kind == JsonValue::Kind::Array) {
        for (const auto& item : prev->arr_val) {
            if (item.kind != JsonValue::Kind::Object) continue;
            UpdateArchiveRef ref;
            if (read_str(item, "version", ref.version, true) &&
                read_str(item, "url", ref.url, true) &&
                read_int(item, "size", ref.size, true) &&
                read_str(item, "signature", ref.signature, true) &&
                ref.size > 0 && static_cast<std::uint64_t>(ref.size) <= kMaxArchiveBytes) {
                m.previous.push_back(std::move(ref));
            }
        }
    }
    return m;
}

std::string SerializeUpdateManifest(const UpdateManifest& m) {
    std::string out = "{\n";
    out += "  \"version\": "; AppendJsonString(out, m.version); out += ",\n";
    if (!m.notes.empty()) {
        out += "  \"notes\": "; AppendJsonString(out, m.notes); out += ",\n";
    }
    if (!m.date.empty()) {
        out += "  \"date\": "; AppendJsonString(out, m.date); out += ",\n";
    }
    out += "  \"url\": "; AppendJsonString(out, m.url); out += ",\n";
    out += "  \"size\": " + std::to_string(m.size) + ",\n";
    out += "  \"signature\": "; AppendJsonString(out, m.signature);
    if (!m.deltas.empty()) {
        out += ",\n  \"deltas\": [\n";
        for (std::size_t i = 0; i < m.deltas.size(); ++i) {
            const auto& d = m.deltas[i];
            out += "    {\"from\": "; AppendJsonString(out, d.from);
            out += ", \"url\": "; AppendJsonString(out, d.url);
            out += ", \"size\": " + std::to_string(d.size);
            out += ", \"signature\": "; AppendJsonString(out, d.signature);
            out += (i + 1 < m.deltas.size()) ? "},\n" : "}\n";
        }
        out += "  ]";
    }
    if (!m.previous.empty()) {
        out += ",\n  \"previous\": [\n";
        for (std::size_t i = 0; i < m.previous.size(); ++i) {
            const auto& p = m.previous[i];
            out += "    {\"version\": "; AppendJsonString(out, p.version);
            out += ", \"url\": "; AppendJsonString(out, p.url);
            out += ", \"size\": " + std::to_string(p.size);
            out += ", \"signature\": "; AppendJsonString(out, p.signature);
            out += (i + 1 < m.previous.size()) ? "},\n" : "}\n";
        }
        out += "  ]";
    }
    if (!m.target.empty()) {
        out += ",\n  \"target\": "; AppendJsonString(out, m.target);
    }
    if (!m.manifest_signature.empty()) {
        out += ",\n  \"manifest_signature\": "; AppendJsonString(out, m.manifest_signature);
    }
    out += "\n}\n";
    return out;
}

bool IsReleaseVersion(std::string_view version) noexcept {
    for (int component = 0; component < 3; ++component) {
        const auto end = version.find('.');
        const auto part = version.substr(0, end);
        if (part.empty() || part.size() > 9 || (part.size() > 1 && part.front() == '0') ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        if (component == 2) return end == std::string_view::npos;
        if (end == std::string_view::npos) return false;
        version.remove_prefix(end + 1);
    }
    return false;
}

static Sha256Digest ManifestDigest(UpdateManifest manifest) {
    manifest.manifest_signature.clear();
    const auto payload = "P2PRemote update manifest v1\n" + SerializeUpdateManifest(manifest);
    return Sha256::Hash(payload.data(), payload.size());
}

void SignUpdateManifest(UpdateManifest& manifest, const Ed25519PrivateKey& key) {
    manifest.manifest_signature = SignDigestBase64(key, ManifestDigest(manifest));
}

bool VerifyUpdateManifest(const UpdateManifest& manifest, std::string_view target, std::string_view public_key_b64) {
    const auto key = ParsePublicKeyBase64(public_key_b64);
    return key && IsReleaseVersion(manifest.version) && manifest.target == target &&
        VerifyDigestBase64(*key, ManifestDigest(manifest), manifest.manifest_signature);
}

bool IsValidFileListEntry(std::string_view name) noexcept {
    if (name.empty() || name == "." || name == ".." || name == kManagedFileListName) return false;
    return name.find_first_of(std::string_view("/\\\0\r\n", 5)) == std::string_view::npos;
}

std::vector<std::string> ParseFileList(std::string_view content) {
    std::vector<std::string> names;
    std::size_t pos = 0;
    while (pos <= content.size()) {
        const std::size_t nl = content.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? content.substr(pos) : content.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        if (IsValidFileListEntry(line)) names.emplace_back(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::vector<std::string> ReadFileList(const std::filesystem::path& dir) {
    std::ifstream in(dir / std::string(kManagedFileListName), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ParseFileList(ss.str());
}

bool WriteFileList(const std::filesystem::path& dir, std::vector<std::string> entries, std::string* error) {
    std::vector<std::string> clean;
    for (auto& e : entries) {
        if (IsValidFileListEntry(e)) clean.push_back(std::move(e));
    }
    std::sort(clean.begin(), clean.end());
    clean.erase(std::unique(clean.begin(), clean.end()), clean.end());
    std::string body;
    for (const auto& n : clean) {
        body += n;
        body.push_back('\n');
    }
    const auto tmp = dir / (std::string(kManagedFileListName) + "." + std::to_string(CurrentPid()) + ".tmp");
    const auto dst = dir / std::string(kManagedFileListName);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out.write(body.data(), static_cast<std::streamsize>(body.size())))) {
            if (error) *error = "could not write managed file list";
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::filesystem::remove(dst, ec);
        ec.clear();
        std::filesystem::rename(tmp, dst, ec);
    }
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        if (error) *error = "could not finalize managed file list: " + ec.message();
        return false;
    }
    return true;
}

bool WriteTarGzArchive(const std::filesystem::path& source_dir,
                       const std::vector<std::string>& entries,
                       const std::filesystem::path& archive_path,
                       std::int64_t* out_size,
                       Sha256Digest* out_sha256,
                       std::string* error) {
    GzipStreamWriter gw;
    if (!gw.Open(archive_path, error)) return false;

    const auto normalized_source = source_dir.lexically_normal();
    const auto base_dir = (normalized_source.filename().empty() && normalized_source.has_parent_path())
        ? normalized_source.parent_path()
        : normalized_source;

    std::vector<std::string> sorted_entries = entries;
    std::sort(sorted_entries.begin(), sorted_entries.end());
    sorted_entries.erase(std::unique(sorted_entries.begin(), sorted_entries.end()), sorted_entries.end());

    auto write_path = [&](const std::filesystem::path& full_path) -> bool {
        std::error_code ec;
        const auto rel = full_path.lexically_relative(base_dir);
        if (rel.empty()) {
            if (error) *error = "failed resolving relative path: " + full_path.string();
            return false;
        }
        std::string rel_str = rel.generic_string();
        std::string cleaned;
        if (!CleanSafeRelativePath(rel_str, cleaned)) {
            if (error) *error = "unsafe path in source directory: " + rel_str;
            return false;
        }
        const auto st = std::filesystem::symlink_status(full_path, ec);
        if (ec) {
            if (error) *error = "failed stat on " + full_path.string() + ": " + ec.message();
            return false;
        }
        if (st.type() == std::filesystem::file_type::symlink) {
            const auto target = std::filesystem::read_symlink(full_path, ec);
            if (ec) {
                if (error) *error = "failed readlink on " + full_path.string();
                return false;
            }
            const std::string link_str = target.generic_string();
            if (!IsSafeSymlinkTarget(cleaned, link_str)) {
                if (error) *error = "symlink escapes archive root: " + cleaned + " -> " + link_str;
                return false;
            }
            return WriteTarEntry(gw, cleaned, '2', 0777, 0, link_str, nullptr, error);
        }
        if (st.type() == std::filesystem::file_type::directory) {
            return WriteTarEntry(gw, cleaned + "/", '5', 0755, 0, {}, nullptr, error);
        }
        if (st.type() == std::filesystem::file_type::regular) {
            const auto file_size = std::filesystem::file_size(full_path, ec);
            if (ec) {
                if (error) *error = "failed reading size of " + full_path.string();
                return false;
            }
            std::uint32_t perm = 0644;
#if !defined(_WIN32)
            struct stat sb{};
            if (lstat(full_path.c_str(), &sb) == 0) perm = static_cast<std::uint32_t>(sb.st_mode & 0777);
#else
            const auto ext = full_path.extension().string();
            if (ext == ".exe" || ext == ".dll" || ext == ".cmd" || ext == ".bat") perm = 0755;
#endif
            return WriteTarEntry(gw, cleaned, '0', perm, file_size, {}, &full_path, error);
        }
        if (error) *error = "unsupported file type: " + full_path.string();
        return false;
    };

    for (const auto& top : sorted_entries) {
        const auto top_path = base_dir / top;
        std::error_code ec;
        const auto st = std::filesystem::symlink_status(top_path, ec);
        if (ec) {
            if (error) *error = "missing entry: " + top;
            return false;
        }
        if (!write_path(top_path)) return false;
        if (st.type() == std::filesystem::file_type::directory) {
            std::vector<std::filesystem::path> children;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     top_path, std::filesystem::directory_options::none, ec)) {
                if (ec) {
                    if (error) *error = "directory walk failed: " + ec.message();
                    return false;
                }
                children.push_back(entry.path());
            }
            std::sort(children.begin(), children.end());
            for (const auto& child : children) {
                if (!write_path(child)) return false;
            }
        }
    }

    const std::array<std::uint8_t, 1024> eof_blocks{};
    if (!gw.Write(eof_blocks.data(), eof_blocks.size(), error)) return false;

    std::int64_t final_size{};
    Sha256Digest final_sha{};
    if (!gw.Close(final_size, final_sha, error)) return false;
    if (out_size) *out_size = final_size;
    if (out_sha256) *out_sha256 = final_sha;
    return true;
}

bool ExtractTarGzArchive(const std::filesystem::path& archive_path,
                         const std::filesystem::path& dest_dir,
                         std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        if (error) *error = "could not create extraction directory: " + ec.message();
        return false;
    }

    GzipStreamReader gr;
    if (!gr.Open(archive_path, error)) return false;

    std::uint64_t total_extracted = 0;
    std::string pax_path;
    std::string pax_link;
    std::optional<std::uint64_t> pax_size;
    std::string gnu_long_name;
    std::string gnu_long_link;

    while (true) {
        std::uint8_t block[512];
        bool eof = false;
        if (!gr.ReadExact(block, sizeof(block), eof, error)) {
            if (eof) return true;
            return false;
        }
        if (IsZeroBlock(block)) return true;

        TarHeader hdr{};
        std::memcpy(&hdr, block, sizeof(hdr));
        if (!VerifyTarChecksum(hdr)) {
            if (error) *error = "invalid tar header checksum";
            return false;
        }
        std::uint64_t size = 0;
        if (!ParseTarOctal(hdr.size, sizeof(hdr.size), size)) {
            if (error) *error = "invalid tar entry size";
            return false;
        }
        std::uint64_t mode = 0644;
        ParseTarOctal(hdr.mode, sizeof(hdr.mode), mode);

        const std::uint64_t pad = (512 - (size % 512)) % 512;

        // PAX extended header ('x') or GNU longname ('L') / longlink ('K')
        if (hdr.typeflag == 'x' || hdr.typeflag == 'g' || hdr.typeflag == 'L' || hdr.typeflag == 'K') {
            if (size > (1ULL << 20)) {
                if (error) *error = "extended tar header too large";
                return false;
            }
            std::string meta(static_cast<std::size_t>(size), '\0');
            if (size > 0 && !gr.ReadExact(meta.data(), meta.size(), eof, error)) return false;
            if (pad > 0) {
                std::uint8_t discard[512];
                if (!gr.ReadExact(discard, static_cast<std::size_t>(pad), eof, error)) return false;
            }
            if (hdr.typeflag == 'x') {
                ParsePaxRecords(meta, pax_path, pax_link, pax_size);
            } else if (hdr.typeflag == 'L') {
                while (!meta.empty() && meta.back() == '\0') meta.pop_back();
                gnu_long_name = std::move(meta);
            } else if (hdr.typeflag == 'K') {
                while (!meta.empty() && meta.back() == '\0') meta.pop_back();
                gnu_long_link = std::move(meta);
            }
            continue;
        }

        std::string raw_name;
        if (!pax_path.empty()) {
            raw_name = std::move(pax_path);
        } else if (!gnu_long_name.empty()) {
            raw_name = std::move(gnu_long_name);
        } else {
            const std::string prefix = ReadTarField(hdr.prefix, sizeof(hdr.prefix));
            const std::string name = ReadTarField(hdr.name, sizeof(hdr.name));
            raw_name = prefix.empty() ? name : (prefix + "/" + name);
        }
        std::string raw_link;
        if (!pax_link.empty()) raw_link = std::move(pax_link);
        else if (!gnu_long_link.empty()) raw_link = std::move(gnu_long_link);
        else raw_link = ReadTarField(hdr.linkname, sizeof(hdr.linkname));

        if (pax_size) {
            size = *pax_size;
            pax_size.reset();
        }
        pax_path.clear();
        pax_link.clear();
        gnu_long_name.clear();
        gnu_long_link.clear();

        // Skip "." root directory entry if present
        if (raw_name == "." || raw_name == "./") {
            if (size > 0 || pad > 0) {
                std::uint64_t skip = size + ((512 - (size % 512)) % 512);
                std::array<std::uint8_t, 4096> discard{};
                while (skip > 0) {
                    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(skip, discard.size()));
                    if (!gr.ReadExact(discard.data(), chunk, eof, error)) return false;
                    skip -= chunk;
                }
            }
            continue;
        }

        std::string safe_rel;
        if (!CleanSafeRelativePath(raw_name, safe_rel)) {
            if (error) *error = "invalid path in update archive: " + raw_name;
            return false;
        }
        const auto target = dest_dir / std::filesystem::path(safe_rel);
        const char tf = hdr.typeflag == '\0' ? '0' : hdr.typeflag;

        if (tf == '5') {
            std::filesystem::create_directories(target, ec);
            if (ec) {
                if (error) *error = "could not create directory: " + target.string();
                return false;
            }
        } else if (tf == '0') {
            if (size > kMaxArchiveBytes || total_extracted + size > kMaxArchiveBytes) {
                if (error) *error = "extracted archive exceeds size limit";
                return false;
            }
            total_extracted += size;
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec) {
                if (error) *error = "could not create parent directory: " + target.parent_path().string();
                return false;
            }
            {
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (!out) {
                    if (error) *error = "could not create file: " + target.string();
                    return false;
                }
                std::array<std::uint8_t, 32768> buf{};
                std::uint64_t remaining = size;
                while (remaining > 0) {
                    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buf.size()));
                    if (!gr.ReadExact(buf.data(), chunk, eof, error)) return false;
                    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(chunk));
                    if (!out) {
                        if (error) *error = "failed writing extracted file: " + target.string();
                        return false;
                    }
                    remaining -= chunk;
                }
            }
            const std::uint64_t actual_pad = (512 - (size % 512)) % 512;
            if (actual_pad > 0) {
                std::uint8_t discard[512];
                if (!gr.ReadExact(discard, static_cast<std::size_t>(actual_pad), eof, error)) return false;
            }
#if !defined(_WIN32)
            chmod(target.c_str(), static_cast<mode_t>(mode & 0777));
#endif
        } else if (tf == '2') {
            if (!IsSafeSymlinkTarget(safe_rel, raw_link)) {
                if (error) *error = "update archive symlink escapes root: " + safe_rel;
                return false;
            }
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec) {
                if (error) *error = "could not create parent directory for symlink";
                return false;
            }
            std::filesystem::remove(target, ec);
            ec.clear();
            std::filesystem::create_symlink(std::filesystem::path(raw_link), target, ec);
            if (ec) {
                if (error) *error = "could not create symlink: " + ec.message();
                return false;
            }
        } else {
            if (error) *error = std::string("unsupported entry type in update archive: ") + tf;
            return false;
        }
    }
}

std::optional<Sha256Digest> HashUpdateFile(const std::filesystem::path& archive_path,
                                          std::int64_t expected_size, std::string* error) {
    if (expected_size <= 0 || static_cast<std::uint64_t>(expected_size) > kMaxArchiveBytes) {
        if (error) *error = "invalid expected archive size";
        return std::nullopt;
    }
    std::ifstream in(archive_path, std::ios::binary);
    if (!in) {
        if (error) *error = "could not open archive for verification";
        return std::nullopt;
    }
    Sha256 sha;
    std::array<std::uint8_t, 65536> buf{};
    std::int64_t total = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const auto got = in.gcount();
        if (got > 0) {
            total += static_cast<std::int64_t>(got);
            if (total > expected_size) {
                if (error) *error = "archive size exceeds manifest size";
                return std::nullopt;
            }
            sha.Update(buf.data(), static_cast<std::size_t>(got));
        }
    }
    if (total != expected_size || !in.eof() || in.bad()) {
        if (error) *error = "archive size mismatch";
        return std::nullopt;
    }
    return sha.Final();
}

bool VerifyArchiveFile(const std::filesystem::path& archive_path,
                       std::int64_t expected_size,
                       std::string_view expected_signature_b64,
                       std::string_view public_key_b64,
                       std::string* error) {
    const auto pub = ParsePublicKeyBase64(public_key_b64);
    if (!pub) {
        if (error) *error = "invalid embedded update public key";
        return false;
    }
    const auto digest = HashUpdateFile(archive_path, expected_size, error);
    if (!digest) return false;
    if (!VerifyDigestBase64(*pub, *digest, expected_signature_b64)) {
        if (error) *error = "update archive signature verification failed";
        return false;
    }
    return true;
}

bool SwapDirectoryFiles(const std::filesystem::path& staged_dir,
                        const std::filesystem::path& target_dir,
                        std::string_view running_entry,
                        std::string* error) {
    std::error_code ec;
    std::vector<std::string> names;
    std::set<std::string> ours;
    for (const auto& entry : std::filesystem::directory_iterator(staged_dir, ec)) {
        if (ec) {
            if (error) *error = "could not read staged directory: " + ec.message();
            return false;
        }
        const std::string name = entry.path().filename().string();
        if (name == kManagedFileListName) continue;
        names.push_back(name);
        ours.insert(name);
    }
    if (names.empty()) {
        if (error) *error = "staged update directory is empty";
        return false;
    }

    const std::string suffix = ".old-" + std::to_string(CurrentPid());
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;   // {orig, aside}
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> placed;  // {src, dst}

    auto undo = [&]() noexcept {
        for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
            std::error_code uec;
            std::filesystem::rename(it->second, it->first, uec);
            if (uec) std::filesystem::remove_all(it->second, uec);
        }
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code uec;
            std::filesystem::rename(it->second, it->first, uec);
        }
    };

    auto move_aside = [&](const std::filesystem::path& dst, std::string* err_out) -> bool {
        const auto aside = target_dir / ("." + dst.filename().string() + suffix);
        std::error_code mec;
        std::filesystem::remove_all(aside, mec);
        mec.clear();
        std::filesystem::rename(dst, aside, mec);
        if (mec) {
            if (err_out) *err_out = "could not move aside " + dst.string() + ": " + mec.message();
            return false;
        }
        moved.emplace_back(dst, aside);
        return true;
    };

    // 1. Move aside files installed by previous version that the new version removed.
    std::vector<std::string> left;
    for (const auto& old_name : ReadFileList(target_dir)) {
        if (ours.contains(old_name) || old_name == running_entry) continue;
        const auto dst = target_dir / old_name;
        std::error_code lec;
        if (!std::filesystem::exists(std::filesystem::symlink_status(dst, lec)) || lec) continue;
        if (!move_aside(dst, nullptr)) {
            left.push_back(old_name);
        }
    }

    std::vector<std::string> next_list = names;
    next_list.insert(next_list.end(), left.begin(), left.end());
    if (!WriteFileList(staged_dir, next_list, error)) {
        undo();
        return false;
    }

    // 2. Move new entries + updated .p2premote-files into place.
    std::vector<std::string> to_place = names;
    to_place.emplace_back(kManagedFileListName);
    for (const auto& name : to_place) {
        const auto src = staged_dir / name;
        const auto dst = target_dir / name;
        std::error_code lec;
        if (std::filesystem::exists(std::filesystem::symlink_status(dst, lec)) && !lec) {
            if (!move_aside(dst, error)) {
                undo();
                return false;
            }
        }
        std::error_code rec;
        std::filesystem::rename(src, dst, rec);
        if (rec) {
            if (error) *error = "could not place " + dst.string() + ": " + rec.message();
            undo();
            return false;
        }
        placed.emplace_back(src, dst);
    }

    // 3. Best-effort cleanup of aside backups (running exe/dlls on Windows stay until next launch).
    for (const auto& m : moved) {
        std::error_code cec;
        std::filesystem::remove_all(m.second, cec);
    }
    return true;
}

void CleanUpdateLeftovers(const std::filesystem::path& target_dir) noexcept {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(target_dir, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.starts_with('.') && name.find(".old-") != std::string::npos) {
            std::error_code rm_ec;
            std::filesystem::remove_all(entry.path(), rm_ec);
        }
    }
}

bool IsDirectoryWritable(const std::filesystem::path& dir) noexcept {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    const auto probe = dir / (".p2premote-write-probe-" + std::to_string(CurrentPid()));
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) return false;
    }
    std::filesystem::remove(probe, ec);
    return true;
}

} // namespace p2premote::update
