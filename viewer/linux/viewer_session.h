#pragma once

#include <p2premote/input_state.h>

#include <p2premote/session_policy.h>

#include "cloud_connection.h"
#include "cursor_shape.h"
#include "managed_api.h"
#include "video_surface.h"

#include <p2premote/core_api.h>
#include <p2premote/input_protocol.h>
#include <p2premote/plugin_workspace.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QTcpSocket>
#include <QTimer>
#include <QWebSocket>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class QJsonObject;
class ViewerAudio;

namespace p2premote { class PluginHost; class PluginOverlays; }

class ViewerSession final : public QObject {
public:
    ViewerSession();
    ~ViewerSession() override;

    void open(const QString& host, quint16 port, const QString& code);
    void openCloud(CloudConnection connection, const QString& device, bool diagnostics);
    void close();
    int state() const;
    bool isOpen() const { return state_ != State::idle; }
    bool canControl() const { return state_ == State::streaming && control_allowed_; }
    bool isViewable() const { return state_ == State::streaming && !diagnostics_; }
    bool sendInput(p2premote::InputKind kind, std::uint16_t code = 0, std::uint16_t x = 0, std::uint16_t y = 0);
    bool updateKeyboardLocks(bool caps_lock, bool num_lock);
    void setQuality(int resolution, int bitrate);
    void setAudio(p2p_audio_options_v1 options);
    void tick();
    bool takeLatestFrame(VideoFrame& frame);
    void recordFrameSwapped(double wait_ms, double draw_to_swap_ms);
    std::uint64_t decodedFrames() const { return decoded_frames_.load(std::memory_order_relaxed); }
    double drawToPresentMs() const { return metrics_.presented ? draw_to_present_ms_ : -1.0; }
    const p2premote::PluginMetrics& metrics() const { return metrics_; }
    p2premote::PluginHost* plugins() const { return plugins_.get(); }
    const std::shared_ptr<p2premote::PluginOverlays>& overlays() const { return overlay_; }
    const QString& notice() const { return notice_; }
    int icePath() const { return ice_path_; }
    const QByteArray& iceProtocol() const { return ice_protocol_; }
    bool srtpActive() const { return srtp_active_; }
    std::uint32_t confirmedBitrate() const { return confirmed_bitrate_; }

    std::function<void(const QString&)> statusChanged;
    std::function<void()> frameReady;
    std::function<void(DecodedCursor)> cursorChanged;

private:
    enum class State { idle, challenge, authentication, approval, negotiation, streaming, reconnecting };

    struct PeerContext {
        ViewerSession& session;
        std::uint64_t generation;
    };

    struct ReconnectState {
        bool is_cloud{};
        CloudConnection cloud;
        QString cloud_device;
        bool diagnostics{};
        QString lan_host;
        quint16 lan_port{};
        QByteArray lan_key;
        p2premote::ReconnectWindow window;
        QString reason;
    };

    void openWithKey(const QString& host, quint16 port, const QByteArray& key, bool reconnecting);
    void openCloudInternal(CloudConnection connection, const QString& device, bool diagnostics, bool reconnecting);
    void teardownTransport();
    void clearReconnect();
    void recover(const QString& reason);
    void attemptReconnect();
    void receiveFrames();
    void handleFrame(const QJsonObject& message);
    bool sendFrame(const QJsonObject& message);
    bool startPeer();
    void accept(const QString& mode);
    void startStreaming();
    void forwardSignal(const QByteArray& data);
    void receiveSignal(const QByteArray& data);
    void processData(QByteArray data, const QByteArray& label);
    bool sendReliable(std::string_view data, const char* channel);
    void processPluginActions();
    void applyMediaSettings();
    void sendCursorReady();
    void sendVideoSettings();
    void sendAudioSettings();
    void queueFrame(const char* data, int width, int height, std::size_t size, std::uint64_t generation);
    void stop(const QString& status);

    static void sendSignal(const char* data, size_t size, void* context) noexcept;
    static void connectionChanged(ConnectionStatus status, const char*, size_t, void* context);
    static void receiveVideo(const MiniRtcVideoFrame* frame, const char*, size_t, const char* stream,
        size_t stream_size, void* context);
    static void receiveData(const char*, size_t, const char*, size_t, const char*, size_t, void*);
    static void receiveAudio(const char*, size_t, const char*, size_t, const char*, size_t, void*);
    static void reportTraffic(const char*, size_t, TraversalMode, const MiniRtcNetTrafficStats*, const char*, size_t, void*);

    QTcpSocket socket_;
    QWebSocket web_socket_;
    QTimer timeout_;
    QTimer plugin_timer_;
    QTimer reconnect_timer_;
    QTimer ws_ping_timer_;
    QByteArray incoming_;
    QByteArray key_;
    QByteArray expected_host_mac_;
    QString lan_host_;
    quint16 lan_port_{};
    QByteArray lan_key_;
    std::optional<ReconnectState> reconnect_;
    State state_{State::idle};
    const p2premote::native_minirtc::Api* api_{};
    PeerPtr* peer_{};
    std::unique_ptr<PeerContext> context_;
    std::uint64_t generation_{};
    std::uint32_t sequence_{};
    bool control_allowed_{};
    bool transport_ready_{};
    bool diagnostics_{};
    std::optional<CloudConnection> cloud_;
    QString cloud_device_;
    bool plugin_channel_{};
    bool plugin_ready_{};
    int workspace_attempts_{};
    std::shared_ptr<p2premote::PluginOverlays> overlay_;
    std::unique_ptr<p2premote::PluginHost> plugins_;
    std::unique_ptr<p2premote::PluginHost> overlay_plugins_;
    p2premote::PluginMetrics metrics_{};
    QString notice_;
    QElapsedTimer connected_since_;
    QElapsedTimer notice_age_;
    QElapsedTimer workspace_retry_;
    QElapsedTimer cursor_retry_;
    int ice_path_{};
    QByteArray ice_protocol_;
    bool srtp_active_{};
    bool cursor_separate_{};
    int requested_quality_{P2P_1080P};
    int requested_bitrate_{P2P_AUTOMATIC};
    int active_bitrate_{P2P_AUTOMATIC};
    p2premote::VideoSettingsNegotiation video_settings_{{1'000, 13'000, 12}};
    std::uint32_t confirmed_bitrate_{};
    p2p_audio_options_v1 audio_{};
    std::mutex sound_mutex_;
    std::shared_ptr<ViewerAudio> sound_;
    std::uint64_t sound_generation_{};
    p2premote::InputSendState input_send_;

    std::mutex frame_mutex_;
    VideoFrame pending_frame_;
    VideoFrame recycled_frame_;
    QSize latest_frame_size_;
    std::uint64_t latest_frame_generation_{};
    std::atomic<std::uint64_t> decoded_frames_{};
    bool frame_update_pending_{};
    double frame_wait_sum_ms_{};
    std::uint64_t frame_wait_samples_{};
    double draw_to_present_ms_{};
};
