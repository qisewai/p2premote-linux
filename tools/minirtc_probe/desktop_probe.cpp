#include "desktop_probe.h"

#include "desktop_input.h"
#include <p2premote/video_viewport.h>

#include <libyuv.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace desktop_probe {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using p2premote::InputKind;
using p2premote::InputPacket;

constexpr auto left_button = static_cast<std::uint16_t>(p2premote::InputButton::left);
constexpr auto right_button = static_cast<std::uint16_t>(p2premote::InputButton::right);
constexpr auto middle_button = static_cast<std::uint16_t>(p2premote::InputButton::middle);
constexpr std::array<COLORREF, 3> marker{RGB(210, 25, 25), RGB(25, 210, 25), RGB(25, 25, 210)};

BITMAPINFO BitmapInfo(int width, int height) {
    BITMAPINFO result{};
    result.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    result.bmiHeader.biWidth = width;
    result.bmiHeader.biHeight = -height;
    result.bmiHeader.biPlanes = 1;
    result.bmiHeader.biBitCount = 32;
    result.bmiHeader.biCompression = BI_RGB;
    return result;
}

std::uint16_t Normalize(int pixel, int extent) {
    const int edge = std::max(1, extent - 1);
    return static_cast<std::uint16_t>(MulDiv(std::clamp(pixel, 0, edge), 65535, edge));
}

class ScreenCapture {
public:
    void Open(const RECT& bounds) {
        bounds_ = bounds;
        const auto fit = p2premote::FitVideo(1280, 720, bounds.right - bounds.left, bounds.bottom - bounds.top);
        width_ = static_cast<int>(fit.width & ~1u);
        height_ = static_cast<int>(fit.height & ~1u);
        screen_ = GetDC(nullptr);
        if (screen_) surface_ = CreateCompatibleDC(screen_);
        if (!surface_ || width_ < 2 || height_ < 2) throw std::runtime_error("cannot open display capture");
        const auto info = BitmapInfo(width_, height_);
        bitmap_ = CreateDIBSection(surface_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        if (!bitmap_) throw std::runtime_error("cannot allocate display capture bitmap");
        previous_ = SelectObject(surface_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR) throw std::runtime_error("cannot select display capture bitmap");
        nv12_.resize(static_cast<size_t>(width_) * height_ * 3 / 2);
    }

    ~ScreenCapture() {
        if (previous_ && previous_ != HGDI_ERROR) SelectObject(surface_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (surface_) DeleteDC(surface_);
        if (screen_) ReleaseDC(nullptr, screen_);
    }

    void Send(PeerPtr* peer) {
        if (!StretchBlt(surface_, 0, 0, width_, height_, screen_, bounds_.left, bounds_.top,
                bounds_.right - bounds_.left, bounds_.bottom - bounds_.top, SRCCOPY | CAPTUREBLT) || !GdiFlush()) {
            throw std::runtime_error("cannot capture the interactive desktop");
        }
        const auto luma = static_cast<size_t>(width_) * height_;
        if (libyuv::ARGBToNV12(static_cast<const std::uint8_t*>(pixels_), width_ * 4,
                nv12_.data(), width_, nv12_.data() + luma, width_, width_, height_) != 0) {
            throw std::runtime_error("cannot convert captured pixels to NV12");
        }
        MiniRtcVideoFrame frame{};
        frame.data = reinterpret_cast<const char*>(nv12_.data());
        frame.size = nv12_.size();
        frame.width = width_;
        frame.height = height_;
        if (SendVideoFrame(peer, &frame, video_stream) != 0) throw std::runtime_error("cannot submit desktop video");
    }

    SIZE Size() const noexcept { return {width_, height_}; }

private:
    RECT bounds_{};
    int width_{}, height_{};
    HDC screen_{}, surface_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_{};
    void* pixels_{};
    std::vector<std::uint8_t> nv12_;
};

struct Frame {
    int width{}, height{};
    std::vector<std::uint8_t> nv12;
};

} // namespace

struct DesktopProbe::Session {
    explicit Session(bool is_host, bool automatic_check) : host(is_host), check(automatic_check) {}
    ~Session() {
        peer = nullptr;
        if (window) DestroyWindow(window);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Session*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Session*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->OnWindowMessage(hwnd, message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void OpenWindow() {
        monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) throw std::runtime_error("cannot locate primary display");
        bounds = info.rcMonitor;
        const auto width = bounds.right - bounds.left;
        const auto height = bounds.bottom - bounds.top;
        target = {bounds.left + width / 4, bounds.top + height / 4,
                  bounds.right - width / 4, bounds.bottom - height / 4};

        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = WindowProc;
        type.lpszClassName = L"P2PRemoteMiniRtcDesktop";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&type)) throw std::runtime_error("cannot register the desktop probe window");

