#pragma once

#include "ed25519.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote::update {

inline constexpr std::string_view kManagedFileListName = ".p2premote-files";
inline constexpr std::uint64_t kMaxManifestBytes = 1ULL << 20;   // 1 MiB
inline constexpr std::uint64_t kMaxArchiveBytes = 1ULL << 30;    // 1 GiB

struct UpdateDelta {
    std::string from;
    std::string url;
    std::int64_t size{};
    std::string signature;
};

struct UpdateArchiveRef {
    std::string version;
    std::string url;
    std::int64_t size{};
    std::string signature;
};

// Wire-compatible with mygo's update-<target>.json manifest format.
struct UpdateManifest {
    std::string version;
    std::string notes;
    std::string date;
    std::string url;
    std::int64_t size{};
    std::string signature;
    std::vector<UpdateDelta> deltas;
    std::vector<UpdateArchiveRef> previous;
    std::string target;
    std::string manifest_signature;
};

// SemVer comparison matching mygo: ignores leading 'v' and '+build', compares numeric parts
// numerically, and ranks pre-releases before their corresponding release.
int CompareVersions(std::string_view a, std::string_view b) noexcept;

// Extracts the "## <version>" or "## [<version>]" section from a Markdown changelog.
std::string ExtractReleaseNotes(std::string_view changelog, std::string_view version);

// Minimal, strict JSON parse/serialize for UpdateManifest (no external JSON dependency).
std::optional<UpdateManifest> ParseUpdateManifest(std::string_view json, std::string* error = nullptr);
std::string SerializeUpdateManifest(const UpdateManifest& manifest);
bool IsReleaseVersion(std::string_view version) noexcept;
void SignUpdateManifest(UpdateManifest& manifest, const Ed25519PrivateKey& key);
bool VerifyUpdateManifest(const UpdateManifest& manifest, std::string_view target,
                          std::string_view public_key_b64);

// Managed entry list (.p2premote-files) helpers (same semantics as mygo's .mygo-files).
bool IsValidFileListEntry(std::string_view name) noexcept;
std::vector<std::string> ParseFileList(std::string_view content);
std::vector<std::string> ReadFileList(const std::filesystem::path& dir);
bool WriteFileList(const std::filesystem::path& dir, std::vector<std::string> entries, std::string* error = nullptr);

// Creates a gzip-compressed POSIX tar archive of top-level entries inside source_dir.
// Returns archive size and SHA-256 digest on success.
bool WriteTarGzArchive(const std::filesystem::path& source_dir,
                       const std::vector<std::string>& entries,
                       const std::filesystem::path& archive_path,
                       std::int64_t* out_size = nullptr,
                       Sha256Digest* out_sha256 = nullptr,
                       std::string* error = nullptr);

// Safely extracts a gzip-compressed POSIX tar archive into dest_dir.
// Rejects absolute paths, path traversal, symlinks escaping dest_dir, and non-file/dir/symlink entries.
bool ExtractTarGzArchive(const std::filesystem::path& archive_path,
                         const std::filesystem::path& dest_dir,
                         std::string* error = nullptr);

// Computes SHA-256 and verifies exact size and Ed25519 signature of archive_path.
bool VerifyArchiveFile(const std::filesystem::path& archive_path,
                       std::int64_t expected_size,
                       std::string_view expected_signature_b64,
                       std::string_view public_key_b64,
                       std::string* error = nullptr);
std::optional<Sha256Digest> HashUpdateFile(const std::filesystem::path& path,
                                          std::int64_t expected_size, std::string* error = nullptr);

// Atomically swaps top-level entries from staged_dir into target_dir using .p2premote-files.
// Preserves unmanaged files and rolls back all moves on any error.
// running_entry (optional, basename of currently executing binary inside target_dir) is never removed
// when absent from staged_dir.
bool SwapDirectoryFiles(const std::filesystem::path& staged_dir,
                        const std::filesystem::path& target_dir,
                        std::string_view running_entry,
                        std::string* error = nullptr);

// Removes .*.old-* leftovers from previous updates inside target_dir.
void CleanUpdateLeftovers(const std::filesystem::path& target_dir) noexcept;

// Checks whether target_dir (and its parent for staging) is writable by the current user.
bool IsDirectoryWritable(const std::filesystem::path& dir) noexcept;

} // namespace p2premote::update
