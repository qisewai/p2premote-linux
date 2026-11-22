#include "account_api.h"

#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace p2premote {
namespace {

using namespace winrt::Windows::Data::Json;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;
using namespace winrt::Windows::Web::Http;

std::wstring AccountOrigin(std::wstring_view server) {
    const Uri uri{winrt::hstring(server)};
    const bool local = uri.SchemeName() == L"http" && uri.Host() == L"127.0.0.1";
    if ((!local && uri.SchemeName() != L"https") || uri.Host().empty() || uri.Path() != L"/"
        || !uri.UserName().empty() || !uri.Password().empty() || !uri.Query().empty() || !uri.Fragment().empty()) {
        throw std::invalid_argument("请填写 HTTPS 服务根地址。本机测试可使用 http://127.0.0.1:8788");
    }
    std::wstring origin(uri.AbsoluteUri());
    origin.pop_back();
    return origin;
}

std::wstring InstallationId() {
    constexpr wchar_t key[] = L"Software\\P2PRemote\\Account";
    constexpr wchar_t value[] = L"InstallationId";
    const winrt::handle lock{CreateMutexW(nullptr, FALSE, L"Local\\P2PRemote.Account.InstallationId")};
    winrt::check_pointer(lock.get());
    const auto waited = WaitForSingleObject(lock.get(), 3'000);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        throw std::runtime_error("无法读取本机设备标识");
    }
    struct Unlock {
        HANDLE handle;
        ~Unlock() { ReleaseMutex(handle); }
    } unlock{lock.get()};
    std::array<wchar_t, 44> saved{};
    DWORD size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, saved.data(), &size) == ERROR_SUCCESS
        && size == sizeof(saved)) {
        return std::wstring(saved.data(), 43);
    }
    std::wstring id(CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::GenerateRandom(32)));
    std::replace(id.begin(), id.end(), L'+', L'-');
    std::replace(id.begin(), id.end(), L'/', L'_');
    id.resize(43);
    const auto result = RegSetKeyValueW(HKEY_CURRENT_USER, key, value, REG_SZ, id.c_str(),
        static_cast<DWORD>((id.size() + 1) * sizeof(wchar_t)));
    winrt::check_hresult(HRESULT_FROM_WIN32(result));
    return id;
}

winrt::hstring ComputerName() {
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> name{};
    DWORD length = static_cast<DWORD>(name.size());
    winrt::check_bool(GetComputerNameW(name.data(), &length));
    return winrt::hstring(name.data(), length);
}

template <typename Operation>
auto AwaitAccount(const Operation& operation, std::stop_token stop) {
    const std::stop_callback cancel(stop, [&] {
        try { operation.Cancel(); } catch (...) {}
    });
    if (operation.wait_for(std::chrono::seconds(8)) == AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("账号服务请求超时");
    }
    return operation.GetResults();
}

struct AccountResponse {
    long status;
    winrt::hstring body;
};

AccountResponse WindowsAccountRequest(std::stop_token stop, const std::wstring& endpoint, const HttpMethod& method,
    const std::wstring& token, const JsonObject& body) {
    const Filters::HttpBaseProtocolFilter filter;
    filter.AllowAutoRedirect(false);
    const HttpClient client(filter);
    const HttpRequestMessage request(method, Uri(endpoint));
    if (!token.empty()) request.Headers().Append(L"Authorization", L"Bearer " + token);
    if (body) {
        request.Content(HttpStringContent(body.Stringify(),
            winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8, L"application/json"));
    }
    const auto response = AwaitAccount(client.SendRequestAsync(request, HttpCompletionOption::ResponseContentRead), stop);
    return {static_cast<long>(response.StatusCode()), response.StatusCode() == HttpStatusCode::NoContent
        ? winrt::hstring{} : AwaitAccount(response.Content().ReadAsStringAsync(), stop)};
}

void CheckCurl(CURLcode code) {
    if (code == CURLE_OPERATION_TIMEDOUT) throw std::runtime_error("账号服务请求超时（兼容登录）");
    if (code != CURLE_OK) throw std::runtime_error(std::string("账号服务连接失败（兼容登录） · ") + curl_easy_strerror(code));
}

