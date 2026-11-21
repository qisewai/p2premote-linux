#pragma once

#include <p2premote/input_protocol.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace p2premote {

class LatencyTrace {
public:
    LatencyTrace(const std::filesystem::path& directory, bool host);
    ~LatencyTrace();

    void Record(const char* event, std::uint64_t id, std::uint64_t aux = 0,
        std::uint64_t size = 0, unsigned flags = 0, std::int64_t qpc = 0);
    void Pointer(const char* event, const InputPacket& input);

private:
    struct Entry {
        std::int64_t qpc;
        const char* event;
        std::uint64_t id;
        std::uint64_t aux;
        std::uint64_t size;
        unsigned flags;
    };

    std::mutex mutex_;
    std::ofstream output_;
    std::vector<Entry> entries_;
    std::size_t omitted_{};
};

} // namespace p2premote
