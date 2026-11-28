#include "viewer_input.h"
#include "qpc.h"

#include <p2premote/video_viewport.h>

#include <windowsx.h>
#include <algorithm>
#include <utility>
#include <vector>

namespace p2premote {
namespace {
constexpr std::int64_t mouse_move_min_interval_ms = 4;
constexpr std::int64_t drag_tail_delay_ms = 24;
}

void ViewerInput::Attach(HWND surface) noexcept {
    if (surface_ == surface) return;
    EndDirectKeyboardInput();
    surface_ = surface;
}

void ViewerInput::BeginDirectKeyboardInput() {
    if (previous_layout_ || !surface_) return;
    previous_layout_ = GetKeyboardLayout(0);
    previous_ime_ = ImmAssociateContext(surface_, nullptr);

    const auto is_english = [](HKL layout) {
        return PRIMARYLANGID(LOWORD(reinterpret_cast<ULONG_PTR>(layout))) == LANG_ENGLISH && !ImmIsIME(layout);
    };
    if (is_english(previous_layout_)) return;
    const int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) return;
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    layouts.resize(GetKeyboardLayoutList(count, layouts.data()));
    const auto english = std::ranges::find_if(layouts, is_english);
    if (english != layouts.end()) ActivateKeyboardLayout(*english, 0);
}

void ViewerInput::EndDirectKeyboardInput() noexcept {
    const auto layout = std::exchange(previous_layout_, nullptr);
    if (!layout) return;
    if (GetKeyboardLayout(0) != layout) ActivateKeyboardLayout(layout, 0);
    const auto ime = std::exchange(previous_ime_, nullptr);
    if (IsWindow(surface_) && ime) ImmAssociateContext(surface_, ime);
}

void ViewerInput::Reset() {
    ReleaseViewerInput();
    last_move_qpc_ = 0;
    last_local_cursor_active_ = false;
    last_local_cursor_ = nullptr;
}

bool ViewerInput::RefreshReady() {
    const auto ready = session_.InputReady();
    if (ready == input_ready_) return false;
    input_ready_ = ready;
    if (ready) {
        if (surface_ && GetFocus() == surface_) BeginDirectKeyboardInput();
        session_.SubmitInput({InputKind::heartbeat});
    } else {
        EndDirectKeyboardInput();
        ReleaseViewerInput();
    }
    return true;
}

void ViewerInput::Heartbeat() {
    drag_tail_pending_ = false;
    if (pressed_.Any() && !session_.SubmitInput({InputKind::heartbeat})) ReleaseViewerInput();
}

void ViewerInput::UpdatePluginLocks(StreamSession& session) {
    session.UpdatePluginLockState((GetKeyState(VK_CAPITAL) & 1) != 0, (GetKeyState(VK_NUMLOCK) & 1) != 0);
}

void ViewerInput::QueueMouseMove(const InputPacket& input) {
    if (!pending_move_ && last_move_sent_ && last_move_sent_->first == input.x && last_move_sent_->second == input.y) {
        return;
    }
    pending_move_ = input;
    FlushPendingMove(false);
}

void ViewerInput::FlushPendingMove(bool force) {
    if (!pending_move_) {
        if (drag_tail_pending_ && PendingMoveTimeoutMs() == 0) Heartbeat();
        return;
    }
    if (!input_ready_) return;
    const auto now = QpcNow();
    const auto min_interval = QpcFrequency() * mouse_move_min_interval_ms / 1000;
    if (!force && last_move_qpc_ && now - last_move_qpc_ < min_interval) return;
    const auto move = *std::exchange(pending_move_, std::nullopt);
    if (last_move_sent_ && last_move_sent_->first == move.x && last_move_sent_->second == move.y) return;
    if (session_.SubmitInput(move)) {
        last_move_sent_ = {move.x, move.y};
        last_move_qpc_ = now;
        drag_tail_pending_ = pressed_.Dragging();
    }
}

DWORD ViewerInput::PendingMoveTimeoutMs() const noexcept {
    if ((!pending_move_ && !drag_tail_pending_) || !input_ready_) return INFINITE;
    const auto delay = pending_move_ ? mouse_move_min_interval_ms : drag_tail_delay_ms;
    const auto min_interval = QpcFrequency() * delay / 1000;
    const auto elapsed = last_move_qpc_ ? (QpcNow() - last_move_qpc_) : min_interval;
    if (elapsed >= min_interval) return 0;
    const auto remaining_ms = ((min_interval - elapsed) * 1000 + QpcFrequency() - 1) / QpcFrequency();
    return static_cast<DWORD>(remaining_ms);
}

void ViewerInput::RefreshLocalCursor() {
    HCURSOR cursor{};
    const bool active = session_.LocalCursor(cursor);
    if (active == last_local_cursor_active_ && cursor == last_local_cursor_) return;
    last_local_cursor_active_ = active;
    last_local_cursor_ = cursor;
    POINT point{};
    if (active && GetFocus() == surface_ && GetCursorPos(&point)
            && (WindowFromPoint(point) == surface_ || GetCapture() == surface_) && ScreenToClient(surface_, &point)
            && PointInput(InputKind::move, 0, point, pressed_.Dragging())) {
        SetCursor(cursor);
    }
}

