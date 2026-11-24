#pragma once

#include <p2premote/connection_options.h>

#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace p2premote {

struct AccountDevice {
    std::wstring id;
    std::wstring name;
    std::wstring platform;
    std::wstring state;

    bool operator==(const AccountDevice&) const = default;
};

struct AccountLogin {
    std::wstring server;
    std::wstring device_id;
    std::wstring installation_id;
    ConnectionOptions connection;
    bool compatible_http{};
};

struct AccountDeviceList {
    std::wstring current_device_id;
    std::vector<AccountDevice> devices;
};

AccountLogin SignIn(std::stop_token stop, const std::wstring& server, const std::wstring& user,
    const std::wstring& password, bool create_account, bool compatible_http, bool relay_only,
    std::wstring_view installation_id = {}, std::wstring_view service_device_id = {});
AccountDeviceList AccountDevices(std::stop_token stop, const AccountLogin& login);
AccountDevice AddCurrentDevice(std::stop_token stop, const AccountLogin& login);
void RemoveAccountDevice(std::stop_token stop, const AccountLogin& login, const std::wstring& device_id);
void SignOut(std::stop_token stop, const AccountLogin& login);

class LoginExpired : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace p2premote
