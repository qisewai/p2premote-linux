#pragma once

#include <p2premote/input_protocol.h>

#include <windows.h>

#include <bitset>
#include <optional>
#include <string>

class InputTestPad {
public:
    ~InputTestPad();

    HWND Create(HWND parent, HINSTANCE instance);
    void BeginSession(HMONITOR monitor);
    void SetReady(bool ready);
    void Apply(const p2premote::InputPacket& input);
    void Expire();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void Paint() const;
    void Redraw();
    void Release();
    RECT Board() const;
    std::optional<POINT> Pointer(const p2premote::InputPacket& input) const;

    HWND window_{};
    HMONITOR monitor_{};
    bool buffered_{};
    bool ready_{};
    std::bitset<512> keys_;
    std::bitset<4> buttons_;
    std::optional<POINT> pointer_;
    std::optional<POINT> marker_;
    std::wstring text_;
    unsigned clicks_{};
    long long wheel_{};
    unsigned last_key_{};
    std::uint64_t last_reliable_{};
};
