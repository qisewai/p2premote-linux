#pragma once

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
#include <QTcpSocket>
#include <QTimer>
#include <QWebSocket>

#include <bitset>
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
    const p2premote::PluginMetrics& metrics() const { return metrics_; }
    p2premote::PluginHost* plugins() const { return plugins_.get(); }
    const std::shared_ptr<p2premote::PluginOverlays>& overlays() const { return overlay_; }
    const QString& notice() const { return notice_; }
    int icePath() const { return ice_path_; }
    const QByteArray& iceProtocol() const { return ice_protocol_; }
    bool srtpActive() const { return srtp_active_; }
    std::uint32_t confirmedBitrate() const { return confirmed_bitrate_; }

    std::function<void(const QString&)> statusChanged;
    std::function<void(VideoFrame&)> frameReceived;
    std::function<void(DecodedCursor)> cursorChanged;

private:
    enum class State { idle, challenge, authentication, approval, negotiation, streaming };

    struct PeerContext {
        ViewerSession& session;
        std::uint64_t generation;
    };

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
    QByteArray incoming_;
    QByteArray key_;
    QByteArray expected_host_mac_;
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
    QElapsedTimer video_retry_;
    QElapsedTimer cursor_retry_;
    int ice_path_{};
    QByteArray ice_protocol_;
    bool srtp_active_{};
    bool cursor_separate_{};
    int cursor_attempts_{};
    int requested_quality_{P2P_1080P};
    int requested_bitrate_{P2P_AUTOMATIC};
    int active_bitrate_{P2P_AUTOMATIC};
    int video_attempts_{};
    bool video_acknowledged_{};
    std::uint32_t video_request_{};
    std::uint32_t confirmed_bitrate_{};
    p2p_audio_options_v1 audio_{};
    std::mutex sound_mutex_;
    std::shared_ptr<ViewerAudio> sound_;
    std::uint64_t sound_generation_{};
    std::bitset<4> sent_buttons_;
    std::optional<p2premote::InputPacket> drag_checkpoint_;

    std::mutex frame_mutex_;
    VideoFrame pending_frame_;
    VideoFrame recycled_frame_;
    std::uint64_t pending_frame_generation_{};
    std::uint64_t pending_frame_ms_{};
    bool frame_update_pending_{};
    double frame_wait_sum_ms_{};
    std::uint64_t frame_wait_samples_{};
};
