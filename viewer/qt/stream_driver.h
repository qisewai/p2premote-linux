#pragma once

#include <p2premote/core_api.h>

#include <QTimer>
#include <QWinEventNotifier>

#include <array>
#include <memory>
#include <string>

struct LaunchOptions {
    std::wstring ice_trace;
    std::wstring latency_trace;
    std::string stun;
    bool stun_only{};
    bool relay_only{};
    bool compatible_http{};
};

class StreamDriver final : public QObject {
    Q_OBJECT

public:
    StreamDriver(p2p_account* account, bool host, const wchar_t* device, void* video_parent,
        const LaunchOptions& options, QObject* parent);
    ~StreamDriver() override;

    p2p_stream* handle() const { return stream_.get(); }
    void update(int result);
    void resize(bool drawable);

signals:
    void changed(const p2p_stream_snapshot_v1& snapshot);
    void failed(const QString& message);

private:
    void dispatch(uint32_t kind);
    void syncWaits();
    void publish();

    std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream_{nullptr, p2p_stream_destroy};
    std::array<QWinEventNotifier, 4> waits_;
    QTimer tick_, cursor_, heartbeat_;
    int state_{P2P_IDLE};
    bool input_ready_{};
};
