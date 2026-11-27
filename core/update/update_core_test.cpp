#include "ed25519.h"
#include "update_archive.h"

#include <zlib.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(expr)                                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " #expr "\n";      \
            std::exit(1);                                                                           \
        }                                                                                           \
    } while (0)

namespace {

using namespace p2premote::update;

std::vector<std::uint8_t> FromHex(std::string_view hex) {
    CHECK(hex.size() % 2 == 0);
    std::vector<std::uint8_t> out(hex.size() / 2);
    auto nib = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
        CHECK(false);
        return 0;
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
    }
    return out;
}

void TestSha256() {
    const auto empty = Sha256::Hash("", 0);
    CHECK(Sha256::ToHex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const auto abc = Sha256::Hash("abc", 3);
    CHECK(Sha256::ToHex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void TestEd25519Rfc8032() {
    // RFC 8032 Section 7.1 Test Vector 1 (empty message)
    {
        const auto seed = FromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto expected_pub = FromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        const auto expected_sig = FromHex(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        Ed25519PublicKey pub{};
        Ed25519PrivateKey priv{};
        Ed25519KeypairFromSeed(seed.data(), pub, priv);
        CHECK(std::memcmp(pub.data(), expected_pub.data(), 32) == 0);
        const auto sig = Ed25519Sign(priv, "", 0);
        CHECK(std::memcmp(sig.data(), expected_sig.data(), 64) == 0);
        CHECK(Ed25519Verify(pub, "", 0, sig));

        auto bad_sig = sig;
        bad_sig[0] ^= 0x01;
        CHECK(!Ed25519Verify(pub, "", 0, bad_sig));
    }

    // RFC 8032 Section 7.1 Test Vector 2 (1-byte message 0x72)
    {
        const auto seed = FromHex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
        const auto expected_pub = FromHex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
        const auto expected_sig = FromHex(
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
        const std::uint8_t msg[1] = {0x72};
        Ed25519PublicKey pub{};
        Ed25519PrivateKey priv{};
        Ed25519KeypairFromSeed(seed.data(), pub, priv);
        CHECK(std::memcmp(pub.data(), expected_pub.data(), 32) == 0);
        const auto sig = Ed25519Sign(priv, msg, 1);
        CHECK(std::memcmp(sig.data(), expected_sig.data(), 64) == 0);
        CHECK(Ed25519Verify(pub, msg, 1, sig));
        CHECK(!Ed25519Verify(pub, "", 0, sig));
    }

    // RFC 8032 Section 7.1 Test Vector 3 (2-byte message 0xaf82)
    {
        const auto seed = FromHex("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7");
        const auto expected_pub = FromHex("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025");
        const auto expected_sig = FromHex(
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a");
        const std::uint8_t msg[2] = {0xaf, 0x82};
        Ed25519PublicKey pub{};
        Ed25519PrivateKey priv{};
        Ed25519KeypairFromSeed(seed.data(), pub, priv);
        CHECK(std::memcmp(pub.data(), expected_pub.data(), 32) == 0);
        const auto sig = Ed25519Sign(priv, msg, 2);
        CHECK(std::memcmp(sig.data(), expected_sig.data(), 64) == 0);
        CHECK(Ed25519Verify(pub, msg, 2, sig));
    }
}

void TestVersionsAndNotes() {
    CHECK(CompareVersions("0.2.0", "0.1.0") > 0);
    CHECK(CompareVersions("v1.2.10", "1.2.2") > 0);
    CHECK(CompareVersions("1.3.0-beta.2", "1.3.0-beta.10") < 0);
    CHECK(CompareVersions("1.3.0-beta.10", "1.3.0") < 0);
    CHECK(CompareVersions("1.0.0+build1", "1.0.0+build2") == 0);

    const std::string changelog =
        "# Changelog\n\n"
        "## [0.2.0] - 2026-11-27\n\n"
        "- Auto updater\n"
        "- Linux plugins\n\n"
        "## 0.1.0\n\n"
        "- Initial\n";
    CHECK(ExtractReleaseNotes(changelog, "0.2.0") == "- Auto updater\n- Linux plugins");
    CHECK(ExtractReleaseNotes(changelog, "0.1.0") == "- Initial");
    CHECK(ExtractReleaseNotes(changelog, "0.3.0").empty());
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    CHECK(out.good());
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void TestArchiveVerifyAndSwap() {
    const auto temp_root = std::filesystem::temp_directory_path() / "p2premote-update-core-test";
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root);

    const auto src_v2 = temp_root / "src-v2";
    const auto app_dir = temp_root / "app";
    const auto extract_dir = temp_root / "extracted";
    const auto archive_path = temp_root / "update.tar.gz";

    // Prepare v1 app directory with a managed obsolete file and an unmanaged user config file.
    WriteFile(app_dir / "app.bin", "v1-binary");
    WriteFile(app_dir / "obsolete.dll", "old-unused-lib");
    WriteFile(app_dir / "user-local.ini", "keep-me=1");
    CHECK(WriteFileList(app_dir, {"app.bin", "obsolete.dll"}));

    // Prepare v2 staged directory.
    WriteFile(src_v2 / "app.bin", "v2-binary-updated");
    WriteFile(src_v2 / "plugins" / "demo" / "main.lua", "return 42");
    CHECK(WriteFileList(src_v2, {"app.bin", "plugins"}));

    Ed25519PublicKey pub{};
    Ed25519PrivateKey priv{};
    CHECK(Ed25519GenerateKeypair(pub, priv));
    const std::string pub_b64 = Base64Encode(pub.data(), pub.size());

    std::int64_t size{};
    Sha256Digest sha{};
    std::string err;
    CHECK(WriteTarGzArchive(src_v2, {"app.bin", "plugins", std::string(kManagedFileListName)},
                            archive_path, &size, &sha, &err));

    UpdateManifest m;
    m.version = "0.2.0";
    m.notes = "Release 0.2.0";
    m.date = "2026-11-27T06:00:00Z";
    m.url = "https://example.com/update.tar.gz";
    m.size = size;
    m.signature = SignDigestBase64(priv, sha);

    const std::string json = SerializeUpdateManifest(m);
    const auto parsed = ParseUpdateManifest(json, &err);
    CHECK(parsed.has_value());
    CHECK(parsed->version == "0.2.0");
    CHECK(parsed->notes == "Release 0.2.0");
    CHECK(parsed->size == size);
    CHECK(parsed->signature == m.signature);

    // Signature verification passes, and fails on wrong key or wrong size.
    CHECK(VerifyArchiveFile(archive_path, parsed->size, parsed->signature, pub_b64, &err));
    CHECK(!VerifyArchiveFile(archive_path, parsed->size + 1, parsed->signature, pub_b64, &err));
    Ed25519PublicKey other_pub{};
    Ed25519PrivateKey other_priv{};
    CHECK(Ed25519GenerateKeypair(other_pub, other_priv));
    CHECK(!VerifyArchiveFile(archive_path, parsed->size, parsed->signature,
                             Base64Encode(other_pub.data(), other_pub.size()), &err));

    // Extract & atomic swap
    CHECK(ExtractTarGzArchive(archive_path, extract_dir, &err));
    CHECK(SwapDirectoryFiles(extract_dir, app_dir, "app.bin", &err));

    CHECK(ReadFile(app_dir / "app.bin") == "v2-binary-updated");
    CHECK(ReadFile(app_dir / "plugins" / "demo" / "main.lua") == "return 42");
    CHECK(!std::filesystem::exists(app_dir / "obsolete.dll"));
    CHECK(ReadFile(app_dir / "user-local.ini") == "keep-me=1");

    // Leftover cleanup
    WriteFile(app_dir / ".app.bin.old-99999", "stale");
    CleanUpdateLeftovers(app_dir);
    CHECK(!std::filesystem::exists(app_dir / ".app.bin.old-99999"));

    std::filesystem::remove_all(temp_root, ec);
}

void TestTraversalRejection() {
    // Craft a malicious gzipped tar containing "../escape.txt" and verify ExtractTarGzArchive rejects it.
    const auto temp_root = std::filesystem::temp_directory_path() / "p2premote-update-traversal-test";
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root);
    const auto bad_tar = temp_root / "bad.tar.gz";

    std::array<std::uint8_t, 1536> raw{};
    std::memcpy(raw.data(), "../escape.txt", 13);
    std::memcpy(raw.data() + 100, "0000644", 7);
    std::memcpy(raw.data() + 108, "0000000", 7);
    std::memcpy(raw.data() + 116, "0000000", 7);
    std::memcpy(raw.data() + 124, "00000000000", 11);
    std::memcpy(raw.data() + 136, "00000000000", 11);
    std::memset(raw.data() + 148, ' ', 8);
    raw[156] = '0';
    std::memcpy(raw.data() + 257, "ustar", 6);
    raw[263] = '0';
    raw[264] = '0';
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += raw[i];
    char chk[8]{};
    std::snprintf(chk, sizeof(chk), "%06o", sum);
    std::memcpy(raw.data() + 148, chk, 7);
    raw[155] = ' ';

    gzFile gz = gzopen(bad_tar.string().c_str(), "wb");
    CHECK(gz != nullptr);
    CHECK(gzwrite(gz, raw.data(), static_cast<unsigned>(raw.size())) == static_cast<int>(raw.size()));
    CHECK(gzclose(gz) == Z_OK);

    std::string err;
    CHECK(!ExtractTarGzArchive(bad_tar, temp_root / "out", &err));
    CHECK(!std::filesystem::exists(temp_root / "escape.txt"));

    std::filesystem::remove_all(temp_root, ec);
}

#if !defined(_WIN32)
void TestSymlinkPreservation() {
    const auto temp_root = std::filesystem::temp_directory_path() / "p2premote-update-symlink-test";
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    const auto src = temp_root / "stage";
    const auto out_dir = temp_root / "extracted";
    const auto archive_path = temp_root / "symlinks.tar.gz";

    WriteFile(src / "lib" / "libfoo.so.1.2.3", "shared-lib-bytes");
    std::filesystem::create_symlink("libfoo.so.1.2.3", src / "lib" / "libfoo.so.1");

    std::int64_t size{};
    Sha256Digest sha{};
    std::string err;
    CHECK(WriteTarGzArchive(src / "", {"lib"}, archive_path, &size, &sha, &err));
    CHECK(ExtractTarGzArchive(archive_path, out_dir, &err));

    CHECK(std::filesystem::is_regular_file(std::filesystem::symlink_status(out_dir / "lib" / "libfoo.so.1.2.3")));
    CHECK(ReadFile(out_dir / "lib" / "libfoo.so.1.2.3") == "shared-lib-bytes");
    CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(out_dir / "lib" / "libfoo.so.1")));
    CHECK(std::filesystem::read_symlink(out_dir / "lib" / "libfoo.so.1") == "libfoo.so.1.2.3");

    std::filesystem::remove_all(temp_root, ec);
}
#endif

} // namespace

int main() {
    TestSha256();
    TestEd25519Rfc8032();
    TestVersionsAndNotes();
    TestArchiveVerifyAndSwap();
    TestTraversalRejection();
#if !defined(_WIN32)
    TestSymlinkPreservation();
#endif
    std::cout << "update_core_test: all assertions passed\n";
    return 0;
}
