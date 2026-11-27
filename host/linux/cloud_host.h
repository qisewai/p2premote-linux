#pragma once

#include <string_view>

namespace p2premote::linux_host {

struct HostConfig;

bool ValidCloudHostConfig(std::string_view data);
int RunSystemService(int argc, char** argv, HostConfig lan_base);

} // namespace p2premote::linux_host
