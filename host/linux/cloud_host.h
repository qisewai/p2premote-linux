#pragma once

#include <string_view>

namespace p2premote::linux_host {

bool ValidCloudHostConfig(std::string_view data);
int RunCloudHost(int argc, char** argv);

} // namespace p2premote::linux_host
