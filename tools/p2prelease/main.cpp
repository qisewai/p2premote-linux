#include "ed25519.h"
#include "update_archive.h"
#include "update_delta.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

using namespace p2premote::update;

void PrintUsage() {
    std::cerr
        << "p2prelease - P2P Remote signed update packager (mygo-compatible)\n\n"
        << "Commands:\n"
        << "  keygen --out <dir>\n"
        << "      Generate Ed25519 key pair (p2premote-update.key and p2premote-update.pub)\n\n"
        << "  pack --dir <staged_app_dir> --target <windows-x64|linux-x64|darwin-arm64>\n"
        << "       --version <semver> --out <out_dir>\n"
        << "       [--key <private_key_file>]        (or env P2PREMOTE_UPDATE_PRIVATE_KEY)\n"
        << "       [--github <owner/repo>]           (default: qisewai/p2premote-linux)\n"
        << "       [--base-url <https://...>]        (overrides --github download URL)\n"
        << "  add-delta --manifest <new.json> --archive <new.tar.gz> --from-manifest <old.json>\n"
        << "            --from-archive <old.tar.gz> --patch <name.delta> --pubkey <key> [--key <key>]\n"
        << "       [--tag-prefix <v>]                (default: v)\n"
        << "       [--changelog <CHANGELOG.md>]\n\n"
        << "  verify --manifest <update-<target>.json> --archive <file.tar.gz>\n"
        << "         --pubkey <base64_or_pub_file>\n";
}

std::string GetEnvString(const char* name) {
#if defined(_WIN32)
    char* raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, name) != 0 || !raw) return {};
    std::string out(raw);
    std::free(raw);
    return out;
#else
    const char* raw = std::getenv(name);
    return raw ? std::string(raw) : std::string{};
#endif
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string UtcRfc3339Now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

int CmdKeygen(int argc, char** argv) {
    std::filesystem::path out_dir = ".";
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 2;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::cerr << "Failed to create output directory: " << ec.message() << "\n";
        return 1;
    }
    Ed25519PublicKey pub{};
    Ed25519PrivateKey priv{};
    if (!Ed25519GenerateKeypair(pub, priv)) {
        std::cerr << "Failed to generate random Ed25519 key pair.\n";
        return 1;
    }
    const std::string pub_b64 = Base64Encode(pub.data(), pub.size());
    const std::string priv_b64 = Base64Encode(priv.data(), priv.size());
    const auto priv_path = out_dir / "p2premote-update.key";
    const auto pub_path = out_dir / "p2premote-update.pub";
    {
        std::ofstream f(priv_path, std::ios::binary | std::ios::trunc);
        if (!f || !(f << priv_b64 << "\n")) {
            std::cerr << "Failed to write private key: " << priv_path.string() << "\n";
            return 1;
        }
    }
#if !defined(_WIN32)
    chmod(priv_path.c_str(), 0600);
#endif
    {
        std::ofstream f(pub_path, std::ios::binary | std::ios::trunc);
        if (!f || !(f << pub_b64 << "\n")) {
            std::cerr << "Failed to write public key: " << pub_path.string() << "\n";
            return 1;
        }
    }
    std::cout << "Private key: " << priv_path.string() << "\n"
              << "Public key:  " << pub_path.string() << "\n"
              << "Public key (base64): " << pub_b64 << "\n";
    return 0;
}

