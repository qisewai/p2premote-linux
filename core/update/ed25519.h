#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote::update {

inline constexpr std::size_t kEd25519SeedSize = 32;
inline constexpr std::size_t kEd25519PublicKeySize = 32;
inline constexpr std::size_t kEd25519PrivateKeySize = 64; // seed (32) || public key (32), RFC 8032 / Go compatible
inline constexpr std::size_t kEd25519SignatureSize = 64;
inline constexpr std::size_t kSha256DigestSize = 32;

using Sha256Digest = std::array<std::uint8_t, kSha256DigestSize>;
using Ed25519PublicKey = std::array<std::uint8_t, kEd25519PublicKeySize>;
using Ed25519PrivateKey = std::array<std::uint8_t, kEd25519PrivateKeySize>;
using Ed25519Signature = std::array<std::uint8_t, kEd25519SignatureSize>;

class Sha256 {
public:
    Sha256() noexcept;
    void Update(const void* data, std::size_t len) noexcept;
    Sha256Digest Final() noexcept;

    static Sha256Digest Hash(const void* data, std::size_t len) noexcept;
    static std::string ToHex(const Sha256Digest& digest);

private:
    void Transform(const std::uint8_t block[64]) noexcept;

    std::uint32_t state_[8]{};
    std::uint64_t bit_count_{0};
    std::uint8_t buffer_[64]{};
    std::size_t buffer_len_{0};
};

std::string Base64Encode(const std::uint8_t* data, std::size_t len);
std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view text);

// RFC 8032 Ed25519 (wire-compatible with Go crypto/ed25519 and mygo).
void Ed25519KeypairFromSeed(const std::uint8_t seed[kEd25519SeedSize],
                            Ed25519PublicKey& pub,
                            Ed25519PrivateKey& priv) noexcept;
bool Ed25519GenerateKeypair(Ed25519PublicKey& pub, Ed25519PrivateKey& priv) noexcept;
Ed25519Signature Ed25519Sign(const Ed25519PrivateKey& priv, const void* msg, std::size_t len) noexcept;
bool Ed25519Verify(const Ed25519PublicKey& pub, const void* msg, std::size_t len,
                   const Ed25519Signature& sig) noexcept;

std::optional<Ed25519PublicKey> ParsePublicKeyBase64(std::string_view text);
std::optional<Ed25519PrivateKey> ParsePrivateKeyBase64(std::string_view text);
std::string SignDigestBase64(const Ed25519PrivateKey& priv, const Sha256Digest& digest);
bool VerifyDigestBase64(const Ed25519PublicKey& pub, const Sha256Digest& digest, std::string_view sig_b64);

} // namespace p2premote::update
