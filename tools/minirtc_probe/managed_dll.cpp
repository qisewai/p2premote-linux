#include "managed_api.h"

#include "minirtc_inner.h"

namespace {

using p2premote::native_minirtc::SendSignal;

int InitExternal(PeerPtr* peer, SendSignal send, void* context) {
    if (!peer || !send) return -1;
    return peer->peer_connection->Init(peer->pc_params, [=](const std::string& message) {
        send(message.data(), message.size(), context);
    });
}

int ReceiveExternal(PeerPtr* peer, const char* data, size_t size) {
    if (!peer || !data || !size || size > 64 * 1024) return -1;
    return peer->peer_connection->ReceiveExternalSignal(std::string(data, size));
}

} // namespace

extern "C" __declspec(dllexport) const p2premote::native_minirtc::Api* P2PRemoteMiniRtcApi(unsigned version) {
    using namespace p2premote::native_minirtc;
    static const Api api{abi_version, &CreatePeer, &DestroyPeer, &AddVideoStream, &AddDataStream,
        &InitExternal, &ReceiveExternal, &JoinConnection, &SendVideoFrame, &SendDataFrame,
        &SendReliableDataFrame, &UpdateVideoBitrateLimit};
    return version == abi_version ? &api : nullptr;
}
