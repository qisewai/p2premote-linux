#pragma once

#include <winrt/Windows.Data.Json.h>

#include <stop_token>
#include <string_view>

namespace p2premote::agent {

enum class LiveOperation { devices, system_info, screen_snapshot, report };

bool IsLiveToken(std::wstring_view token) noexcept;
bool IsReportTool(std::wstring_view name) noexcept;
winrt::Windows::Data::Json::JsonObject QueryLocalAgent(std::wstring_view token, LiveOperation op,
    std::wstring_view device = {}, std::stop_token stop = {}, std::wstring_view report = {});

} // namespace p2premote::agent
