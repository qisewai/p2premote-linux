#include "cursor_shape.h"

#include <cstdint>
#include <cstring>

namespace p2premote {
namespace {

constexpr unsigned header_size = 9;

struct IconBitmaps {
    ICONINFO info{};
    ~IconBitmaps() {
        if (info.hbmMask) DeleteObject(info.hbmMask);
        if (info.hbmColor) DeleteObject(info.hbmColor);
    }
};

struct DibInfo {
    BITMAPINFOHEADER header{};
    RGBQUAD palette[2]{{0, 0, 0, 0}, {255, 255, 255, 0}};

    DibInfo(unsigned width, unsigned height, WORD bits) {
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(width);
        header.biHeight = -static_cast<LONG>(height);
        header.biPlanes = 1;
        header.biBitCount = bits;
        header.biCompression = BI_RGB;
    }
    BITMAPINFO* data() { return reinterpret_cast<BITMAPINFO*>(this); }
};

unsigned MaskBytes(unsigned width, unsigned height) { return ((width + 31) / 32) * 4 * height; }

void Write16(std::string& data, unsigned offset, unsigned value) {
    data[offset] = static_cast<char>(value & 255);
    data[offset + 1] = static_cast<char>(value >> 8);
}

unsigned Read16(std::string_view data, unsigned offset) {
    return static_cast<unsigned char>(data[offset]) | (static_cast<unsigned char>(data[offset + 1]) << 8);
}

HBITMAP BitmapFromBytes(unsigned width, unsigned height, WORD bits, std::string_view data) {
    DibInfo info(width, height, bits);
    void* memory{};
    const auto bitmap = CreateDIBSection(nullptr, info.data(), DIB_RGB_COLORS, &memory, nullptr, 0);
    if (bitmap) std::memcpy(memory, data.data(), data.size());
    return bitmap;
}

} // namespace

std::string EncodeCursor(HCURSOR cursor) {
    IconBitmaps bitmaps;
    auto& icon = bitmaps.info;
    if (!GetIconInfo(reinterpret_cast<HICON>(cursor), &icon)) return {};
    const bool color = icon.hbmColor != nullptr;
    BITMAP mask{};
    BITMAP color_bitmap{};
    if (!GetObjectW(icon.hbmMask, sizeof(mask), &mask) || mask.bmWidth <= 0 || mask.bmHeight <= 0
            || (!color && mask.bmHeight % 2)) return {};
    const auto width = static_cast<unsigned>(mask.bmWidth);
    const auto height = static_cast<unsigned>(mask.bmHeight) / (color ? 1 : 2);
    if (!height || width > cursor_max_dimension || height > cursor_max_dimension
            || icon.xHotspot >= width || icon.yHotspot >= height) return {};
    const auto mask_height = static_cast<unsigned>(mask.bmHeight);
    if (color && (!GetObjectW(icon.hbmColor, sizeof(color_bitmap), &color_bitmap)
            || color_bitmap.bmWidth != mask.bmWidth || color_bitmap.bmHeight != mask.bmHeight)) return {};
    const auto mask_bytes = MaskBytes(width, mask_height);
    std::string data(header_size + mask_bytes + (color ? width * height * 4 : 0), '\0');
    data[0] = cursor_marker;
    Write16(data, 1, width);
    Write16(data, 3, height);
    Write16(data, 5, icon.xHotspot);
    Write16(data, 7, icon.yHotspot);

    const auto dc = GetDC(nullptr);
    if (!dc) return {};
    DibInfo mask_info(width, mask_height, 1);
    DibInfo color_info(width, height, 32);
    const auto mask_read = GetDIBits(dc, icon.hbmMask, 0, mask_height,
        data.data() + header_size, mask_info.data(), DIB_RGB_COLORS);
    const auto color_read = color ? GetDIBits(dc, icon.hbmColor, 0, height,
        data.data() + header_size + mask_bytes, color_info.data(), DIB_RGB_COLORS) : 0;
    ReleaseDC(nullptr, dc);
    if (mask_read != static_cast<int>(mask_height) || (color && color_read != static_cast<int>(height))) return {};
    return data;
}

HCURSOR DecodeCursor(std::string_view data) {
    if (data.size() < header_size || data[0] != cursor_marker) return nullptr;
    const auto width = Read16(data, 1);
    const auto height = Read16(data, 3);
    const auto x = Read16(data, 5);
    const auto y = Read16(data, 7);
    if (!width || !height || width > cursor_max_dimension || height > cursor_max_dimension || x >= width || y >= height) {
        return nullptr;
    }
    const auto color_mask = MaskBytes(width, height);
    const auto pixels = width * height * 4;
    const bool color = data.size() == header_size + color_mask + pixels;
    const auto mask_height = height * (color ? 1 : 2);
    const auto mask_bytes = MaskBytes(width, mask_height);
    if (!color && data.size() != header_size + mask_bytes) return nullptr;

    IconBitmaps bitmaps;
    auto& icon = bitmaps.info;
    icon.xHotspot = x;
    icon.yHotspot = y;
    icon.hbmMask = BitmapFromBytes(width, mask_height, 1, data.substr(header_size, mask_bytes));
    if (color) {
        icon.hbmColor = BitmapFromBytes(width, height, 32, data.substr(header_size + mask_bytes));
        if (!icon.hbmColor) return nullptr;
    }
    return icon.hbmMask ? reinterpret_cast<HCURSOR>(CreateIconIndirect(&icon)) : nullptr;
}

void DestroyDecodedCursor(HCURSOR cursor) noexcept {
    if (!cursor) return;
    if (GetCursor() == cursor) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    DestroyCursor(cursor);
}

} // namespace p2premote
