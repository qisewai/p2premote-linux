#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace p2premote {

class PluginHost {
public:
    using SendFrame = std::function<bool(std::string_view)>;

    PluginHost(bool host, SendFrame send, std::filesystem::path root = {});
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // One-shot session lifecycle: transitions false -> true -> false; create a new instance on reconnect.
    void SetConnected(bool connected) noexcept;
    void Receive(std::string_view frame) noexcept;
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
