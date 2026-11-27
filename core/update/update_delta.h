#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace p2premote::update {

// Reconstruct the full update archive. Verify the patch before applying it and
// verify the resulting full archive before passing it to an installer.
bool ApplyBsdiffPatch(const std::filesystem::path& base_archive,
                      const std::filesystem::path& patch,
                      const std::filesystem::path& result,
                      std::int64_t expected_size,
                      std::string* error = nullptr);

} // namespace p2premote::update
