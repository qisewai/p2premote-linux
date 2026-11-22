#include <p2premote/connection_options.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <fstream>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace p2premote {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Foundation::Uri;

std::string ReadConfig(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("无法打开公网连接配置");
    const auto size = file.tellg();
    if (size <= 0 || size > 65536) throw std::runtime_error("公网连接配置大小无效");
    std::string text(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(text.data(), static_cast<std::streamsize>(size))) throw std::runtime_error("无法读取公网连接配置");
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return text;
}

bool ValidToken(std::wstring_view token) {
    constexpr std::wstring_view alphabet = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    return token.size() == 43 && token.find_first_not_of(alphabet) == std::wstring_view::npos;
}

} // namespace

ConnectionOptions ReadInternetConfig(const std::filesystem::path& path) {
    return ParseInternetConfig(winrt::to_hstring(ReadConfig(path)), false);
}

ConnectionOptions ParseInternetConfig(std::wstring_view text, bool managed) {
    try {
        const auto json = JsonObject::Parse(winrt::hstring(text));
        ConnectionOptions result;
        result.mode = ConnectionMode::internet;
        result.managed = managed;
        const Uri endpoint(json.GetNamedString(L"signalUrl"));
        const bool local = managed && endpoint.SchemeName() == L"ws" && endpoint.Host() == L"127.0.0.1";
        if ((!local && endpoint.SchemeName() != L"wss") || endpoint.Host().empty() || endpoint.Path() != L"/signal"
            || !endpoint.UserName().empty() || !endpoint.Password().empty() || !endpoint.Query().empty()
            || !endpoint.Fragment().empty()) {
            throw std::invalid_argument("公网信令需要 wss://主机/signal 地址");
        }
        result.signal_url = endpoint.AbsoluteUri().c_str();
        result.access_token = json.GetNamedString(L"accessToken").c_str();
        if (!ValidToken(result.access_token)) throw std::invalid_argument("公网访问密钥格式无效");
        const auto policy = json.GetNamedString(L"iceTransportPolicy", L"all");
        if (policy != L"all" && policy != L"relay") throw std::invalid_argument("ICE 策略只能是 all 或 relay");
        result.relay_only = policy == L"relay";
        bool has_turn{};
        for (const auto& value : json.GetNamedArray(L"iceServers")) {
            const auto server = value.GetObject();
            IceServerOptions ice;
            ice.username = winrt::to_string(server.GetNamedString(L"username", L""));
            ice.credential = winrt::to_string(server.GetNamedString(L"credential", L""));
            const auto add_url = [&](const auto& address) {
                auto url = winrt::to_string(address);
                const auto scheme_end = url.find(':');
                if (scheme_end == std::string::npos || scheme_end + 1 == url.size()
                    || url.find_first_of(" \r\n\t") != std::string::npos) {
                    throw std::invalid_argument("ICE 服务地址格式无效");
                }
                const auto scheme = std::string_view(url).substr(0, scheme_end);
                if (scheme == "turn" || scheme == "turns") {
                    if (ice.username.empty() || ice.credential.empty()) throw std::invalid_argument("TURN 需要用户名与凭据");
                    has_turn = true;
                } else if (scheme != "stun") {
                    throw std::invalid_argument("ICE 服务地址需要使用 stun:、turn: 或 turns:");
                }
                ice.urls.push_back(std::move(url));
            };
            const auto urls = server.GetNamedValue(L"urls");
            if (urls.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                add_url(urls.GetString());
            } else {
                for (const auto& url : urls.GetArray()) add_url(url.GetString());
            }
            if (ice.urls.empty()) throw std::invalid_argument("ICE 服务地址为空");
            result.ice_servers.push_back(std::move(ice));
        }
        if (result.ice_servers.empty()) throw std::invalid_argument("请配置 STUN 或 TURN");
        if (result.relay_only && !has_turn) throw std::invalid_argument("relay 策略需要 TURN 服务");
        return result;
    } catch (const winrt::hresult_error&) {
        throw std::invalid_argument("公网连接配置格式无效，请检查 signalUrl、accessToken 和 iceServers");
    }
}

} // namespace p2premote
