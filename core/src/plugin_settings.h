#pragma once

#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <string_view>

namespace p2premote {

class PluginSettings {
public:
    explicit PluginSettings(std::filesystem::path file);
    winrt::Windows::Data::Json::IJsonValue Get(std::wstring_view key) const;
    bool Set(std::wstring_view key, const winrt::Windows::Data::Json::IJsonValue& value) const;

private:
    winrt::Windows::Data::Json::JsonObject Read(bool reset_corrupt = false) const;

    std::filesystem::path file_;
    winrt::Windows::Data::Json::IJsonObjectStatics parser_{nullptr};
    winrt::Windows::Foundation::IActivationFactory factory_{nullptr};
};

std::filesystem::path DefaultPluginSettingsDirectory();

} // namespace p2premote
