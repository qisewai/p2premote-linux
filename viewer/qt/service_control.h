#pragma once

#include <p2premote/core_api.h>

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class QObject;

struct HostServiceInfo {
    bool installed{};
    bool running{};
    bool pipe_connected{};
    bool cloud_enabled{};
    int state{};
    bool desktop_control{};
    bool lan_enabled{};
    std::uint16_t lan_port{P2P_LAN_DEFAULT_PORT};
    int lan_state{};
    bool lan_desktop_control{};
    int monitor_index{};
    QString user;
    QString device_id;
    QString status;
    QString lan_status;
#if defined(Q_OS_LINUX)
    bool enabled{};
    bool cloud_installed{};
    bool cloud_online{};
    bool operator==(const HostServiceInfo&) const = default;
#endif
};

struct HostServiceConfig {
    QString server;
    QString user;
    std::wstring password;
    QString installation_id;
    QString device_id;
    bool compatible_http{};
    bool relay_only{};
    bool lan_enabled{};
    std::uint16_t lan_port{P2P_LAN_DEFAULT_PORT};
    std::wstring lan_access_code;
    int monitor_index{};
};

bool isProcessElevated();
bool serviceExecutableExists();
HostServiceInfo queryHostServiceInfo();
HostServiceInfo queryHostWorkerStatus(HostServiceInfo info);

bool installHostService(const HostServiceConfig& config, QString* error = nullptr);
bool uninstallHostService(QString* error = nullptr);
bool relaunchClientElevated(QString* error = nullptr);

#if defined(Q_OS_LINUX)
std::optional<HostServiceInfo> queryLinuxHostServiceInfo();
void manageLinuxHostService(QObject* owner, bool enable, std::uint16_t port, const QString& code,
    std::function<void(QString)> completed);
void manageLinuxCloudService(QObject* owner, const std::optional<HostServiceConfig>& config,
    std::function<void(QString)> completed);
#endif
