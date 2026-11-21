#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace p2premote {

inline constexpr char cursor_marker = 'C';
inline constexpr unsigned cursor_max_dimension = 128;
inline constexpr unsigned cursor_max_packet = 9 + cursor_max_dimension * (4 * cursor_max_dimension + 16);

// A marker alone means the cursor is hidden. A visible cursor carries the
// dimensions, hotspot and DIB bitmaps. An empty result means use a stock arrow.
std::string EncodeCursor(HCURSOR cursor);
HCURSOR DecodeCursor(std::string_view packet);
void DestroyDecodedCursor(HCURSOR cursor) noexcept;

} // namespace p2premote