std::optional<LRESULT> ViewerInput::OnSurfaceMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SETFOCUS:
        if (input_ready_) BeginDirectKeyboardInput();
        break;
    case WM_KILLFOCUS:
        EndDirectKeyboardInput();
        ReleaseViewerInput();
        break;
    case WM_NCDESTROY:
        Attach(nullptr);
        break;
    case WM_CANCELMODE:
        ReleaseViewerInput();
        break;
    case WM_CAPTURECHANGED:
        if (pressed_.Dragging()) ReleaseViewerInput();
        break;
    }
    if (!input_ready_) return std::nullopt;

    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_SETCURSOR:
        if (GetFocus() == hwnd && LOWORD(lparam) == HTCLIENT) {
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)
                && PointInput(InputKind::move, 0, cursor, pressed_.Dragging())) {
                HCURSOR shape{};
                session_.LocalCursor(shape);
                SetCursor(shape);
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: SetButton(InputButton::left, true, point); return 0;
    case WM_LBUTTONUP: SetButton(InputButton::left, false, point); return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: SetButton(InputButton::right, true, point); return 0;
    case WM_RBUTTONUP: SetButton(InputButton::right, false, point); return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK: SetButton(InputButton::middle, true, point); return 0;
    case WM_MBUTTONUP: SetButton(InputButton::middle, false, point); return 0;
    case WM_MOUSEMOVE:
        if (GetFocus() == hwnd) {
            if (const auto input = PointInput(InputKind::move, 0, point, pressed_.Dragging())) {
                QueueMouseMove(*input);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (GetFocus() == hwnd && ScreenToClient(hwnd, &point)) {
            FlushPendingMove(true);
            const auto code = static_cast<std::uint16_t>(GET_WHEEL_DELTA_WPARAM(wparam));
            if (const auto input = PointInput(InputKind::wheel, code, point, pressed_.Dragging())) SendControl(*input);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (wparam == VK_CAPITAL || wparam == VK_NUMLOCK) {
            UpdatePluginLocks(session_);
            if (session_.PluginCapturesLockKeys()) return 0;
        }
        if (GetFocus() == hwnd) SetKey(wparam, lparam, message == WM_KEYDOWN);
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (GetFocus() == hwnd) {
            SetKey(wparam, lparam, message == WM_SYSKEYDOWN);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (GetFocus() == hwnd) return 0;
        break;
    case WM_CHAR:
        return 0;
    }
    return std::nullopt;
}

std::optional<InputPacket> ViewerInput::PointInput(InputKind kind, std::uint16_t code, POINT point, bool clamp) const {
    RECT client{};
    GetClientRect(surface_, &client);
    unsigned width{}, height{};
    session_.VideoDimensions(width, height);
    const auto viewport = FitVideo(client.right, client.bottom, width, height);
    if (!viewport.width || !viewport.height) return std::nullopt;
    const auto x = static_cast<int>(point.x) - static_cast<int>(viewport.left);
    const auto y = static_cast<int>(point.y) - static_cast<int>(viewport.top);
    const auto right = static_cast<int>(viewport.width) - 1;
    const auto bottom = static_cast<int>(viewport.height) - 1;
    if (!clamp && (x < 0 || y < 0 || x > right || y > bottom)) return std::nullopt;
    const auto normalized = [](int coordinate, int edge) {
        return static_cast<std::uint16_t>(MulDiv(std::clamp(coordinate, 0, edge), 65535, std::max(1, edge)));
    };
    return InputPacket{kind, code, normalized(x, right), normalized(y, bottom)};
}

void ViewerInput::SetButton(InputButton button, bool down, POINT point) {
    const auto code = static_cast<std::uint16_t>(button);
    if (pressed_.buttons.test(code) == down) return;
    FlushPendingMove(true);
    const auto input = PointInput(down ? InputKind::button_down : InputKind::button_up, code, point, pressed_.Dragging());
    if (!input) {
        if (!down) ReleaseViewerInput();
        return;
    }
    if (down) {
        SetFocus(surface_);
        if (GetFocus() != surface_) return;
        HCURSOR cursor{};
        session_.LocalCursor(cursor);
        SetCursor(cursor);
    }
    if (!SendControl(*input)) return;
    drag_tail_pending_ = false;
    last_move_sent_ = {input->x, input->y};
    pressed_.buttons.set(code, down);
    if (down) SetCapture(surface_);
    else if (pressed_.buttons.none() && GetCapture() == surface_) ReleaseCapture();
}

void ViewerInput::SetKey(WPARAM key, LPARAM flags, bool down) {
    if (key == VK_F12) {
        if (down) SetFocus(GetAncestor(surface_, GA_ROOT));
        return;
    }
    FlushPendingMove(true);
    const auto code = static_cast<std::uint16_t>((flags >> 16) & 0x1ff);
    if (!code || (pressed_.keys.test(code) == down && !down)) return;
    if (SendControl({down ? InputKind::key_down : InputKind::key_up, code})) pressed_.keys.set(code, down);
}

bool ViewerInput::SendControl(const InputPacket& input) {
    if (session_.SubmitInput(input)) return true;
    ReleaseViewerInput();
    return false;
}

void ViewerInput::ReleaseViewerInput() {
    drag_tail_pending_ = false;
    if (!surface_) return;
    pending_move_.reset();
    last_move_sent_.reset();
    pressed_.Clear();
    if (GetCapture() == surface_) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (input_ready_) session_.SubmitInput({InputKind::reset});
}

} // namespace p2premote
