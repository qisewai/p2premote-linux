#pragma once

#include <p2premote/plugin_workspace.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace p2premote {

inline constexpr std::string_view workspace_protocol_id = "_workspace";

class PluginHost {
public:
    using SendFrame = std::function<bool(std::string_view)>;
    using Notify = std::function<void()>;

    PluginHost(bool host, SendFrame send, std::filesystem::path root = {}, Notify notify = {},
        std::filesystem::path config_root = {});
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // One-shot session lifecycle: transitions false -> true -> false; create a new instance on reconnect.
    void SetConnected(bool connected) noexcept;
    void Receive(std::string_view frame) noexcept;
    void UpdateMetrics(PluginMetrics metrics);
    void UpdateLockState(bool caps, bool num);
    bool CapturesLockKeys() const noexcept;
    bool HasMenu() const;
    std::vector<PluginMenuItem> Menu() const;
    void Invoke(unsigned id);
    std::vector<PluginAction> TakeActions();
    std::vector<PluginUiCommand> TakeUiCommands();
    void SendUiEvent(PluginUiEvent event);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
