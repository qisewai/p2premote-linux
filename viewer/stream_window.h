#pragma once

#include <p2premote/connection_options.h>
#include <windows.h>

struct StreamLaunchOptions {
    bool share{};
    p2premote::ConnectionOptions connection;
};

StreamLaunchOptions ReadStreamOptions(bool host);
int RunStreamWindow(HINSTANCE instance, int show, bool host, StreamLaunchOptions options);
