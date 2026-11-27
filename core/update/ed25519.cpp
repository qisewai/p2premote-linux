#include "ed25519.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace p2premote::update {
namespace {

inline std::uint32_t Rotr32(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }
inline std::uint64_t Rotr64(std::uint64_t x, unsigned n) noexcept { return (x >> n) | (x << (64 - n)); }

inline std::uint32_t LoadBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void StoreBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

inline std::uint64_t LoadBe64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(LoadBe32(p)) << 32) | LoadBe32(p + 4);
}

inline void StoreBe64(std::uint8_t* p, std::uint64_t v) noexcept {
    StoreBe32(p, static_cast<std::uint32_t>(v >> 32));
    StoreBe32(p + 4, static_cast<std::uint32_t>(v));
}

// ============================================================================
// SHA-512 (used internally by RFC 8032 Ed25519)
// ============================================================================

class Sha512 {
public:
    Sha512() noexcept {
        h_[0] = 0x6a09e667f3bcc908ULL; h_[1] = 0xbb67ae8584caa73bULL;
        h_[2] = 0x3c6ef372fe94f82bULL; h_[3] = 0xa54ff53a5f1d36f1ULL;
        h_[4] = 0x510e527fade682d1ULL; h_[5] = 0x9b05688c2b3e6c1fULL;
        h_[6] = 0x1f83d9abfb41bd6bULL; h_[7] = 0x5be0cd19137e2179ULL;
    }

    void Update(const void* data, std::size_t len) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        total_lo_ += len;
        if (total_lo_ < len) ++total_hi_;
        if (buf_len_ > 0) {
            const std::size_t take = std::min(len, sizeof(buf_) - buf_len_);
            std::memcpy(buf_ + buf_len_, bytes, take);
            buf_len_ += take;
            bytes += take;
            len -= take;
            if (buf_len_ == sizeof(buf_)) {
                Transform(buf_);
                buf_len_ = 0;
            }
        }
        while (len >= sizeof(buf_)) {
            Transform(bytes);
            bytes += sizeof(buf_);
            len -= sizeof(buf_);
        }
        if (len > 0) {
            std::memcpy(buf_, bytes, len);
            buf_len_ = len;
        }
    }

    void Final(std::uint8_t out[64]) noexcept {
        const std::uint64_t bits_lo = total_lo_ << 3;
        const std::uint64_t bits_hi = (total_hi_ << 3) | (total_lo_ >> 61);
        buf_[buf_len_++] = 0x80;
        if (buf_len_ > 112) {
            std::memset(buf_ + buf_len_, 0, sizeof(buf_) - buf_len_);
            Transform(buf_);
            buf_len_ = 0;
        }
        std::memset(buf_ + buf_len_, 0, 112 - buf_len_);
        StoreBe64(buf_ + 112, bits_hi);
        StoreBe64(buf_ + 120, bits_lo);
        Transform(buf_);
        for (int i = 0; i < 8; ++i) StoreBe64(out + i * 8, h_[i]);
    }

private:
    void Transform(const std::uint8_t block[128]) noexcept {
        static constexpr std::uint64_t k[80] = {
            0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
            0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
            0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
            0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
        };
        std::uint64_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = LoadBe64(block + i * 8);
        for (int i = 16; i < 80; ++i) {
            const std::uint64_t s0 = Rotr64(w[i - 15], 1) ^ Rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const std::uint64_t s1 = Rotr64(w[i - 2], 19) ^ Rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint64_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 80; ++i) {
            const std::uint64_t s1 = Rotr64(e, 14) ^ Rotr64(e, 18) ^ Rotr64(e, 41);
            const std::uint64_t ch = (e & f) ^ ((~e) & g);
            const std::uint64_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint64_t s0 = Rotr64(a, 28) ^ Rotr64(a, 34) ^ Rotr64(a, 39);
            const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint64_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    std::uint64_t h_[8]{};
    std::uint64_t total_lo_{0};
    std::uint64_t total_hi_{0};
    std::uint8_t buf_[128]{};
    std::size_t buf_len_{0};
};

// ============================================================================
// TweetNaCl-derived Ed25519 over GF(2^255 - 19) (public domain math)
// ============================================================================

using gf = std::int64_t[16];

constexpr gf kGf0 = {0};
constexpr gf kGf1 = {1};
constexpr gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                   0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
constexpr gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
constexpr gf kBx = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
constexpr gf kBy = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
constexpr gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                   0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

inline void Set25519(gf r, const gf a) noexcept {
    for (int i = 0; i < 16; ++i) r[i] = a[i];
}

inline void Car25519(gf o) noexcept {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        const std::int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * (1LL << 16);
    }
}