        const bool marker_window = host && check;
        const DWORD style = marker_window ? WS_POPUP : (host ? WS_OVERLAPPED | WS_SYSMENU : WS_OVERLAPPEDWINDOW);
        const wchar_t* title = marker_window ? L"P2PRemote MiniRTC input check" : host ?
            L"P2PRemote MiniRTC is sharing the primary display (close to stop)" :
            L"P2PRemote MiniRTC desktop (F12 to disconnect)";
        if (!CreateWindowExW(check || host ? WS_EX_TOPMOST : 0, type.lpszClassName, title, style | WS_VISIBLE,
                marker_window ? target.left : bounds.left + 40, marker_window ? target.top : bounds.top + 40,
                marker_window ? target.right - target.left : (host ? 560 : std::min(980L, width - 80)),
                marker_window ? target.bottom - target.top : (host ? 90 : std::min(620L, height - 80)),
                nullptr, nullptr, type.hInstance, this)) {
            window = nullptr;
            throw std::runtime_error("cannot open the desktop probe window");
        }
        // The harness hides the console, which can also suppress the initial
        // window show. Explicitly show the separate desktop window.
        if (!SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
            throw std::runtime_error("cannot show the desktop probe window");
        }
        if (marker_window) SetForegroundWindow(window);
        UpdateWindow(window);
    }

    void SendInput(InputPacket packet) {
        if (!peer || host) return;
        packet.sequence = ++sequence;
        packet.sent_ms = GetTickCount64();
        const auto bytes = p2premote::EncodeInput(packet);
        if (SendReliableDataFrame(peer, reinterpret_cast<const char*>(bytes.data()), bytes.size(), input_stream) == 0) {
            ++inputs_sent;
        } else {
            input_failed = true;
        }
    }

    void Pointer(InputKind kind, std::uint16_t code, POINT point) {
        if (!viewport.width || !viewport.height) return;
        const auto x = point.x - static_cast<LONG>(viewport.left);
        const auto y = point.y - static_cast<LONG>(viewport.top);
        if (GetCapture() != window &&
            (x < 0 || y < 0 || x >= static_cast<LONG>(viewport.width) || y >= static_cast<LONG>(viewport.height))) return;
        SendInput({kind, code, Normalize(x, viewport.width), Normalize(y, viewport.height)});
    }

    void Paint() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        ++paint_calls;
        RECT client{};
        GetClientRect(window, &client);
        if (host && check) {
            HBRUSH brush = CreateSolidBrush(marker[phase]);
            FillRect(dc, &client, brush);
            DeleteObject(brush);
        } else {
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (!bgra.empty()) {
                viewport = p2premote::FitVideo(client.right, client.bottom, video_width, video_height);
                const auto info = BitmapInfo(video_width, video_height);
                const int rows = StretchDIBits(dc, viewport.left, viewport.top, viewport.width, viewport.height,
                    0, 0, video_width, video_height, bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
                if (rows != 0 && rows != GDI_ERROR && painted_frame != received_frames) {
                    painted_frame = received_frames;
                    ++presented_frames;
                } else if (rows == 0 || rows == GDI_ERROR) {
                    ++paint_failures;
                }
            }
        }
        EndPaint(window, &paint);
    }

