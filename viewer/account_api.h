#pragma once

#include <p2premote/connection_options.h>

#include <stop_token>
#include <stdexcept>
#include <string>
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
    ConnectionOptions connection;
    bool compatible_http{};
};

AccountLogin SignIn(std::stop_token stop, const std::wstring& server, const std::wstring& user,
    const std::wstring& password, bool create_account, bool compatible_http);
std::vector<AccountDevice> AccountDevices(std::stop_token stop, const AccountLogin& login);
void SignOut(std::stop_token stop, const AccountLogin& login);

class LoginExpired : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace p2premote
