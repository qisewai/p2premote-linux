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
    bool unattended{};
    int monitor_index{};
    QString user;
    QString device_id;
    bool needs_upgrade{};
    QString foreground_lease;
    QString status;
    QString lan_status;
    bool enabled{};
    bool cloud_installed{};
    bool needs_cleanup{};
    bool cloud_online{};
    bool operator==(const HostServiceInfo&) const = default;
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
    bool unattended{};
    int monitor_index{};
};

enum class HostConfigUpdate { install, lan, cloud, monitor, unattended, upgrade };

class HostServiceAdapter {
public:
    static bool available();
    static std::optional<HostServiceInfo> query();
    static void configure(QObject* owner, HostConfigUpdate update, bool enabled, const HostServiceConfig& config,
        std::function<void(QString)> completed);

#if defined(Q_OS_WIN)
    // The Windows SCM and foreground worker have separate status and command paths.
    static HostServiceInfo queryWorker(HostServiceInfo info);
    static bool configureSync(HostConfigUpdate update, bool enabled, const HostServiceConfig& config, QString* error);
#endif
};

#if defined(Q_OS_WIN)
bool isProcessElevated();
bool relaunchClientElevated(QString* error = nullptr);
#endif
