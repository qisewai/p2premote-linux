#pragma once

#include <p2premote/stream_session.h>

#include <bitset>

namespace p2premote {

class ViewerInput {
public:
    explicit ViewerInput(StreamSession& session) : session_(session) {}

    void Attach(HWND surface) noexcept { surface_ = surface; }
    void Reset();
    bool RefreshReady();
    void Heartbeat();
    void RefreshLocalCursor();
    void FlushPendingMove(bool force = false);
    DWORD PendingMoveTimeoutMs() const noexcept;
    std::optional<LRESULT> OnSurfaceMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static void UpdatePluginLocks(StreamSession& session);

private:
    void ReleaseViewerInput();
    void QueueMouseMove(const InputPacket& input);
    std::optional<InputPacket> PointInput(InputKind kind, std::uint16_t code, POINT point, bool clamp) const;
    void SetButton(InputButton button, bool down, POINT point);
    void SetKey(WPARAM key, LPARAM flags, bool down);
    bool SendControl(const InputPacket& input);

    StreamSession& session_;
    HWND surface_{};
    bool input_ready_{};
    std::optional<InputPacket> pending_move_;
    std::optional<std::pair<std::uint16_t, std::uint16_t>> last_move_sent_;
    std::int64_t last_move_qpc_{};
    bool drag_tail_pending_{};
    bool last_local_cursor_active_{};
    HCURSOR last_local_cursor_{};
    std::bitset<4> buttons_;
    std::bitset<512> keys_;
};

} // namespace p2premote
