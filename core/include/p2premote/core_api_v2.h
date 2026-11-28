#ifndef P2PREMOTE_CORE_API_V2_H
#define P2PREMOTE_CORE_API_V2_H

#include <p2premote/core_api.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    P2P_RENDER_TARGET_NONE = 0,
    P2P_RENDER_TARGET_WIN32_WINDOW = 1,
    P2P_RENDER_TARGET_QT_VIDEO_SURFACE = 2
};

/* The target and callback context belong to the caller and must outlive the stream.
 * If supplied, the callback runs on the stream's owner thread and may queue a UI action. */
typedef struct p2p_render_target_v2 {
    uint32_t kind;
    uintptr_t native_target;
    void (*request_fullscreen)(void* context);
    void* context;
} p2p_render_target_v2;

typedef struct p2p_connection_options_v2 {
    const char* ice_trace_directory;
    const char* latency_trace_directory;
    const char* stun_url;
    int32_t stun_only, auto_control, diagnostics;
} p2p_connection_options_v2;

typedef struct p2p_lan_options_v2 {
    const char* host_address;
    uint16_t port;
    const char* access_code;
    int32_t auto_control;
    const char* ice_trace_directory;
    const char* latency_trace_directory;
} p2p_lan_options_v2;

typedef struct p2p_stream_snapshot_v2 {
    const char* status;
    const char* codec;
    const char* plugin_notice;
    const char* ice_protocol;
    uint64_t captured, submitted, received, decoded, presented;
    uint32_t width, height, upload_bps, download_bps, bitrate_limit_bps;
    int32_t state, ice_path, desktop_control, input_ready, has_plugin_menu, srtp, gpu_native, gpu_conversion;
    double seconds, rtt_ms, loss_fraction, capture_to_submit_ms, draw_to_present_ms;
    double send_queue_ms, decoded_frame_wait_ms;
} p2p_stream_snapshot_v2;

enum {
    P2P_STREAM_EVENT_SESSION = 1u << 0,
    P2P_STREAM_EVENT_CAPTURE = 1u << 1,
    P2P_STREAM_EVENT_PRESENT = 1u << 2,
    P2P_STREAM_EVENT_INPUT = 1u << 3
};

typedef struct p2p_stream_events_v2 {
    uint32_t ready;
    int32_t state, input_ready;
} p2p_stream_events_v2;

/* UTF-8 strings, owner-thread calls and borrowed results as in v1.
 * Snapshot strings remain valid until the next v2 snapshot on the same stream. */
P2P_CORE_API const char* p2p_last_error_utf8(void);
P2P_CORE_API const char* p2p_default_server_utf8(void);
P2P_CORE_API int32_t p2p_account_login_v2(p2p_account* account, const char* server, const char* user,
    const char* password, int32_t create, int32_t compatible_http, int32_t relay_only);
/* P2P_PENDING means there was no completed account operation to consume. */
P2P_CORE_API int32_t p2p_account_poll_events(p2p_account* account);
P2P_CORE_API p2p_stream* p2p_stream_create_v2(const p2p_account* account, int32_t host,
    const char* remote_device, const p2p_render_target_v2* target, const p2p_connection_options_v2* options);
P2P_CORE_API p2p_stream* p2p_stream_create_lan_v2(int32_t host, const p2p_render_target_v2* target,
    const p2p_lan_options_v2* options);
P2P_CORE_API int32_t p2p_stream_stop_v2(p2p_stream* stream, const char* reason);
P2P_CORE_API int32_t p2p_stream_snapshot_v2_read(p2p_stream* stream, p2p_stream_snapshot_v2* snapshot);
/* Nonblocking. On Windows this pumps ready native events. On Linux the caller
 * also runs its event loop for socket and renderer callbacks. */
P2P_CORE_API int32_t p2p_stream_poll_events(p2p_stream* stream, p2p_stream_events_v2* events);

#ifdef __cplusplus
}
#endif

#endif
