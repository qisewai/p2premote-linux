#pragma once

#include <QImage>

#include <optional>
#include <string_view>

struct DecodedCursor {
    bool visible{true};
    QImage image;
    int hot_x{};
    int hot_y{};
};

inline constexpr char linux_cursor_marker = 'C';
inline constexpr unsigned linux_cursor_max_dimension = 128;
inline constexpr unsigned linux_cursor_max_packet = 9 + linux_cursor_max_dimension * (4 * linux_cursor_max_dimension + 16);

std::optional<DecodedCursor> decodeRemoteCursor(std::string_view packet);
