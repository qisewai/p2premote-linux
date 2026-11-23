#include "plugin_settings.h"

#include <windows.h>
#include <shlobj.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <fstream>
#include <memory>
#include <mutex>
#include <utility>

namespace p2premote {
namespace {

using namespace winrt::Windows::Data::Json;
constexpr std::size_t settings_limit = 16 * 1024;
std::mutex settings_mutex;

} // namespace

std::filesystem::path DefaultPluginSettingsDirectory() {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) return {};
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> local(path, CoTaskMemFree);
    return std::filesystem::path(local.get()) / L"P2PRemote" / L"plugin-config";
}

PluginSettings::PluginSettings(std::filesystem::path file)
    : file_(std::move(file)), parser_(winrt::try_get_activation_factory<JsonObject, IJsonObjectStatics>()),
      factory_(winrt::try_get_activation_factory<JsonObject>()) {}

JsonObject PluginSettings::Read(bool reset_corrupt) const {
    const auto attributes = GetFileAttributesW(file_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            return factory_.ActivateInstance<JsonObject>();
        }
        return nullptr;
    }
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    std::ifstream file(file_, std::ios::binary);
    std::string contents(settings_limit + 1, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file.eof() || file.bad()) return nullptr;
    contents.resize(static_cast<std::size_t>(file.gcount()));
    JsonObject result{nullptr};
    if (!parser_.TryParse(winrt::to_hstring(contents), result) && reset_corrupt) {
        return factory_.ActivateInstance<JsonObject>();
    }
    return result;
}

IJsonValue PluginSettings::Get(std::wstring_view key) const {
    const std::lock_guard lock(settings_mutex);
    if (file_.empty() || !parser_ || !factory_) return nullptr;
    const auto json = Read();
    return json && json.HasKey(key) ? json.GetNamedValue(key) : nullptr;
}

bool PluginSettings::Set(std::wstring_view key, const IJsonValue& value) const {
    const std::lock_guard lock(settings_mutex);
    if (file_.empty() || !parser_ || !factory_) return false;
    const auto json = Read(true);
    if (!json) return false;
    if (value) json.SetNamedValue(key, value);
    else if (json.HasKey(key)) json.Remove(key);
    const auto data = winrt::to_string(json.Stringify());
    if (data.size() > settings_limit) return false;

    const auto directory = file_.parent_path();
    std::filesystem::create_directories(directory);
    const auto attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(directory.c_str(), L"p2p", 0, temporary)) return false;
    struct RemoveTemp { const wchar_t* path; ~RemoveTemp() { DeleteFileW(path); } } cleanup{temporary};
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    return file && MoveFileExW(temporary, file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace p2premote