    LRESULT OnWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_DESTROY:
            window = nullptr;
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_KILLFOCUS:
            if (!host && !check) SendInput({InputKind::reset});
            break;
        case WM_MOUSEMOVE:
            if (!host && (!check || generating_inputs)) {
                Pointer(InputKind::move, 0, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            } else if (host && check && dispatched_move) {
                mouse_moved = true;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            if (!host && (!check || generating_inputs)) {
                const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
                const auto button = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ? left_button :
                    message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ? right_button : middle_button;
                Pointer(down ? InputKind::button_down : InputKind::button_up, button,
                    {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                if (!check && down) SetCapture(hwnd);
                if (!check && !down && !(wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) && GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                return 0;
            }
            if (host && check && mouse_moved && dispatched_click && message == WM_LBUTTONUP && phase == 0) {
                ++phase;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSEWHEEL:
            if (!host && !check) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd, &point);
                Pointer(InputKind::wheel, static_cast<std::uint16_t>(GET_WHEEL_DELTA_WPARAM(wparam)), point);
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wparam == VK_F12 && message == WM_KEYDOWN && !check) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (!host && (!check || generating_inputs)) {
                const auto scan = static_cast<std::uint16_t>((lparam >> 16) & 0x1ff);
                if (scan) SendInput({message == WM_KEYDOWN || message == WM_SYSKEYDOWN ?
                    InputKind::key_down : InputKind::key_up, scan});
                return 0;
            }
            if (host && check && dispatched_key && message == WM_KEYUP && wparam == VK_F8 && phase == 1) {
                ++phase;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool CheckInput(const InputPacket& packet, std::uint16_t test_key) {
        if (packet.kind == InputKind::key_down || packet.kind == InputKind::key_up) {
            if (phase != 1 || packet.code != test_key || GetForegroundWindow() != window) return false;
            dispatched_key = true;
            return true;
        }
        if (packet.kind != InputKind::move &&
            !((packet.kind == InputKind::button_down || packet.kind == InputKind::button_up) &&
                packet.code == left_button)) return false;
        const POINT point{bounds.left + MulDiv(packet.x, bounds.right - bounds.left - 1, 65535),
                          bounds.top + MulDiv(packet.y, bounds.bottom - bounds.top - 1, 65535)};
        if (phase != 0 || !PtInRect(&target, point) || WindowFromPoint(point) != window) return false;
        dispatched_move |= packet.kind == InputKind::move;
        dispatched_click |= packet.kind == InputKind::button_up;
        return true;
    }

    void ApplyInputs(p2premote::DesktopInput& input) {
        std::deque<InputPacket> incoming;
        {
            std::lock_guard lock(mutex);
            incoming.swap(inputs);
        }
        for (const auto& packet : incoming) {
            const auto accepted = input_state.Accept(packet, GetTickCount64());
            if (accepted && (!check || CheckInput(*accepted, static_cast<std::uint16_t>(MapVirtualKeyW(VK_F8, MAPVK_VK_TO_VSC))))) {
                input.Submit(*accepted);
                ++inputs_applied;
            }
        }
    }

    void Present() {
        Frame frame;
        {
            std::lock_guard lock(mutex);
            std::swap(frame, pending);
        }
        if (frame.nv12.empty()) return;
        video_width = frame.width;
        video_height = frame.height;
        const auto luma = static_cast<size_t>(video_width) * video_height;
        bgra.resize(luma * 4);
        if (libyuv::NV12ToARGB(frame.nv12.data(), video_width, frame.nv12.data() + luma, video_width,
                bgra.data(), video_width * 4, video_width, video_height) != 0) {
            throw std::runtime_error("cannot convert decoded desktop video");
        }
        ++received_frames;
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
    }

    void VerifyPaint() {
        if (!presented_frames || phase == marker.size()) return;
        const HDC dc = GetDC(window);
        if (!dc) return;
        const auto x = viewport.left + viewport.width / 2;
        const auto y = viewport.top + viewport.height / 2;
        const COLORREF color = GetPixel(dc, x, y);
        ReleaseDC(window, dc);
        const auto expected = marker[phase];
        if (color == CLR_INVALID || std::abs(GetRValue(color) - GetRValue(expected)) > 45 ||
            std::abs(GetGValue(color) - GetGValue(expected)) > 45 ||
            std::abs(GetBValue(color) - GetBValue(expected)) > 45) return;
        generating_inputs = true;
        if (phase == 0) {
            const auto point = MAKELPARAM(x, y);
            SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x - 20, y));
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
            SendMessageW(window, WM_LBUTTONUP, 0, point);
        } else if (phase == 1) {
            const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(VK_F8, MAPVK_VK_TO_VSC)) << 16;
            SendMessageW(window, WM_KEYDOWN, VK_F8, scan);
            SendMessageW(window, WM_KEYUP, VK_F8, scan);
        }
        generating_inputs = false;
        ++phase;
    }

    bool Run(PeerPtr* live_peer, std::chrono::seconds duration, const std::function<bool()>& ended) {
        peer = live_peer;
        OpenWindow();
        ScreenCapture capture;
        std::unique_ptr<p2premote::DesktopInput> input;
        if (host) {
            capture.Open(bounds);
            const auto size = capture.Size();
            video_width = size.cx;
            video_height = size.cy;
            input = std::make_unique<p2premote::DesktopInput>(monitor);
        }
        auto next = Clock::now();
        const auto deadline = next + duration;
        auto heartbeat = next;
        while (window && Clock::now() < deadline && !ended() && !input_failed) {
            MSG message{};
            while (window && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!window) break;
            const auto now = Clock::now();
            if (host) {
                ApplyInputs(*input);
                if (now >= next) {
                    capture.Send(peer);
                    ++captured_frames;
                    next = now + 50ms;
                }
            } else {
                Present();
                if (check) VerifyPaint();
                else if (now >= heartbeat) {
                    SendInput({InputKind::heartbeat});
                    heartbeat = now + std::chrono::milliseconds(p2premote::input_heartbeat_ms);
                }
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        const bool mouse = check && (host ? phase >= 1 : phase >= 2);
        const bool keyboard = check && (host ? phase >= 2 : phase >= 3);
        const bool pass = !input_failed && (host ? captured_frames > 0 : presented_frames > 0) &&
            (!check || (mouse && keyboard));
        std::cout << "DESKTOP=" << (pass ? "pass" : "fail") << " role=" << (host ? "host" : "viewer")
            << " captured=" << captured_frames << " decoded=" << received_frames << " presented=" << presented_frames
            << " width=" << video_width << " height=" << video_height << " input_sent=" << inputs_sent
            << " input_applied=" << inputs_applied << " mouse_verified=" << mouse << " keyboard_verified=" << keyboard
            << " marker_stage=" << phase << " window_visible=" << !!IsWindowVisible(window)
            << " paint_calls=" << paint_calls << " paint_failures=" << paint_failures << std::endl;
        return pass;
    }

    const bool host, check;
    HWND window{};
    HMONITOR monitor{};
    RECT bounds{}, target{};
    PeerPtr* peer{};
    p2premote::VideoViewport viewport{};
    p2premote::InputReceiveState input_state;
    std::mutex mutex;
    std::deque<InputPacket> inputs;
    Frame pending;
    std::vector<std::uint8_t> bgra;
    int video_width{}, video_height{};
    std::uint32_t sequence{};
    unsigned phase{}, captured_frames{}, received_frames{}, presented_frames{}, painted_frame{}, paint_calls{}, paint_failures{};
    unsigned inputs_sent{}, inputs_applied{};
    bool dispatched_move{}, dispatched_click{}, dispatched_key{}, mouse_moved{}, generating_inputs{}, input_failed{};
};

DesktopProbe::DesktopProbe(bool host, bool check) : session_(std::make_unique<Session>(host, check)) {}
DesktopProbe::~DesktopProbe() = default;

void DesktopProbe::ReceiveInput(const char* data, size_t size) {
    if (!session_->host) return;
    const auto input = p2premote::DecodeInput({reinterpret_cast<const std::uint8_t*>(data), size});
    if (!input) return;
    std::lock_guard lock(session_->mutex);
    if (session_->inputs.size() >= 128) {
        session_->inputs.clear();
        auto reset = *input;
        reset.kind = InputKind::reset;
        session_->inputs.push_back(reset);
    } else {
        session_->inputs.push_back(*input);
    }
}

void DesktopProbe::ReceiveVideo(const MiniRtcVideoFrame& frame) {
    if (session_->host || !frame.data || !frame.width || !frame.height || (frame.width | frame.height) & 1u ||
        frame.width > 4096 || frame.height > 2160) return;
    const auto size = static_cast<size_t>(frame.width) * frame.height * 3 / 2;
    if (frame.size < size) return;
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(frame.data);
    std::lock_guard lock(session_->mutex);
    session_->pending.width = frame.width;
    session_->pending.height = frame.height;
    session_->pending.nv12.assign(pixels, pixels + size);
}

bool DesktopProbe::Run(PeerPtr* peer, std::chrono::seconds duration, const std::function<bool()>& ended) {
    return session_->Run(peer, duration, ended);
}

} // namespace desktop_probe
