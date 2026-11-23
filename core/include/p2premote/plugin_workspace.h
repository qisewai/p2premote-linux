#pragma once

#include <p2premote/connection_options.h>
#include <p2premote/input_protocol.h>

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
};

struct PluginAction {
    enum class Kind { notice, input, bitrate } kind{};
    std::wstring notice;
    InputCommand input;
    VideoBitratePreset bitrate{VideoBitratePreset::automatic};
};

} // namespace p2premote