AccountResponse CurlAccountRequest(std::stop_token stop, const std::wstring& endpoint, const HttpMethod& method,
    const std::wstring& token, const JsonObject& body) {
    struct CurlRuntime {
        CurlRuntime() { CheckCurl(curl_global_init(CURL_GLOBAL_DEFAULT)); }
        ~CurlRuntime() { curl_global_cleanup(); }
    };
    static const CurlRuntime runtime;
    if (stop.stop_requested()) throw std::runtime_error("账号请求已取消");

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    const auto add_header = [&headers](const std::string& value) {
        auto* next = curl_slist_append(headers.get(), value.c_str());
        if (!next) throw std::bad_alloc();
        headers.release();
        headers.reset(next);
    };
    if (!token.empty()) add_header("Authorization: Bearer " + winrt::to_string(token));
    if (body) add_header("Content-Type: application/json; charset=utf-8");

    const auto url = winrt::to_string(endpoint);
    const auto payload = body ? winrt::to_string(body.Stringify()) : std::string{};
    std::string response;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
    if (!request) throw std::runtime_error("无法初始化兼容登录连接");
    const auto curl = request.get();
    const auto secure = endpoint.starts_with(L"https:");
    CheckCurl(curl_easy_setopt(curl, CURLOPT_URL, url.c_str()));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, secure ? "https" : "http"));
    if (!secure) CheckCurl(curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1"));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2)));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1)));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8'000L));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 12'000L));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get()));
    if (method.Method() == L"POST") {
        CheckCurl(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str()));
        CheckCurl(curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size())));
    }
    CheckCurl(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* bytes, std::size_t size, std::size_t count, void* context) noexcept -> std::size_t {
            constexpr std::size_t max_response = 4 * 1024 * 1024;
            auto& text = *static_cast<std::string*>(context);
            if (size && count > (max_response - text.size()) / size) return 0;
            const auto length = size * count;
            try { text.append(bytes, length); } catch (...) { return 0; }
            return length;
        }));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop));
    CheckCurl(curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept -> int {
            return static_cast<const std::stop_token*>(context)->stop_requested() ? 1 : 0;
        }));
    CheckCurl(curl_easy_perform(curl));
    long status{};
    CheckCurl(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status));
    return {status, winrt::to_hstring(response)};
}

winrt::hstring AccountRequest(std::stop_token stop, bool compatible_http, const std::wstring& endpoint,
    const HttpMethod& method, const std::wstring& token = {}, const JsonObject& body = nullptr) {
    const auto [status, text] = compatible_http
        ? CurlAccountRequest(stop, endpoint, method, token, body) : WindowsAccountRequest(stop, endpoint, method, token, body);
    if (!token.empty() && status == static_cast<long>(HttpStatusCode::Unauthorized)) {
        throw LoginExpired("登录已失效，请重新登录");
    }
    if (status < 200 || status >= 300) {
        JsonObject error{nullptr};
        if (JsonObject::TryParse(text, error)) {
            throw std::runtime_error(winrt::to_string(error.GetNamedString(L"error", L"账号服务请求失败")));
        }
        throw std::runtime_error("账号服务请求失败，HTTP " + std::to_string(status));
    }
    return text;
}

} // namespace

AccountLogin SignIn(std::stop_token stop, const std::wstring& server, const std::wstring& user,
    const std::wstring& password, bool create_account, bool compatible_http) {
    const auto origin = AccountOrigin(server);
    const JsonObject body;
    body.SetNamedValue(L"username", JsonValue::CreateStringValue(user));
    body.SetNamedValue(L"password", JsonValue::CreateStringValue(password));
    body.SetNamedValue(L"installationId", JsonValue::CreateStringValue(InstallationId()));
    body.SetNamedValue(L"deviceName", JsonValue::CreateStringValue(ComputerName()));
    body.SetNamedValue(L"platform", JsonValue::CreateStringValue(L"Windows x64"));
    auto auth = JsonObject::Parse(AccountRequest(stop, compatible_http,
        origin + (create_account ? L"/api/v1/register" : L"/api/v1/login"), HttpMethod::Post(), {}, body));
    auto signal = origin;
    const bool secure = signal.starts_with(L"https:");
    signal.replace(0, secure ? 5 : 4, secure ? L"wss" : L"ws");
    auth.SetNamedValue(L"signalUrl", JsonValue::CreateStringValue(signal + L"/signal"));
    auto connection = ParseInternetConfig(auth.Stringify(), true);
    connection.hardware_video = true;
    return {origin, std::wstring(auth.GetNamedObject(L"device").GetNamedString(L"id")), std::move(connection), compatible_http};
}

std::vector<AccountDevice> AccountDevices(std::stop_token stop, const AccountLogin& login) {
    const auto data = JsonArray::Parse(AccountRequest(stop, login.compatible_http, login.server + L"/api/v1/devices",
        HttpMethod::Get(), login.connection.access_token));
    std::vector<AccountDevice> devices;
    for (const auto& entry : data) {
        const auto item = entry.GetObject();
        devices.push_back({std::wstring(item.GetNamedString(L"id")), std::wstring(item.GetNamedString(L"name")),
            std::wstring(item.GetNamedString(L"platform")), std::wstring(item.GetNamedString(L"state"))});
    }
    std::sort(devices.begin(), devices.end(), [&login](const AccountDevice& a, const AccountDevice& b) {
        if ((a.id == login.device_id) != (b.id == login.device_id)) return a.id == login.device_id;
        if (a.name == b.name) return a.id < b.id;
        return a.name < b.name;
    });
    return devices;
}

void SignOut(std::stop_token stop, const AccountLogin& login) {
    AccountRequest(stop, login.compatible_http, login.server + L"/api/v1/logout", HttpMethod::Post(), login.connection.access_token);
}

} // namespace p2premote
