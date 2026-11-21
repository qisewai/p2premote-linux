#include "latency_trace.h"

#include "qpc.h"

#include <locale>
#include <stdexcept>

namespace p2premote {
namespace {
constexpr std::size_t trace_limit = 131'072;
}

LatencyTrace::LatencyTrace(const std::filesystem::path& directory, bool host) {
    std::filesystem::create_directories(directory);
    auto name = std::string(host ? "host-" : "viewer-") + std::to_string(GetCurrentProcessId()) + '-'
        + std::to_string(QpcNow()) + ".csv";
    output_.open(directory / name, std::ios::binary);
    if (!output_) throw std::runtime_error("无法创建延迟诊断文件");
    output_.imbue(std::locale::classic());
    entries_.reserve(trace_limit);
}

LatencyTrace::~LatencyTrace() {
    output_ << "time_us,event,id,aux,bytes,flags\n";
    for (const auto& event : entries_) {
        output_ << static_cast<std::int64_t>(QpcMilliseconds(event.qpc) * 1000) << ',' << event.event << ','
            << event.id << ',' << event.aux << ',' << event.size << ',' << event.flags << '\n';
    }
    if (omitted_) output_ << "0,trace_omitted," << omitted_ << ",0,0,0\n";
}

void LatencyTrace::Record(const char* event, std::uint64_t id, std::uint64_t aux, std::uint64_t size, unsigned flags,
    std::int64_t qpc) {
    if (!qpc) qpc = QpcNow();
    const std::lock_guard lock(mutex_);
    if (entries_.size() < trace_limit) entries_.push_back({qpc, event, id, aux, size, flags});
    else ++omitted_;
}

void LatencyTrace::Pointer(const char* event, const InputPacket& input) {
    constexpr auto left = static_cast<std::uint16_t>(InputButton::left);
    if (input.kind == InputKind::move || (input.code == left &&
            (input.kind == InputKind::button_down || input.kind == InputKind::button_up))) {
        Record(event, input.sequence, static_cast<std::uint8_t>(input.kind));
    }
}

} // namespace p2premote
