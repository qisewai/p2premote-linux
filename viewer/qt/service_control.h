#pragma once

#include <QString>

#include <string>

struct HostServiceInfo {
    bool installed{};
    bool running{};
    bool pipe_connected{};
    int state{};
    bool desktop_control{};
    int monitor_index{};
    QString user;
    QString device_id;
    QString status;
};

bool isProcessElevated();
bool serviceExecutableExists();
HostServiceInfo queryHostServiceInfo();
HostServiceInfo queryHostWorkerStatus(HostServiceInfo info);

bool installHostService(const QString& server, const QString& user, const std::wstring& password,
    const QString& installation_id, const QString& device_id, int monitor_index,
    bool compatible_http, bool relay_only, QString* error = nullptr);
bool uninstallHostService(QString* error = nullptr);
bool relaunchClientElevated(QString* error = nullptr);