int CmdPack(int argc, char** argv) {
    std::filesystem::path stage_dir;
    std::filesystem::path out_dir;
    std::filesystem::path key_file;
    std::filesystem::path changelog_file;
    std::string target;
    std::string version;
    std::string github_repo = "qisewai/p2premote-linux";
    std::string base_url;
    std::string tag_prefix = "v";

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string tmp;
        if (arg == "--dir" && next(tmp)) stage_dir = tmp;
        else if (arg == "--target" && next(target)) {}
        else if (arg == "--version" && next(version)) {}
        else if (arg == "--out" && next(tmp)) out_dir = tmp;
        else if (arg == "--key" && next(tmp)) key_file = tmp;
        else if (arg == "--github" && next(github_repo)) {}
        else if (arg == "--base-url" && next(base_url)) {}
        else if (arg == "--tag-prefix" && next(tag_prefix)) {}
        else if (arg == "--changelog" && next(tmp)) changelog_file = tmp;
        else {
            std::cerr << "Invalid or unknown argument: " << arg << "\n";
            return 2;
        }
    }

    if (stage_dir.empty() || !IsReleaseVersion(version) || out_dir.empty() ||
        (target != "windows-x64" && target != "linux-x64" && target != "darwin-arm64" && target != "darwin-x64")) {
        std::cerr << "Error: --dir, --target, --version, and --out are required.\n";
        return 2;
    }

    std::string key_text;
    if (!key_file.empty()) {
        key_text = ReadTextFile(key_file);
    } else {
        key_text = GetEnvString("P2PREMOTE_UPDATE_PRIVATE_KEY");
    }
    const auto priv = ParsePrivateKeyBase64(key_text);
    if (!priv) {
        std::cerr << "Error: missing or invalid Ed25519 private key (pass --key or set P2PREMOTE_UPDATE_PRIVATE_KEY).\n";
        return 1;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(stage_dir, ec) || ec) {
        std::cerr << "Error: staged directory does not exist: " << stage_dir.string() << "\n";
        return 1;
    }
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::cerr << "Error: could not create output directory: " << ec.message() << "\n";
        return 1;
    }

    std::vector<std::string> entries;
    for (const auto& e : std::filesystem::directory_iterator(stage_dir, ec)) {
        if (ec) {
            std::cerr << "Error scanning staged directory: " << ec.message() << "\n";
            return 1;
        }
        const std::string name = e.path().filename().string();
        if (name == kManagedFileListName) continue;
        entries.push_back(name);
    }
    if (entries.empty()) {
        std::cerr << "Error: staged directory is empty.\n";
        return 1;
    }

    std::string err;
    if (!WriteFileList(stage_dir, entries, &err)) {
        std::cerr << "Error writing .p2premote-files: " << err << "\n";
        return 1;
    }
    entries.emplace_back(kManagedFileListName);

    const std::string archive_name = "p2premote-" + version + "-" + target + ".tar.gz";
    const auto archive_path = out_dir / archive_name;
    std::int64_t archive_size{};
    Sha256Digest digest{};
    if (!WriteTarGzArchive(stage_dir, entries, archive_path, &archive_size, &digest, &err)) {
        std::cerr << "Error writing archive: " << err << "\n";
        return 1;
    }

    std::string notes;
    if (!changelog_file.empty()) {
        const std::string changelog = ReadTextFile(changelog_file);
        notes = ExtractReleaseNotes(changelog, version);
    }

    UpdateManifest manifest;
    manifest.target = target;
    manifest.version = version;
    manifest.notes = notes;
    manifest.date = UtcRfc3339Now();
    if (!base_url.empty()) {
        std::string trimmed = base_url;
        while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
        manifest.url = trimmed + "/" + archive_name;
    } else {
        manifest.url = "https://github.com/" + github_repo + "/releases/download/" +
            tag_prefix + version + "/" + archive_name;
    }
    manifest.size = archive_size;
    manifest.signature = SignDigestBase64(*priv, digest);
    SignUpdateManifest(manifest, *priv);

    const std::string manifest_name = "update-" + target + ".json";
    const auto manifest_path = out_dir / manifest_name;
    {
        std::ofstream f(manifest_path, std::ios::binary | std::ios::trunc);
        if (!f || !(f << SerializeUpdateManifest(manifest))) {
            std::cerr << "Error writing manifest: " << manifest_path.string() << "\n";
            return 1;
        }
    }

    const auto sha_path = out_dir / (archive_name + ".sha256");
    {
        std::ofstream f(sha_path, std::ios::binary | std::ios::trunc);
        if (f) f << Sha256::ToHex(digest) << "  " << archive_name << "\n";
    }

    std::cout << "Archive:  " << archive_path.string() << " (" << archive_size << " bytes)\n"
              << "SHA-256:  " << Sha256::ToHex(digest) << "\n"
              << "Manifest: " << manifest_path.string() << "\n";
    return 0;
}

std::string ReadPublicKey(std::string_view value) {
    const std::filesystem::path path(value);
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec ? ReadTextFile(path) : std::string(value);
}