inline void Sel25519(gf p, gf q, int b) noexcept {
    const std::int64_t c = ~(static_cast<std::int64_t>(b) - 1);
    for (int i = 0; i < 16; ++i) {
        const std::int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

inline void Pack25519(std::uint8_t o[32], const gf n) noexcept {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    Car25519(t);
    Car25519(t);
    Car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        Sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<std::uint8_t>(t[i] >> 8);
    }
}

inline int Neq25519(const gf a, const gf b) noexcept {
    std::uint8_t c[32], d[32];
    Pack25519(c, a);
    Pack25519(d, b);
    unsigned diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (c[i] ^ d[i]);
    return diff != 0 ? 1 : 0;
}

inline std::uint8_t Par25519(const gf a) noexcept {
    std::uint8_t d[32];
    Pack25519(d, a);
    return d[0] & 1;
}

inline void Unpack25519(gf o, const std::uint8_t n[32]) noexcept {
    for (int i = 0; i < 16; ++i) {
        o[i] = static_cast<std::int64_t>(n[2 * i]) + (static_cast<std::int64_t>(n[2 * i + 1]) << 8);
    }
    o[15] &= 0x7fff;
}

inline void AddGf(gf o, const gf a, const gf b) noexcept {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

inline void SubGf(gf o, const gf a, const gf b) noexcept {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

inline void MulGf(gf o, const gf a, const gf b) noexcept {
    std::int64_t t[31]{};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    Car25519(o);
    Car25519(o);
}

inline void SqrGf(gf o, const gf a) noexcept { MulGf(o, a, a); }

inline void Inv25519(gf o, const gf i) noexcept {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        SqrGf(c, c);
        if (a != 2 && a != 4) MulGf(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline void Pow2523(gf o, const gf i) noexcept {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) {
        SqrGf(c, c);
        if (a != 1) MulGf(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline void PointAdd(gf p[4], gf q[4]) noexcept {
    gf a, b, c, d, t, e, f, g, h;
    SubGf(a, p[1], p[0]);
    SubGf(t, q[1], q[0]);
    MulGf(a, a, t);
    AddGf(b, p[0], p[1]);
    AddGf(t, q[0], q[1]);
    MulGf(b, b, t);
    MulGf(c, p[3], q[3]);
    MulGf(c, c, kD2);
    MulGf(d, p[2], q[2]);
    AddGf(d, d, d);
    SubGf(e, b, a);
    SubGf(f, d, c);
    AddGf(g, d, c);
    AddGf(h, b, a);
    MulGf(p[0], e, f);
    MulGf(p[1], h, g);
    MulGf(p[2], g, f);
    MulGf(p[3], e, h);
}

inline void Cswap(gf p[4], gf q[4], std::uint8_t b) noexcept {
    for (int i = 0; i < 4; ++i) Sel25519(p[i], q[i], b);
}

inline void PackPoint(std::uint8_t r[32], gf p[4]) noexcept {
    gf tx, ty, zi;
    Inv25519(zi, p[2]);
    MulGf(tx, p[0], zi);
    MulGf(ty, p[1], zi);
    Pack25519(r, ty);
    r[31] ^= static_cast<std::uint8_t>(Par25519(tx) << 7);
}

inline void ScalarMult(gf p[4], gf q[4], const std::uint8_t s[32]) noexcept {
    Set25519(p[0], kGf0);
    Set25519(p[1], kGf1);
    Set25519(p[2], kGf1);
    Set25519(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        const std::uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        Cswap(p, q, b);
        PointAdd(q, p);
        PointAdd(p, p);
        Cswap(p, q, b);
    }
}

inline void ScalarBase(gf p[4], const std::uint8_t s[32]) noexcept {
    gf q[4];
    Set25519(q[0], kBx);
    Set25519(q[1], kBy);
    Set25519(q[2], kGf1);
    MulGf(q[3], kBx, kBy);
    ScalarMult(p, q, s);
}

// Group order L = 2^252 + 27742317777372353535851937790883648493
constexpr std::uint8_t kOrderL[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

inline bool ScalarIsCanonical(const std::uint8_t s[32]) noexcept {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < kOrderL[i]) return true;
        if (s[i] > kOrderL[i]) return false;
    }
    return false; // equal to L -> non-canonical
}

inline void ModL(std::uint8_t r[32], std::int64_t x[64]) noexcept {
    for (int i = 63; i >= 32; --i) {
        std::int64_t carry = 0;
        for (int j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * static_cast<std::int64_t>(kOrderL[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    std::int64_t carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * static_cast<std::int64_t>(kOrderL[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * static_cast<std::int64_t>(kOrderL[j]);
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<std::uint8_t>(x[i] & 255);
    }
}

inline void Reduce(std::uint8_t r[64]) noexcept {
    std::int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = r[i];
    for (int i = 0; i < 64; ++i) r[i] = 0;
    ModL(r, x);
}

inline bool UnpackNeg(gf r[4], const std::uint8_t p[32]) noexcept {
    gf t, chk, num, den, den2, den4, den6;
    Set25519(r[2], kGf1);
    Unpack25519(r[1], p);
    SqrGf(num, r[1]);
    MulGf(den, num, kD);
    SubGf(num, num, r[2]);
    AddGf(den, r[2], den);

    SqrGf(den2, den);
    SqrGf(den4, den2);
    MulGf(den6, den4, den2);
    MulGf(t, den6, num);
    MulGf(t, t, den);

    Pow2523(t, t);
    MulGf(t, t, num);
    MulGf(t, t, den);
    MulGf(t, t, den);
    MulGf(r[0], t, den);

    SqrGf(chk, r[0]);
    MulGf(chk, chk, den);
    if (Neq25519(chk, num)) MulGf(r[0], r[0], kI);

    SqrGf(chk, r[0]);
    MulGf(chk, chk, den);
    if (Neq25519(chk, num)) return false;

    if (Par25519(r[0]) == (p[31] >> 7)) SubGf(r[0], kGf0, r[0]);
    MulGf(r[3], r[0], r[1]);
    return true;
}

bool FillRandom(std::uint8_t* dst, std::size_t len) noexcept {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::size_t offset = 0;
    while (offset < len) {
        const ssize_t n = read(fd, dst + offset, len - offset);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    close(fd);
    return true;
#endif
}

std::string_view TrimAscii(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

// ============================================================================
// SHA-256
// ============================================================================

Sha256::Sha256() noexcept {
    state_[0] = 0x6a09e667U; state_[1] = 0xbb67ae85U;
    state_[2] = 0x3c6ef372U; state_[3] = 0xa54ff53aU;
    state_[4] = 0x510e527fU; state_[5] = 0x9b05688cU;
    state_[6] = 0x1f83d9abU; state_[7] = 0x5be0cd19U;
}

void Sha256::Update(const void* data, std::size_t len) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    bit_count_ += static_cast<std::uint64_t>(len) * 8ULL;
    if (buffer_len_ > 0) {
        const std::size_t take = std::min(len, sizeof(buffer_) - buffer_len_);
        std::memcpy(buffer_ + buffer_len_, bytes, take);
        buffer_len_ += take;
        bytes += take;
        len -= take;
        if (buffer_len_ == sizeof(buffer_)) {
            Transform(buffer_);
            buffer_len_ = 0;
        }
    }
    while (len >= sizeof(buffer_)) {
        Transform(bytes);
        bytes += sizeof(buffer_);
        len -= sizeof(buffer_);
    }
    if (len > 0) {
        std::memcpy(buffer_, bytes, len);
        buffer_len_ = len;
    }
}

Sha256Digest Sha256::Final() noexcept {
    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > 56) {
        std::memset(buffer_ + buffer_len_, 0, sizeof(buffer_) - buffer_len_);
        Transform(buffer_);
        buffer_len_ = 0;
    }
    std::memset(buffer_ + buffer_len_, 0, 56 - buffer_len_);
    StoreBe64(buffer_ + 56, bit_count_);
    Transform(buffer_);
    Sha256Digest out{};
    for (int i = 0; i < 8; ++i) StoreBe32(out.data() + i * 4, state_[i]);
    return out;
}

void Sha256::Transform(const std::uint8_t block[64]) noexcept {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = LoadBe32(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
        const std::uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

Sha256Digest Sha256::Hash(const void* data, std::size_t len) noexcept {
    Sha256 h;
    h.Update(data, len);
    return h.Final();
}

std::string Sha256::ToHex(const Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(digest.size() * 2, '\0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[2 * i] = kHex[digest[i] >> 4];
        out[2 * i + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

// ============================================================================
// Base64
// ============================================================================

std::string Base64Encode(const std::uint8_t* data, std::size_t len) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kTable[(triple >> 18) & 0x3f]);
        out.push_back(kTable[(triple >> 12) & 0x3f]);
        out.push_back((i + 1 < len) ? kTable[(triple >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < len) ? kTable[triple & 0x3f] : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view text) {
    text = TrimAscii(text);
    if (text.size() % 4 != 0) return std::nullopt;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const int v0 = val(text[i]);
        const int v1 = val(text[i + 1]);
        const bool p2 = (text[i + 2] == '=');
        const bool p3 = (text[i + 3] == '=');
        if (v0 < 0 || v1 < 0) return std::nullopt;
        if (p2 && !p3) return std::nullopt;
        if ((p2 || p3) && i + 4 != text.size()) return std::nullopt;
        const int v2 = p2 ? 0 : val(text[i + 2]);
        const int v3 = p3 ? 0 : val(text[i + 3]);
        if (v2 < 0 || v3 < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
        if (!p2) out.push_back(static_cast<std::uint8_t>(((v1 & 0x0f) << 4) | (v2 >> 2)));
        if (!p3) out.push_back(static_cast<std::uint8_t>(((v2 & 0x03) << 6) | v3));
    }
    return out;
}

// ============================================================================
// RFC 8032 Ed25519 public API
// ============================================================================

void Ed25519KeypairFromSeed(const std::uint8_t seed[kEd25519SeedSize],
                            Ed25519PublicKey& pub,
                            Ed25519PrivateKey& priv) noexcept {
    std::uint8_t d[64];
    Sha512 h;
    h.Update(seed, kEd25519SeedSize);
    h.Final(d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    gf p[4];
    ScalarBase(p, d);
    PackPoint(pub.data(), p);
    std::memcpy(priv.data(), seed, 32);
    std::memcpy(priv.data() + 32, pub.data(), 32);
}

bool Ed25519GenerateKeypair(Ed25519PublicKey& pub, Ed25519PrivateKey& priv) noexcept {
    std::uint8_t seed[kEd25519SeedSize];
    if (!FillRandom(seed, sizeof(seed))) return false;
    Ed25519KeypairFromSeed(seed, pub, priv);
    std::memset(seed, 0, sizeof(seed));
    return true;
}

Ed25519Signature Ed25519Sign(const Ed25519PrivateKey& priv, const void* msg, std::size_t len) noexcept {
    std::uint8_t d[64];
    {
        Sha512 h;
        h.Update(priv.data(), 32);
        h.Final(d);
    }
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    std::uint8_t r[64];
    {
        Sha512 h;
        h.Update(d + 32, 32);
        if (len > 0) h.Update(msg, len);
        h.Final(r);
    }
    Reduce(r);

    gf p[4];
    ScalarBase(p, r);
    Ed25519Signature sig{};
    PackPoint(sig.data(), p);

    std::uint8_t hram[64];
    {
        Sha512 h;
        h.Update(sig.data(), 32);
        h.Update(priv.data() + 32, 32);
        if (len > 0) h.Update(msg, len);
        h.Final(hram);
    }
    Reduce(hram);

    std::int64_t x[64]{};
    for (int i = 0; i < 32; ++i) x[i] = r[i];
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x[i + j] += static_cast<std::int64_t>(hram[i]) * static_cast<std::int64_t>(d[j]);
        }
    }
    ModL(sig.data() + 32, x);
    return sig;
}

bool Ed25519Verify(const Ed25519PublicKey& pub, const void* msg, std::size_t len,
                   const Ed25519Signature& sig) noexcept {
    if (!ScalarIsCanonical(sig.data() + 32)) return false;

    gf p[4], q[4];
    if (!UnpackNeg(q, pub.data())) return false;

    std::uint8_t h[64];
    {
        Sha512 sha;
        sha.Update(sig.data(), 32);
        sha.Update(pub.data(), 32);
        if (len > 0) sha.Update(msg, len);
        sha.Final(h);
    }
    Reduce(h);

    ScalarMult(p, q, h);
    ScalarBase(q, sig.data() + 32);
    PointAdd(p, q);

    std::uint8_t t[32];
    PackPoint(t, p);
    unsigned diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (t[i] ^ sig[i]);
    return diff == 0;
}

std::optional<Ed25519PublicKey> ParsePublicKeyBase64(std::string_view text) {
    const auto decoded = Base64Decode(text);
    if (!decoded || decoded->size() != kEd25519PublicKeySize) return std::nullopt;
    Ed25519PublicKey out{};
    std::memcpy(out.data(), decoded->data(), out.size());
    return out;
}

std::optional<Ed25519PrivateKey> ParsePrivateKeyBase64(std::string_view text) {
    const auto decoded = Base64Decode(text);
    if (!decoded || decoded->size() != kEd25519PrivateKeySize) return std::nullopt;
    Ed25519PrivateKey out{};
    std::memcpy(out.data(), decoded->data(), out.size());
    return out;
}

std::string SignDigestBase64(const Ed25519PrivateKey& priv, const Sha256Digest& digest) {
    const auto sig = Ed25519Sign(priv, digest.data(), digest.size());
    return Base64Encode(sig.data(), sig.size());
}

bool VerifyDigestBase64(const Ed25519PublicKey& pub, const Sha256Digest& digest, std::string_view sig_b64) {
    const auto decoded = Base64Decode(sig_b64);
    if (!decoded || decoded->size() != kEd25519SignatureSize) return false;
    Ed25519Signature sig{};
    std::memcpy(sig.data(), decoded->data(), sig.size());
    return Ed25519Verify(pub, digest.data(), digest.size(), sig);
}

} // namespace p2premote::update
