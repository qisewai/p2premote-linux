#include "update_delta.h"
#include "update_archive.h"

#include <bzlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>

namespace p2premote::update {
namespace {

constexpr std::int64_t kHeaderSize = 32;
constexpr auto kBufferSize = 64 * 1024;
constexpr auto kMaxOffset = static_cast<std::int64_t>(kMaxArchiveBytes) * 2;

std::int64_t ReadOffset(const char* bytes) {
    std::uint64_t magnitude = static_cast<unsigned char>(bytes[7]) & 0x7f;
    for (int i = 6; i >= 0; --i) {
        magnitude = (magnitude << 8) | static_cast<unsigned char>(bytes[i]);
    }
    const auto value = static_cast<std::int64_t>(magnitude);
    return (static_cast<unsigned char>(bytes[7]) & 0x80) ? -value : value;
}

class PatchBlock {
public:
    PatchBlock(const std::filesystem::path& path, std::int64_t offset, std::int64_t size)
        : file_(path, std::ios::binary), remaining_(size) {
        ready_ = file_.seekg(offset).good() && BZ2_bzDecompressInit(&stream_, 0, 0) == BZ_OK;
    }
    ~PatchBlock() {
        if (ready_) BZ2_bzDecompressEnd(&stream_);
    }
    PatchBlock(const PatchBlock&) = delete;
    PatchBlock& operator=(const PatchBlock&) = delete;

    bool Read(char* data, unsigned length) {
        if (!ready_) return false;
        stream_.next_out = data;
        stream_.avail_out = length;
        while (stream_.avail_out > 0) {
            if (end_ || !Advance()) return false;
        }
        return true;
    }

    bool AtEnd() {
        char extra{};
        stream_.next_out = &extra;
        stream_.avail_out = 1;
        while (ready_ && !end_ && stream_.avail_out == 1) {
            if (!Advance()) return false;
        }
        return end_ && stream_.avail_out == 1 && !remaining_ && !stream_.avail_in;
    }

private:
    bool Advance() {
        if (!stream_.avail_in && remaining_) {
            const auto n = static_cast<unsigned>(std::min<std::int64_t>(remaining_, input_.size()));
            if (!file_.read(input_.data(), n)) return false;
            remaining_ -= n;
            stream_.next_in = input_.data();
            stream_.avail_in = n;
        }
        const auto before_in = stream_.avail_in;
        const auto before_out = stream_.avail_out;
        const auto status = BZ2_bzDecompress(&stream_);
        end_ = status == BZ_STREAM_END;
        return end_ || (status == BZ_OK && (before_in != stream_.avail_in || before_out != stream_.avail_out));
    }

    std::ifstream file_;
    std::int64_t remaining_{};
    std::array<char, kBufferSize> input_{};
    bz_stream stream_{};
    bool ready_{};
    bool end_{};
};

} // namespace

bool ApplyBsdiffPatch(const std::filesystem::path& base_archive,
                      const std::filesystem::path& patch,
                      const std::filesystem::path& result,
                      std::int64_t expected_size,
                      std::string* error) {
    const auto fail = [&](std::string_view message) {
        if (error) error->assign(message);
        return false;
    };
    std::error_code ec;
    const auto base_size = std::filesystem::file_size(base_archive, ec);
    if (ec || base_size > kMaxArchiveBytes) return fail("invalid delta base archive");
    const auto patch_size = std::filesystem::file_size(patch, ec);
    if (ec || patch_size < kHeaderSize || patch_size > kMaxArchiveBytes ||
        expected_size <= 0 || static_cast<std::uint64_t>(expected_size) > kMaxArchiveBytes) {
        return fail("invalid update delta size");
    }

    std::ifstream header_file(patch, std::ios::binary);
    std::array<char, kHeaderSize> header{};
    if (!header_file.read(header.data(), header.size()) || std::string_view(header.data(), 8) != "BSDIFF40") {
        return fail("invalid BSDIFF40 header");
    }
    const auto file_size = static_cast<std::int64_t>(patch_size);
    const auto control_size = ReadOffset(header.data() + 8);
    const auto diff_size = ReadOffset(header.data() + 16);
    if (control_size < 0 || control_size > file_size - kHeaderSize ||
        diff_size < 0 || diff_size > file_size - kHeaderSize - control_size ||
        ReadOffset(header.data() + 24) != expected_size) {
        return fail("invalid BSDIFF40 block sizes");
    }

    PatchBlock control(patch, kHeaderSize, control_size);
    PatchBlock diff(patch, kHeaderSize + control_size, diff_size);
    PatchBlock extra(patch, kHeaderSize + control_size + diff_size, file_size - kHeaderSize - control_size - diff_size);
    std::ifstream old(base_archive, std::ios::binary);
    std::ofstream out(result, std::ios::binary | std::ios::trunc);
    if (!old || !out) return fail("could not open delta input or output");

    std::array<char, kBufferSize> data{}, base{};
    std::int64_t old_pos = 0, written = 0;
    while (written < expected_size) {
        std::array<char, 24> tuple{};
        if (!control.Read(tuple.data(), tuple.size())) return fail("incomplete BSDIFF40 control block");
        auto diff_remaining = ReadOffset(tuple.data());
        auto extra_remaining = ReadOffset(tuple.data() + 8);
        const auto seek = ReadOffset(tuple.data() + 16);
        if (diff_remaining < 0 || diff_remaining > expected_size - written ||
            extra_remaining < 0 || extra_remaining > expected_size - written - diff_remaining ||
            seek < -kMaxOffset || seek > kMaxOffset ||
            (!diff_remaining && !extra_remaining && !seek)) {
            return fail("invalid BSDIFF40 control entry");
        }

        while (diff_remaining) {
            const auto length = static_cast<unsigned>(std::min<std::int64_t>(data.size(), diff_remaining));
            if (!diff.Read(data.data(), length)) return fail("incomplete BSDIFF40 diff block");
            const auto first = std::max<std::int64_t>(0, old_pos);
            const auto end = std::min(static_cast<std::int64_t>(base_size), old_pos + length);
            if (first < end) {
                const auto overlap = end - first;
                if (!old.seekg(first).read(base.data(), overlap)) return fail("could not read delta base archive");
                for (std::int64_t i = 0; i < overlap; ++i) {
                    auto& byte = data[static_cast<std::size_t>(first - old_pos + i)];
                    byte = static_cast<char>(static_cast<unsigned char>(byte) + static_cast<unsigned char>(base[i]));
                }
            }
            if (!out.write(data.data(), length)) return fail("could not write reconstructed archive");
            old_pos += length;
            written += length;
            diff_remaining -= length;
        }
        while (extra_remaining) {
            const auto length = static_cast<unsigned>(std::min<std::int64_t>(data.size(), extra_remaining));
            if (!extra.Read(data.data(), length)) return fail("incomplete BSDIFF40 extra block");
            if (!out.write(data.data(), length)) return fail("could not write reconstructed archive");
            written += length;
            extra_remaining -= length;
        }
        old_pos += seek;
        if (old_pos < -kMaxOffset || old_pos > kMaxOffset) return fail("BSDIFF40 base offset out of range");
    }
    out.close();
    return (out.good() && control.AtEnd() && diff.AtEnd() && extra.AtEnd()) ||
        fail("invalid end of BSDIFF40 patch");
}

} // namespace p2premote::update
