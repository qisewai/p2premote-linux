#include <p2premote/core_api.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static int check(int condition, const char* description) {
    if (!condition) fprintf(stderr, "%s\n", description);
    return condition;
}

int main(void) {
    if (!check(p2p_core_abi_version() == P2P_CORE_ABI_VERSION, "ABI version mismatch")) return 1;
    if (!check(p2p_default_server()[0] != 0, "Missing default server")) return 1;

    p2p_account* account = p2p_account_create();
    if (!check(account != NULL, "Account allocation failed")) return 1;
    int passed = check(p2p_account_device_id(account) == NULL, "Unexpected initial login")
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
    {
        p2p_ui_command ui_cmd = {0};
        p2p_audio_options_v1 audio = {1, 0, 1.0f, 1};
        passed &= check(p2p_stream_poll_ui_command(NULL, &ui_cmd) == P2P_ERROR, "Null stream poll accepted")
            && check(p2p_stream_send_ui_event(NULL, 1, "btn", "click", "{}") == P2P_ERROR, "Null stream UI event accepted")
            && check(p2p_stream_set_audio(NULL, &audio) == P2P_ERROR, "Null stream audio options accepted");
    }
    p2p_account_destroy(account);
    return passed ? 0 : 1;
}
