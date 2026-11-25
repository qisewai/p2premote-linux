#include "snapshot.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace p2premote::agent {
namespace {

using namespace winrt::Windows::Data::Json;

constexpr std::size_t snapshot_limit = 4096;

bool IsUnsignedInteger(const IJsonValue& value) {
    if (!value || value.ValueType() != JsonValueType::Number) return false;
    const auto number = value.GetNumber();
    return number >= 0 && number <= 9007199254740991.0 && std::trunc(number) == number;
}

} // namespace

JsonObject LoadDiagnosticSnapshot(const std::filesystem::path& path) {
    const auto root = path.root_name().native();
    if (!path.is_absolute() || root.size() != 2 || root[1] != L':' ||
        path.native().find(L':', 2) != std::wstring::npos ||
        _wcsicmp(path.extension().c_str(), L".json") != 0 ||
        GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED) {
        throw std::runtime_error("Use an absolute local .json snapshot path on a fixed drive.");
    }
    winrt::handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow > snapshot_limit) {
        throw std::runtime_error("Could not open a regular diagnostics snapshot (maximum 4 KiB).");
    }
    std::array<char, snapshot_limit> data{};
    DWORD size{};
    if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &size, nullptr) ||
        size != info.nFileSizeLow) {
        throw std::runtime_error("Could not read the diagnostics snapshot.");
    }
    return ParseDiagnosticSnapshot(std::string_view(data.data(), size));
}

JsonObject ParseDiagnosticSnapshot(std::string_view bytes) {
    JsonObject source{nullptr};
    if (bytes.empty() || bytes.size() > snapshot_limit ||
        !JsonObject::TryParse(winrt::to_hstring(bytes), source) ||
        source.GetNamedNumber(L"schema", 0) != 1) {
        throw std::runtime_error("Not a P2P Remote diagnostics snapshot.");
    }
    const auto arch = source.GetNamedString(L"architecture", L"");
    if (arch != L"x64" && arch != L"arm64" && arch != L"x86" && arch != L"unknown") {
        throw std::runtime_error("Invalid architecture in diagnostics snapshot.");
    }
    JsonObject snapshot;
    snapshot.SetNamedValue(L"schema", JsonValue::CreateNumberValue(1));
    snapshot.SetNamedValue(L"architecture", JsonValue::CreateStringValue(arch));
    for (const auto* key : {L"windows_major", L"windows_minor", L"windows_build", L"logical_processors",
            L"memory_total_mib", L"memory_available_mib", L"uptime_seconds"}) {
        const auto value = source.GetNamedValue(key, nullptr);
        if (!IsUnsignedInteger(value)) {
            throw std::runtime_error("Invalid measurement in diagnostics snapshot.");
        }
        snapshot.SetNamedValue(key, value);
    }
    if (!snapshot.GetNamedNumber(L"windows_major") || !snapshot.GetNamedNumber(L"logical_processors") ||
        snapshot.GetNamedNumber(L"memory_available_mib") > snapshot.GetNamedNumber(L"memory_total_mib")) {
        throw std::runtime_error("Inconsistent measurements in diagnostics snapshot.");
    }
    const auto disk_total = source.GetNamedValue(L"system_disk_total_mib", nullptr);
    const auto disk_free = source.GetNamedValue(L"system_disk_free_mib", nullptr);
    if (IsUnsignedInteger(disk_total) && IsUnsignedInteger(disk_free) && disk_free.GetNumber() <= disk_total.GetNumber()) {
        snapshot.SetNamedValue(L"system_disk_total_mib", disk_total);
        snapshot.SetNamedValue(L"system_disk_free_mib", disk_free);
    }
    for (const auto* key : {L"host_name", L"host_user"}) {
        const auto value = source.GetNamedValue(key, nullptr);
        if (!value || value.ValueType() != JsonValueType::String) continue;
        const auto name = value.GetString();
        if (!name.empty() && winrt::to_string(name).size() <= 768 && std::ranges::none_of(name, [](wchar_t ch) {
            return ch < L' ' || ch == 127;
        })) snapshot.SetNamedValue(key, value);
    }
    const auto elevated = source.GetNamedValue(L"host_elevated", nullptr);
    if (elevated && elevated.ValueType() == JsonValueType::Boolean) snapshot.SetNamedValue(L"host_elevated", elevated);
    return snapshot;
}

} // namespace p2premote::agent
