#pragma once

#include <windows.h>

#include <cstdint>

namespace p2premote {

inline std::int64_t QpcNow() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline std::int64_t QpcFrequency() noexcept {
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

inline std::int64_t ToQpc(std::int64_t ticks_100ns) noexcept {
    constexpr std::int64_t ticks_per_second = 10'000'000;
    const auto frequency = QpcFrequency();
    return (ticks_100ns / ticks_per_second) * frequency
        + (ticks_100ns % ticks_per_second) * frequency / ticks_per_second;
}

inline double QpcMilliseconds(std::int64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(QpcFrequency());
}

} // namespace p2premote
