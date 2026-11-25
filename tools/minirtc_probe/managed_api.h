#pragma once

#include "minirtc.h"

namespace p2premote::native_minirtc {

using SendSignal = void (*)(const char*, size_t, void*) noexcept;

struct Api {
    unsigned version;
    decltype(&CreatePeer) create;
    decltype(&DestroyPeer) destroy;
    decltype(&AddVideoStream) add_video;
    decltype(&AddDataStream) add_data;
    int (*init)(PeerPtr*, SendSignal, void*);
    int (*receive_signal)(PeerPtr*, const char*, size_t);
    decltype(&JoinConnection) join;
    decltype(&SendVideoFrame) send_video;
    decltype(&SendDataFrame) send_data;
    decltype(&SendReliableDataFrame) send_reliable;
    decltype(&UpdateVideoBitrateLimit) set_bitrate;
};

inline constexpr unsigned abi_version = 5;
using GetApi = const Api* (*)(unsigned);

} // namespace p2premote::native_minirtc

#ifndef _WIN32
extern "C" const p2premote::native_minirtc::Api* P2PRemoteMiniRtcApi(unsigned version);
#endif
