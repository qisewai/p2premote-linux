#pragma once

#include <p2premote/connection_options.h>
#include <windows.h>
#include <stop_token>

struct StreamLaunchOptions {
    p2premote::ConnectionOptions connection;
    bool connect_on_open{};
    std::stop_token lifetime;
};

int RunStreamWindow(HINSTANCE instance, int show, bool host, StreamLaunchOptions options);
