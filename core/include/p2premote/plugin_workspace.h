#pragma once

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>
#include <p2premote/overlay.h>

#include <optional>
#include <string>

namespace p2premote {

struct PluginMenuItem {
    unsigned id{};
    std::wstring label;
    std::optional<bool> checked;
};

struct PluginMetrics {
    IcePath route{};
    std::optional<double> rtt_ms;
    std::optional<double> loss;
    std::optional<std::uint32_t> download_bps;
    std::optional<std::uint32_t> upload_bps;
    std::optional<double> frame_wait_ms;
    std::string protocol;
    unsigned width{}, height{};
    double seconds{};
    std::uint64_t presented{};
};

struct PluginAction {
    enum class Kind { notice, input, bitrate } kind{};
    std::wstring notice;
    InputCommand input;
    VideoBitratePreset bitrate{VideoBitratePreset::automatic};
};

struct PluginUiCommand {
    std::uint32_t dialog_id{};
    std::string kind;
    std::string json;
};

struct PluginUiEvent {
    std::uint32_t dialog_id{};
    std::string control;
    std::string event;
    std::string json;
};

} // namespace p2premote
