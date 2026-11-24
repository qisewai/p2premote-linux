#pragma once

#include <cstdint>
#include <optional>

namespace p2premote {

class DxgiCursorCheck {
public:
    bool Observe(bool possibly_embedded, std::uint64_t now_ms) {
        if (!possibly_embedded) {
            first_.reset();
            return false;
        }
        // DXGI and GetCursorInfo can describe different moments during a cursor transition.
        if (!first_ || now_ms - last_ > 200) first_ = now_ms;
        last_ = now_ms;
        return now_ms - *first_ >= 64;
    }

private:
    std::optional<std::uint64_t> first_;
    std::uint64_t last_{};
};

} // namespace p2premote
