#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace p2premote::linux_host {

struct HostConfig;

bool LoadServiceConfig(HostConfig& config);
int ManageHostService(bool enable);
bool LoadCloudServiceConfig(std::optional<std::string>& config);
int ManageCloudHostService(bool enable, const std::function<bool(std::string_view)>& validate);
int InstallHostServices(const std::function<bool(std::string_view)>& validate_cloud);
int UninstallHostServices();
int ApplyLinuxSystemUpdate(std::string_view archive_path, std::string_view manifest_path);
int ApplyDownloadedLinuxUpdate(std::string_view directory);
int SetUnattended(bool enabled);
bool IsUnattendedModeEnabled() noexcept;
bool HostReceptionAllowed() noexcept;
void NotifyHostState(std::string_view state, std::uint16_t port, bool ready = false) noexcept;
void NotifyCloudState(std::string_view state, std::string_view device, bool ready = false) noexcept;
void NotifyServiceReady() noexcept;

} // namespace p2premote::linux_host
