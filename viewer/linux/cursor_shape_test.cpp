#include "cursor_shape.h"

#include <cassert>
#include <string>

namespace {

void write16(std::string& data, std::size_t offset, unsigned value) {
    data[offset] = static_cast<char>(value & 0xffu);
    data[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
}

std::string makeHeader(unsigned width, unsigned height, unsigned hot_x, unsigned hot_y, std::size_t payload_size) {
    std::string data(9 + payload_size, '\0');
    data[0] = linux_cursor_marker;
    write16(data, 1, width);
    write16(data, 3, height);
    write16(data, 5, hot_x);
    write16(data, 7, hot_y);
    return data;
}

} // namespace

int main() {
    // Reject empty or wrong marker.
    assert(!decodeRemoteCursor("").has_value());
    assert(!decodeRemoteCursor("X").has_value());

    // Hidden cursor ('C').
    {
        const auto hidden = decodeRemoteCursor("C");
        assert(hidden.has_value());
        assert(!hidden->visible);
        assert(hidden->image.isNull());
    }

    // Stock arrow fallback ('CA').
    {
        const auto arrow = decodeRemoteCursor("CA");
        assert(arrow.has_value());
        assert(arrow->visible);
        assert(arrow->image.isNull());
    }

    // 32-bit ARGB color cursor (2x2).
    {
        const std::size_t stride = 4;
        const std::size_t mask_bytes = stride * 2;
        const std::size_t color_bytes = 2 * 2 * 4;
        auto packet = makeHeader(2, 2, 1, 0, mask_bytes + color_bytes);
        // Pixel (0,0): B=10, G=20, R=30, A=255.
        const std::size_t base = 9 + mask_bytes;
        packet[base + 0] = 10;
        packet[base + 1] = 20;
        packet[base + 2] = 30;
        packet[base + 3] = static_cast<char>(255);
        // Pixel (1,0): transparent.
        const auto cursor = decodeRemoteCursor(packet);
        assert(cursor.has_value() && cursor->visible && !cursor->image.isNull());
        assert(cursor->hot_x == 1 && cursor->hot_y == 0);
        assert(cursor->image.pixel(0, 0) == qRgba(30, 20, 10, 255));
        assert(cursor->image.pixel(1, 0) == qRgba(0, 0, 0, 0));
    }

    // Legacy 32-bit RGB cursor with zero alpha and AND mask (2x1).
    {
        const std::size_t stride = 4;
        const std::size_t mask_bytes = stride * 1;
        const std::size_t color_bytes = 2 * 1 * 4;
        auto packet = makeHeader(2, 1, 0, 0, mask_bytes + color_bytes);
        // AND mask: pixel 0 opaque (bit 7 = 0), pixel 1 transparent (bit 6 = 1).
        packet[9] = static_cast<char>(0x40);
        const std::size_t base = 9 + mask_bytes;
        packet[base + 0] = 40;
        packet[base + 1] = 50;
        packet[base + 2] = 60;
        packet[base + 3] = 0;
        const auto cursor = decodeRemoteCursor(packet);
        assert(cursor.has_value() && cursor->visible && !cursor->image.isNull());
        assert(cursor->image.pixel(0, 0) == qRgba(60, 50, 40, 255));
        assert(cursor->image.pixel(1, 0) == qRgba(0, 0, 0, 0));
    }

    // Monochrome cursor with XOR pixel (3x1: transparent, XOR, transparent).
    {
        const std::size_t stride = 4;
        auto packet = makeHeader(3, 1, 1, 0, stride * 2);
        // AND mask: all 3 pixels have AND=1 (bits 7,6,5 = 0xE0).
        packet[9] = static_cast<char>(0xE0);
        // XOR mask: pixel 1 has XOR=1 (bit 6 = 0x40).
        packet[9 + stride] = static_cast<char>(0x40);
        const auto cursor = decodeRemoteCursor(packet);
        assert(cursor.has_value() && cursor->visible && !cursor->image.isNull());
        assert(cursor->hot_x == 1 && cursor->hot_y == 0);
        // Center XOR pixel is black; adjacent transparent pixels get white halo.
        assert(cursor->image.pixel(1, 0) == qRgba(0, 0, 0, 255));
        assert(cursor->image.pixel(0, 0) == qRgba(255, 255, 255, 255));
        assert(cursor->image.pixel(2, 0) == qRgba(255, 255, 255, 255));
    }

    return 0;
}