int CmdAddDelta(int argc, char** argv) {
    std::filesystem::path manifest_file, archive_file, from_manifest_file, from_archive_file, patch_file, key_file;
    std::string public_key_arg;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view option = argv[i];
        const char* value = argv[i + 1];
        if (option == "--manifest") manifest_file = value;
        else if (option == "--archive") archive_file = value;
        else if (option == "--from-manifest") from_manifest_file = value;
        else if (option == "--from-archive") from_archive_file = value;
        else if (option == "--patch") patch_file = value;
        else if (option == "--key") key_file = value;
        else if (option == "--pubkey") public_key_arg = value;
        else return 2;
    }
    if ((argc % 2) || manifest_file.empty() || archive_file.empty() || from_manifest_file.empty() ||
        from_archive_file.empty() || patch_file.extension() != ".delta" || public_key_arg.empty()) {
        return 2;
    }
    const auto public_key = ReadPublicKey(public_key_arg);
    const auto private_key = ParsePrivateKeyBase64(key_file.empty()
        ? GetEnvString("P2PREMOTE_UPDATE_PRIVATE_KEY") : ReadTextFile(key_file));
    auto manifest = ParseUpdateManifest(ReadTextFile(manifest_file));
    const auto base = ParseUpdateManifest(ReadTextFile(from_manifest_file));
    if (!private_key || !manifest || !base || manifest->target.empty() ||
        !VerifyUpdateManifest(*manifest, manifest->target, public_key) ||
        !VerifyUpdateManifest(*base, manifest->target, public_key) ||
        CompareVersions(base->version, manifest->version) >= 0 ||
        !VerifyArchiveFile(archive_file, manifest->size, manifest->signature, public_key) ||
        !VerifyArchiveFile(from_archive_file, base->size, base->signature, public_key)) {
        std::cerr << "Could not verify the full release archives and their manifests.\n";
        return 1;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(patch_file, ec);
    if (ec || !size || size >= static_cast<std::uint64_t>(manifest->size)) {
        std::cerr << "Delta must be smaller than the full archive.\n";
        return 1;
    }

    auto reconstructed = patch_file;
    reconstructed += ".check.tmp";
    if (std::filesystem::exists(std::filesystem::symlink_status(reconstructed, ec))) {
        std::cerr << "Delta verification output already exists.\n";
        return 1;
    }
    std::string err;
    const bool verified = ApplyBsdiffPatch(from_archive_file, patch_file, reconstructed, manifest->size, &err) &&
        VerifyArchiveFile(reconstructed, manifest->size, manifest->signature, public_key, &err);
    std::filesystem::remove(reconstructed, ec);
    if (!verified) {
        std::cerr << "Delta does not reconstruct the signed full archive: " << err << "\n";
        return 1;
    }
    const auto patch_size = static_cast<std::int64_t>(size);
    const auto digest = HashUpdateFile(patch_file, patch_size, &err);
    const auto slash = manifest->url.rfind('/');
    if (!digest || slash == std::string::npos) return 1;
    std::erase_if(manifest->deltas, [&](const auto& delta) { return delta.from == base->version; });
    std::erase_if(manifest->previous, [&](const auto& previous) { return previous.version == base->version; });
    manifest->deltas.push_back({base->version, manifest->url.substr(0, slash + 1) + patch_file.filename().string(),
                                patch_size, SignDigestBase64(*private_key, *digest)});
    manifest->previous.push_back({base->version, base->url, base->size, base->signature});
    SignUpdateManifest(*manifest, *private_key);
    if (!VerifyUpdateManifest(*manifest, manifest->target, public_key)) {
        std::cerr << "Signing key does not match the trusted public key.\n";
        return 1;
    }
    std::ofstream out(manifest_file, std::ios::binary | std::ios::trunc);
    if (!out || !(out << SerializeUpdateManifest(*manifest))) return 1;
    out.close();
    if (!out) return 1;
    std::cout << "Verified delta from " << base->version << ": " << patch_size << " / " << manifest->size << " bytes\n";
    return 0;
}

int CmdVerify(int argc, char** argv) {
    std::filesystem::path manifest_path;
    std::filesystem::path archive_path;
    std::string pubkey_arg;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--manifest" && i + 1 < argc) manifest_path = argv[++i];
        else if (arg == "--archive" && i + 1 < argc) archive_path = argv[++i];
        else if (arg == "--pubkey" && i + 1 < argc) pubkey_arg = argv[++i];
        else {
            std::cerr << "Invalid argument: " << arg << "\n";
            return 2;
        }
    }
    if (manifest_path.empty() || archive_path.empty() || pubkey_arg.empty()) {
        std::cerr << "Error: --manifest, --archive, and --pubkey are required.\n";
        return 2;
    }
    const std::string pubkey_b64 = ReadPublicKey(pubkey_arg);
    std::string err;
    const auto manifest = ParseUpdateManifest(ReadTextFile(manifest_path), &err);
    if (!manifest) {
        std::cerr << "Manifest parse failed: " << err << "\n";
        return 1;
    }
    if (manifest->target.empty() || !VerifyUpdateManifest(*manifest, manifest->target, pubkey_b64)) {
        std::cerr << "Manifest signature verification failed.\n";
        return 1;
    }
    if (!VerifyArchiveFile(archive_path, manifest->size, manifest->signature, pubkey_b64, &err)) {
        std::cerr << "Archive verification failed: " << err << "\n";
        return 1;
    }
    std::cout << "Verified OK: version=" << manifest->version << " size=" << manifest->size << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "keygen") return CmdKeygen(argc, argv);
    if (cmd == "pack") return CmdPack(argc, argv);
    if (cmd == "add-delta") return CmdAddDelta(argc, argv);
    if (cmd == "verify") return CmdVerify(argc, argv);
    if (cmd == "--help" || cmd == "-h") {
        PrintUsage();
        return 0;
    }
    PrintUsage();
    return 2;
}
