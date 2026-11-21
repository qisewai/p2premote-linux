#include "preview_stats.h"

#include "qpc.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <stdexcept>
#include <vector>

namespace p2premote {
namespace {

constexpr std::size_t rolling_sample_count = 300;
constexpr std::size_t csv_sample_limit = 262'144;

template <typename Delta>
Percentiles RecentPercentiles(const std::deque<PresentedFrameTiming>& frames, Delta delta) {
    const auto count = std::min(frames.size(), rolling_sample_count);
    if (count == 0) {
        return {};
    }
    std::vector<std::int64_t> samples;
    samples.reserve(count);
    const auto first = frames.end() - static_cast<std::ptrdiff_t>(count);
    std::transform(first, frames.end(), std::back_inserter(samples), delta);
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples, count](unsigned percent) {
        const auto index = (count * percent + 99) / 100 - 1;
        return QpcMilliseconds(samples[index]);
    };
    return {percentile(50), percentile(95)};
}

} // namespace

void PreviewStats::Start() noexcept {
    counters_ = {};
    frames_.clear();
    started_qpc_ = QpcNow();
    stopped_qpc_ = 0;
}

void PreviewStats::Stop() noexcept {
    stopped_qpc_ = QpcNow();
    counters_.queue_depth = 0;
}

void PreviewStats::Captured(unsigned acquired, unsigned dropped, unsigned width, unsigned height, bool pending) noexcept {
    counters_.captured += acquired;
    counters_.dropped += dropped;
    counters_.source_width = width;
    counters_.source_height = height;
    counters_.queue_depth = pending ? 1 : 0;
    counters_.max_queue_depth = std::max(counters_.max_queue_depth, counters_.queue_depth);
}

void PreviewStats::Presented(PresentedFrameTiming frame) {
    frame.capture_count = counters_.captured;
    frame.present_count = counters_.presented + 1;
    frame.drop_count = counters_.dropped;
    frames_.push_back(frame);
    ++counters_.presented;
    if (frames_.size() > csv_sample_limit) {
        frames_.pop_front();
    }
}

PreviewStatistics PreviewStats::Snapshot() const {
    auto result = counters_;
    if (started_qpc_) {
        const auto end_qpc = stopped_qpc_ ? stopped_qpc_ : QpcNow();
        result.elapsed_seconds = QpcMilliseconds(end_qpc - started_qpc_) / 1000.0;
    }
    result.capture_to_ready = RecentPercentiles(frames_, [](const auto& frame) {
        return frame.ready_qpc - frame.metadata.capture_qpc;
    });
    result.ready_to_draw = RecentPercentiles(frames_, [](const auto& frame) {
        return frame.draw_begin_qpc - frame.ready_qpc;
    });
    result.draw_to_submit = RecentPercentiles(frames_, [](const auto& frame) {
        return frame.submit_qpc - frame.draw_begin_qpc;
    });
    result.present_call = RecentPercentiles(frames_, [](const auto& frame) {
        return frame.present_return_qpc - frame.submit_qpc;
    });
    result.capture_to_present_return = RecentPercentiles(frames_, [](const auto& frame) {
        return frame.present_return_qpc - frame.metadata.capture_qpc;
    });
    return result;
}

void PreviewStats::ExportCsv(const std::filesystem::path& path) const {
    std::ofstream csv(path, std::ios::binary);
    if (!csv) {
        throw std::runtime_error("Unable to open the timing CSV file.");
    }
    csv.imbue(std::locale::classic());
    csv << "frame_id,width,height,pixel_format,color_space,qpc_frequency,source_qpc,capture_qpc,capture_ready_qpc,"
           "draw_begin_qpc,submit_qpc,present_return_qpc,capture_to_ready_ms,ready_to_draw_ms,"
           "draw_to_submit_ms,present_call_ms,capture_to_present_return_ms,"
           "capture_count,present_count,drop_count,queue_depth\r\n" << std::fixed << std::setprecision(3);
    const auto frequency = QpcFrequency();
    for (const auto& frame : frames_) {
        const auto& metadata = frame.metadata;
        csv << metadata.frame_id << ',' << metadata.width << ',' << metadata.height
            << ",BGRA8,SDR_RGB_FULL," << frequency << ',' << metadata.source_qpc << ',' << metadata.capture_qpc << ',' << frame.ready_qpc
            << ',' << frame.draw_begin_qpc << ',' << frame.submit_qpc << ',' << frame.present_return_qpc
            << ',' << QpcMilliseconds(frame.ready_qpc - metadata.capture_qpc)
            << ',' << QpcMilliseconds(frame.draw_begin_qpc - frame.ready_qpc)
            << ',' << QpcMilliseconds(frame.submit_qpc - frame.draw_begin_qpc)
            << ',' << QpcMilliseconds(frame.present_return_qpc - frame.submit_qpc)
            << ',' << QpcMilliseconds(frame.present_return_qpc - metadata.capture_qpc)
            << ',' << frame.capture_count << ',' << frame.present_count << ',' << frame.drop_count
            << ',' << frame.queue_depth << "\r\n";
    }
    csv.close();
    if (!csv) {
        throw std::runtime_error("Unable to finish writing the timing CSV file.");
    }
}

} // namespace p2premote
