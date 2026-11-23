#include "core_api_internal.h"

#include <p2premote/service_identity.h>

#include <array>
#include <mutex>
#include <optional>
#include <thread>

using namespace p2premote;
using namespace p2premote::abi;

namespace {
thread_local wchar_t error_text[1024]{};
std::mutex apartment_mutex;
unsigned apartments{};

struct AccountResult {
    int32_t status{P2P_OK};
    std::array<wchar_t, std::size(error_text)> error{};
    std::optional<AccountLogin> login;
    std::optional<std::vector<AccountDevice>> devices;
};
}

struct p2p_account {
    p2p_account() { winrt::check_pointer(ready.get()); }

    template<class Job>
    void Run(Job job) {
        if (worker.joinable()) throw std::logic_error("已有账号请求正在处理");
        result = {};
        worker = std::jthread([this, job = std::move(job)](std::stop_token stop) mutable {
            {
                std::optional<Apartment> apartment;
                try {
                    apartment.emplace(winrt::apartment_type::multi_threaded);
                    job(stop, result);
                } catch (...) {
                    result.status = ReportException();
                    wcsncpy_s(result.error.data(), result.error.size(), p2p_last_error(), _TRUNCATE);
                }
            }
            SetEvent(ready.get());
        });
    }

    void Cancel() {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        ResetEvent(ready.get());
        result = {};
    }

    winrt::handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::optional<AccountLogin> login;
    std::vector<AccountDevice> devices;
    AccountResult result;
    std::jthread worker;
};

Apartment::Apartment(winrt::apartment_type type) {
    winrt::init_apartment(type);
    const std::lock_guard lock(apartment_mutex);
    ++apartments;
}

Apartment::~Apartment() {
    {
        const std::lock_guard lock(apartment_mutex);
        // A shell may have no other WinRT apartment once its last worker exits.
        if (--apartments == 0) winrt::clear_factory_cache();
    }
    winrt::uninit_apartment();
}

void p2premote::abi::SetError(const wchar_t* message) noexcept {
    wcsncpy_s(error_text, message, _TRUNCATE);
}

int32_t p2premote::abi::ReportException() noexcept {
    try {
        try {
            throw;
        } catch (const LoginExpired& error) {
            SetError(winrt::to_hstring(error.what()).c_str());
            return P2P_LOGIN_EXPIRED;
        } catch (const winrt::hresult_error& error) {
            SetError(error.message().c_str());
        } catch (const std::exception& error) {
            SetError(winrt::to_hstring(error.what()).c_str());
        }
    } catch (...) {
        SetError(L"核心操作失败");
    }
    return P2P_ERROR;
}

ConnectionOptions p2premote::abi::GetConnection(const p2p_account* account) {
    const auto& login = Required(account).login;
    if (!login) throw std::logic_error("请先登录");
    return login->connection;
}

uint32_t p2p_core_abi_version() { return P2P_CORE_ABI_VERSION; }
const wchar_t* p2p_last_error() { return error_text; }
const wchar_t* p2p_default_server() { return wuhan_service_address; }

p2p_account* p2p_account_create() {
    p2p_account* account{};
    Guard([&] { account = new p2p_account; });
    return account;
}

void p2p_account_destroy(p2p_account* account) { delete account; }
void* p2p_account_event(const p2p_account* account) { return account ? account->ready.get() : nullptr; }

int32_t p2p_account_login(p2p_account* account, const wchar_t* server, const wchar_t* user,
    const wchar_t* password, int32_t create, int32_t compatible_http, int32_t relay_only) {
    return Guard([&] {
        auto& state = Required(account);
        if (state.login) throw std::logic_error("请先退出当前账号");
        if (!server || !*server || !user || !*user || !password || !*password) {
            throw std::invalid_argument("请输入服务地址、账号和密码");
        }
        state.Run([server = std::wstring(server), user = std::wstring(user), password = std::wstring(password),
                      create, compatible_http, relay_only](std::stop_token stop, AccountResult& result) mutable {
            struct ErasePassword {
                std::wstring& password;
                ~ErasePassword() { SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t)); }
            } erase{password};
            result.login = SignIn(stop, server, user, password, create != 0, compatible_http != 0, relay_only != 0);
        });
    });
}

int32_t p2p_account_refresh(p2p_account* account) {
    return Guard([&] {
        auto& state = Required(account);
        if (!state.login) throw std::logic_error("请先登录");
        state.Run([login = *state.login](std::stop_token stop, AccountResult& result) {
            result.devices = AccountDevices(stop, login);
        });
    });
}

int32_t p2p_account_logout(p2p_account* account) {
    return Guard([&] {
        auto& state = Required(account);
        if (!state.login) throw std::logic_error("请先登录");
        state.Run([login = *state.login](std::stop_token stop, AccountResult&) { SignOut(stop, login); });
        state.login.reset();
        state.devices.clear();
    });
}

void p2p_account_cancel(p2p_account* account) {
    if (account) Guard([&] { account->Cancel(); });
}

int32_t p2p_account_complete(p2p_account* account) {
    if (!account || !account->worker.joinable() || WaitForSingleObject(account->ready.get(), 0) != WAIT_OBJECT_0) {
        return P2P_PENDING;
    }
    auto status = P2P_OK;
    const auto guarded = Guard([&] {
        account->worker.join();
        ResetEvent(account->ready.get());
        auto result = std::exchange(account->result, {});
        if (result.status == P2P_LOGIN_EXPIRED) {
            account->login.reset();
            account->devices.clear();
        }
        status = static_cast<p2p_result>(result.status);
        if (status != P2P_OK) SetError(result.error.data());
        if (result.login) account->login = std::move(result.login);
        if (result.devices) account->devices = std::move(*result.devices);
    });
    return guarded == P2P_OK ? status : guarded;
}

const wchar_t* p2p_account_device_id(const p2p_account* account) {
    return account && account->login ? account->login->device_id.c_str() : nullptr;
}

size_t p2p_account_device_count(const p2p_account* account) { return account ? account->devices.size() : 0; }

int32_t p2p_account_device(const p2p_account* account, size_t index, p2p_device* device) {
    return Guard([&] {
        const auto& info = Required(account).devices.at(index);
        Required(device) = {info.id.c_str(), info.name.c_str(), info.platform.c_str(), info.state.c_str()};
    });
}
