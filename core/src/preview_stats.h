#pragma once

#include <p2premote/preview_session.h>

#include <deque>

namespace p2premote {

struct PresentedFrameTiming {
    FrameMetadata metadata;
    std::int64_t ready_qpc{};
    std::int64_t draw_begin_qpc{};
    std::int64_t submit_qpc{};
    std::int64_t present_return_qpc{};
    std::uint64_t capture_count{};
    std::uint64_t present_count{};
    std::uint64_t drop_count{};
    unsigned queue_depth{};
};

class PreviewStats {
public:
    void Start() noexcept;
    void Stop() noexcept;
    void Captured(unsigned acquired, unsigned dropped, unsigned width, unsigned height, bool pending) noexcept;
    void Presented(PresentedFrameTiming frame);
    void Dequeued() noexcept { counters_.queue_depth = 0; }
    void Dropped() noexcept { ++counters_.dropped; }
    [[nodiscard]] PreviewStatistics Snapshot() const;
    void ExportCsv(const std::filesystem::path& path) const;

private:
    PreviewStatistics counters_{};
    std::deque<PresentedFrameTiming> frames_;
    std::int64_t started_qpc_{};
    std::int64_t stopped_qpc_{};
};

} // namespace p2premote
