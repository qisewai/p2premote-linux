#include "signal_socket.h"

#include "stream_events.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <stdexcept>

namespace p2premote {
namespace {

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;

template <typename Operation>
void Await(const Operation& operation, std::stop_token stop, std::chrono::seconds timeout = std::chrono::seconds(5)) {
    const std::stop_callback cancel(stop, [&operation]() noexcept {
        try { operation.Cancel(); } catch (...) {}
    });
    if (operation.wait_for(timeout) == AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("信令操作超时，请检查服务地址和网络连接");
    }
    // wait_for has already completed the operation, so GetResults does not block.
    operation.GetResults();
}

void Write(const DataWriter& writer, std::stop_token stop, const std::string& kind, const std::string& payload) {
    JsonObject object;
    object.SetNamedValue(L"type", JsonValue::CreateStringValue(winrt::to_hstring(kind)));
    object.SetNamedValue(L"payload", JsonValue::CreateStringValue(winrt::to_hstring(payload)));
    writer.WriteString(object.Stringify());
    Await(writer.StoreAsync(), stop);
}

} // namespace

SignalSocket::SignalSocket(std::shared_ptr<StreamEvents> events, std::wstring endpoint, std::string kind, std::string payload,
                           std::wstring token)
    : events_(std::move(events)), worker_([this, endpoint = std::move(endpoint), kind = std::move(kind),
                                          payload = std::move(payload), token = std::move(token)](std::stop_token stop) {
        try {
            winrt::init_apartment();
            try {
                Run(stop, endpoint, kind, payload, token);
            } catch (...) {
                winrt::uninit_apartment();
                throw;
            }
            winrt::uninit_apartment();
        } catch (const winrt::hresult_error& error) {
            if (!stop.stop_requested()) {
                events_->Push({StreamEventType::error, {}, "信令连接失败 · " + winrt::to_string(error.message())});
            }
        } catch (const std::exception& error) {
            if (!stop.stop_requested()) {
                events_->Push({StreamEventType::error, {}, error.what()});
            }
        }
    }) {}

void SignalSocket::Send(std::string kind, std::string payload) {
    {
        const std::lock_guard lock(mutex_);
        if (pending_.size() >= 64) {
            throw std::runtime_error("信令发送队列已满");
        }
        pending_.emplace_back(std::move(kind), std::move(payload));
    }
    changed_.notify_one();
}

void SignalSocket::Run(std::stop_token stop, const std::wstring& endpoint, const std::string& kind,
                       const std::string& payload, const std::wstring& token) {
    MessageWebSocket socket;
    const Uri address(endpoint);
    if (!token.empty()) {
        if (address.SchemeName() != L"wss") throw std::invalid_argument("公网访问密钥只能通过 WSS 发送");
        socket.SetRequestHeader(L"Authorization", L"Bearer " + token);
    }
    socket.Control().MessageType(SocketMessageType::Utf8);
    socket.Control().MaxMessageSize(256 * 1024);
    const auto received = socket.MessageReceived(winrt::auto_revoke,
        [events = events_](const MessageWebSocket&, const MessageWebSocketMessageReceivedEventArgs& args) {
            try {
                auto reader = args.GetDataReader();
                const auto json = JsonObject::Parse(reader.ReadString(reader.UnconsumedBufferLength()));
                events->Push({StreamEventType::signal, winrt::to_string(json.GetNamedString(L"type")),
                    winrt::to_string(json.GetNamedString(L"payload", L""))});
            } catch (...) {
                events->Push({StreamEventType::error, {}, "收到无效的信令消息"});
            }
        });
    const auto closed = socket.Closed(winrt::auto_revoke,
        [events = events_](const IWebSocket&, const WebSocketClosedEventArgs&) {
            events->Push({StreamEventType::closed, {}, {}});
        });

    const auto connect_timeout = token.empty() ? std::chrono::seconds(5) : std::chrono::seconds(15);
    Await(socket.ConnectAsync(address), stop, connect_timeout);
    const DataWriter writer(socket.OutputStream());
    if (!stop.stop_requested()) {
        Write(writer, stop, kind, payload);
    }
    for (;;) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait(lock, stop, [this] { return !pending_.empty(); }) || stop.stop_requested()) {
            break;
        }
        auto next = std::move(pending_.front());
        pending_.pop_front();
        lock.unlock();
        Write(writer, stop, next.first, next.second);
    }
    writer.DetachStream();
    socket.Close(1000, L"");
}

} // namespace p2premote
