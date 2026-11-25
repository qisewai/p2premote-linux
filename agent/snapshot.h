#pragma once

#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <string_view>

namespace p2premote::agent {

winrt::Windows::Data::Json::JsonObject LoadDiagnosticSnapshot(const std::filesystem::path& path);
winrt::Windows::Data::Json::JsonObject ParseDiagnosticSnapshot(std::string_view bytes);

} // namespace p2premote::agent
