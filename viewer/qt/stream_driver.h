#pragma once

#include <p2premote/core_api.h>

#include <QString>
#include <QTimer>
#if defined(Q_OS_WIN)
#include <QWinEventNotifier>
#endif

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

struct LanConnectOptions {
    std::wstring host;
    std::uint16_t port{P2P_LAN_DEFAULT_PORT};
    std::wstring access_code;
};

class StreamDriver final : public QObject {
    Q_OBJECT

public:
    StreamDriver(p2p_account* account, bool host, const wchar_t* device, void* video_parent,
        const LaunchOptions& options, QObject* parent, bool diagnostics = false);
    StreamDriver(const LanConnectOptions& lan, void* video_parent,
        const LaunchOptions& options, QObject* parent);
    ~StreamDriver() override;

    p2p_stream* handle() const { return stream_.get(); }
    void update(int result);
    void resize(bool drawable);

signals:
    void changed(const p2p_stream_snapshot_v1& snapshot);
    void uiCommandsReady();
    void failed(const QString& message);

private:
    void initTimersAndWaits();
    void dispatch(uint32_t kind);
    void syncWaits();
    void publish();
    void publishIfChanged();
    void publishSnapshot(bool force);
    bool hasVideo() const noexcept { return state_ == P2P_STREAMING && width_ > 0 && height_ > 0; }

    std::unique_ptr<p2p_stream, decltype(&p2p_stream_destroy)> stream_{nullptr, p2p_stream_destroy};
#if defined(Q_OS_WIN)
    std::array<QWinEventNotifier, 4> waits_;
#endif
    QTimer tick_, cursor_, heartbeat_;
    int state_{P2P_IDLE};
    bool input_ready_{};
    uint32_t width_{};
    uint32_t height_{};
    QString status_;
    QString notice_;
};
