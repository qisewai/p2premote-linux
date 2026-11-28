#include <p2premote/core_api_v2.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int check(int condition, const char* description) {
    if (!condition) fprintf(stderr, "%s\n", description);
    return condition;
}

static void fullscreen_requested(void* context) {
    ++*(int*)context;
}

int main(void) {
    if (!check(p2p_core_abi_version() == P2P_CORE_ABI_VERSION, "ABI version mismatch")) return 1;
    if (!check(p2p_default_server()[0] != 0, "Missing default server")) return 1;

    p2p_account* account = p2p_account_create();
    if (!check(account != NULL, "Account allocation failed")) return 1;
    int passed = check(!p2p_account_signed_in(account) && p2p_account_device_id(account) == NULL, "Unexpected initial login")
        && check(p2p_account_add_local_device(account) == P2P_ERROR, "Unauthenticated enrollment accepted")
        && check(p2p_account_remove_device(account, L"device") == P2P_ERROR, "Unauthenticated removal accepted")
        && check(p2p_account_service_login(account, L"http://127.0.0.1", L"test", L"test", NULL, NULL, 0, 0)
            == P2P_ERROR, "Unbound service login accepted")
        && check(p2p_stream_create(account, 1, NULL, NULL, NULL) == NULL, "Unauthenticated stream accepted")
        && check(p2p_account_refresh(account) == P2P_ERROR, "Unauthenticated refresh accepted")
        && check(p2p_account_login(account, L"", L"test", L"test", 0, 0, 0) == P2P_ERROR, "Missing server accepted")
        && check(p2p_last_error()[0] != 0, "No error from invalid login")
        && check(p2p_account_complete(account) == P2P_PENDING, "Unexpected initial request completion")
        && check(p2p_account_login(account, L"file:///test", L"test", L"test", 0, 0, 0) == P2P_OK, "Could not submit worker request")
        && check(p2p_account_login(account, L"file:///test", L"test", L"test", 0, 0, 0) == P2P_ERROR, "Overlapping login accepted")
        && check(WaitForSingleObject(p2p_account_event(account), 5000) == WAIT_OBJECT_0, "No worker event")
        && check(p2p_account_complete(account) == P2P_ERROR, "Invalid origin accepted")
        && check(p2p_last_error()[0] != 0, "Missing worker error")
        && check(WaitForSingleObject(p2p_account_event(account), 0) == WAIT_TIMEOUT, "Completion event did not reset")
        && check(p2p_account_login(account, L"file:///test", L"test", L"test", 0, 0, 0) == P2P_OK, "Could not resubmit worker request");

    p2p_account_cancel(account);
    passed &= check(p2p_account_complete(account) == P2P_PENDING, "Cancelled result was published");
    passed &= check(p2p_default_server_utf8()[0] != 0, "Missing UTF-8 default server")
        && check(p2p_account_login_v2(account, "https://localhost", "\xC0\xAF", "test", 0, 0, 0) == P2P_ERROR,
            "Malformed UTF-8 login accepted")
        && check(p2p_last_error_utf8()[0] != 0, "Missing UTF-8 error")
        && check(p2p_account_poll_events(account) == P2P_PENDING, "Malformed login produced a completion");
    if (check(p2p_account_login_v2(account, "file:///test", "test", "test", 0, 0, 0) == P2P_OK,
            "Could not submit UTF-8 worker request")) {
        int32_t event = P2P_PENDING;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        do {
            Sleep(1);
            event = p2p_account_poll_events(account);
        } while (event == P2P_PENDING && GetTickCount64() < deadline);
        passed &= check(event == P2P_ERROR && p2p_account_poll_events(account) == P2P_PENDING,
            "Portable poll did not consume exactly one completion");
    } else {
        passed = 0;
    }
    {
        p2p_ui_command ui_cmd = {0};
        p2p_audio_options_v1 audio = {1, 0, 1.0f, 1};
        passed &= check(p2p_stream_poll_ui_command(NULL, &ui_cmd) == P2P_ERROR, "Null stream poll accepted")
            && check(p2p_stream_send_ui_event(NULL, 1, "btn", "click", "{}") == P2P_ERROR, "Null stream UI event accepted")
            && check(p2p_stream_set_audio(NULL, &audio) == P2P_ERROR, "Null stream audio options accepted");
    }
    {
        p2p_lan_options_v1 bad_lan = {L"127.0.0.1", P2P_LAN_DEFAULT_PORT, L"123", 1, NULL, NULL};
        p2p_lan_scanner* scanner = p2p_lan_scanner_create();
        passed &= check(p2p_stream_create_lan(1, NULL, NULL) == NULL, "Null LAN options accepted")
            && check(p2p_stream_create_lan(1, NULL, &bad_lan) == NULL, "Short LAN access code accepted")
            && check(p2p_lan_local_addresses() != NULL, "Local LAN addresses returned NULL")
            && check(scanner != NULL, "LAN scanner creation failed");
        p2p_lan_scanner_destroy(scanner);
    }
    {
        p2p_lan_options_v2 lan = {"127.0.0.1", P2P_LAN_DEFAULT_PORT, "test-code", 0, NULL, NULL};
        const p2p_render_target_v2 unsupported = {P2P_RENDER_TARGET_QT_VIDEO_SURFACE, 1, NULL, NULL};
        passed &= check(p2p_stream_create_lan_v2(0, &unsupported, &lan) == NULL,
            "Accepted a renderer from a different platform");
        HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
        int requests = 0;
        const p2p_render_target_v2 renderer = {
            P2P_RENDER_TARGET_WIN32_WINDOW, (uintptr_t)parent, fullscreen_requested, &requests};
        p2p_stream* stream = parent ? p2p_stream_create_lan_v2(0, &renderer, &lan) : NULL;
        if (check(stream != NULL, "Could not create a stream with a typed renderer")) {
            p2p_stream_events_v2 events = {0};
            p2p_stream_snapshot_v2 snapshot = {0};
            HWND child = FindWindowExW(parent, NULL, L"STATIC", NULL);
            if (child) SendMessageW(child, WM_KEYDOWN, VK_F11, 0);
            passed &= check(requests == 1, "Typed renderer did not receive its fullscreen callback")
                && check(p2p_stream_poll_events(stream, &events) == P2P_OK && events.state == P2P_IDLE,
                    "Idle stream poll failed")
                && check(p2p_stream_stop_v2(stream, "关闭连接 😀") == P2P_OK, "UTF-8 stop failed")
                && check(p2p_stream_snapshot_v2_read(stream, &snapshot) == P2P_OK &&
                    strcmp(snapshot.status, "关闭连接 😀") == 0, "UTF-8 status did not round-trip");
            p2p_stream_destroy(stream);
        } else {
            passed = 0;
        }
        if (parent) DestroyWindow(parent);
    }
    p2p_account_destroy(account);
    return passed ? 0 : 1;
}
