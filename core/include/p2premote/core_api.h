#ifndef P2PREMOTE_CORE_API_H
#define P2PREMOTE_CORE_API_H

#include <stdint.h>
#include <stddef.h>

#if defined(P2PREMOTE_CORE_EXPORTS)
#define P2P_CORE_API __declspec(dllexport)
#else
#define P2P_CORE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define P2P_CORE_ABI_VERSION 1u
/* The viewer posts this message to video_parent for a local F11 fullscreen toggle. */
#define P2P_VIDEO_FULLSCREEN_MESSAGE 0x8042u

typedef struct p2p_account p2p_account;
typedef struct p2p_stream p2p_stream;

enum p2p_result {
    P2P_OK = 0,
    P2P_ERROR = -1,
    P2P_LOGIN_EXPIRED = -2,
    P2P_PENDING = 1
};

enum p2p_state {
    P2P_IDLE, P2P_WAITING, P2P_APPROVAL, P2P_NEGOTIATING, P2P_STREAMING,
    P2P_RECONNECTING, P2P_STOPPED
};

enum p2p_ice_path { P2P_ICE_UNKNOWN, P2P_ICE_HOST, P2P_ICE_NAT, P2P_ICE_RELAY };
enum p2p_quality { P2P_720P = 1, P2P_1080P, P2P_1440P };
enum p2p_bitrate { P2P_ECONOMY = 1, P2P_AUTOMATIC, P2P_DETAIL };
enum p2p_wait_kind { P2P_WAIT_SESSION, P2P_WAIT_CAPTURE, P2P_WAIT_PRESENT, P2P_WAIT_INPUT };

typedef struct p2p_device {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* platform;
    const wchar_t* state;
} p2p_device;

typedef struct p2p_connection_options_v1 {
    const wchar_t* ice_trace_directory;
    const wchar_t* latency_trace_directory;
    const char* stun_url;
    int32_t stun_only;
    int32_t auto_control;
} p2p_connection_options_v1;

typedef struct p2p_stream_snapshot_v1 {
    const wchar_t* status;
    const wchar_t* codec;
    const wchar_t* plugin_notice;
    const char* ice_protocol;
    uint64_t captured, submitted, received, decoded, presented;
    uint32_t width, height, upload_bps, download_bps, bitrate_limit_bps;
    int32_t state, ice_path, desktop_control, input_ready, has_plugin_menu, srtp, gpu_native, gpu_conversion;
    /* Missing measurements are negative. */
    double seconds, rtt_ms, loss_fraction, capture_to_submit_ms, draw_to_present_ms;
    double send_queue_ms, decoded_frame_wait_ms;
} p2p_stream_snapshot_v1;

typedef struct p2p_menu_item {
    const wchar_t* label;
    uint32_t id;
    /* -1 means a plain command, 0 and 1 indicate a toggle. */
    int32_t checked;
} p2p_menu_item;

/*
 * Windows x64, default C calling convention. Strings are UTF-16 unless marked char (UTF-8).
 * Use each handle from its creating thread and release it with its DLL destroy function.
 * Input pointers need only remain valid for the call. Returned pointers are borrowed.
 */
P2P_CORE_API uint32_t p2p_core_abi_version(void);
P2P_CORE_API const wchar_t* p2p_last_error(void); /* This thread's most recent error. */
P2P_CORE_API const wchar_t* p2p_default_server(void);

P2P_CORE_API p2p_account* p2p_account_create(void);
P2P_CORE_API void p2p_account_destroy(p2p_account* account);
/* The account event is a manual-reset HANDLE, valid until account destruction. */
P2P_CORE_API void* p2p_account_event(const p2p_account* account);
P2P_CORE_API int32_t p2p_account_login(p2p_account* account, const wchar_t* server,
    const wchar_t* user, const wchar_t* password, int32_t create, int32_t compatible_http, int32_t relay_only);
P2P_CORE_API int32_t p2p_account_refresh(p2p_account* account);
P2P_CORE_API int32_t p2p_account_logout(p2p_account* account);
P2P_CORE_API void p2p_account_cancel(p2p_account* account);
P2P_CORE_API int32_t p2p_account_complete(p2p_account* account);
/* Account and device string views expire on the next completed request or logout. */
P2P_CORE_API const wchar_t* p2p_account_device_id(const p2p_account* account);
P2P_CORE_API size_t p2p_account_device_count(const p2p_account* account);
P2P_CORE_API int32_t p2p_account_device(const p2p_account* account, size_t index, p2p_device* device);

/*
 * Pass a native parent HWND for a viewer, or NULL and a host HMONITOR to start a host.
 * The DLL owns the viewer child HWND. Destroy the stream before the parent HWND.
 * A stream owns its connection data independently of the account, and requires an STA
 * thread that dispatches native messages. Omitting options enables host auto-control.
 */
P2P_CORE_API p2p_stream* p2p_stream_create(const p2p_account* account, int32_t host,
    const wchar_t* remote_device, void* video_parent, const p2p_connection_options_v1* options);
P2P_CORE_API void p2p_stream_destroy(p2p_stream* stream);
P2P_CORE_API int32_t p2p_stream_start(p2p_stream* stream, void* monitor, uint32_t fps);
P2P_CORE_API int32_t p2p_stream_stop(p2p_stream* stream, const wchar_t* reason);
P2P_CORE_API int32_t p2p_stream_approve(p2p_stream* stream, int32_t desktop);
P2P_CORE_API int32_t p2p_stream_quality(p2p_stream* stream, uint32_t quality);
P2P_CORE_API int32_t p2p_stream_bitrate(p2p_stream* stream, uint32_t bitrate);
P2P_CORE_API int32_t p2p_stream_resize(p2p_stream* stream, int32_t drawable);
P2P_CORE_API int32_t p2p_stream_display_changed(p2p_stream* stream);
P2P_CORE_API int32_t p2p_stream_tick(p2p_stream* stream);
P2P_CORE_API int32_t p2p_stream_poll_cursor(p2p_stream* stream);
P2P_CORE_API int32_t p2p_stream_heartbeat(p2p_stream* stream);
P2P_CORE_API int32_t p2p_stream_state(const p2p_stream* stream);
P2P_CORE_API int32_t p2p_stream_input_ready(const p2p_stream* stream);
/*
 * Windows HANDLE valid until the next stream operation. Duplicate it if the wait spans
 * another operation. Dispatch an activation only while that same object is still current.
 * The session event is manual-reset. Other waits may consume the signal.
 */
P2P_CORE_API void* p2p_stream_wait_handle(const p2p_stream* stream, uint32_t kind);
P2P_CORE_API int32_t p2p_stream_dispatch(p2p_stream* stream, uint32_t kind);
/* Snapshot text views expire at the next snapshot call or destruction. */
P2P_CORE_API int32_t p2p_stream_snapshot(p2p_stream* stream, p2p_stream_snapshot_v1* snapshot);
P2P_CORE_API size_t p2p_stream_read_menu(p2p_stream* stream);
/* Menu text views expire at the next read_menu call or destruction. */
P2P_CORE_API int32_t p2p_stream_menu_item(const p2p_stream* stream, size_t index, p2p_menu_item* item);
P2P_CORE_API int32_t p2p_stream_invoke_menu(p2p_stream* stream, uint32_t id);
P2P_CORE_API int32_t p2p_stream_export_csv(p2p_stream* stream, const wchar_t* file);

#ifdef __cplusplus
}
#endif
#endif
