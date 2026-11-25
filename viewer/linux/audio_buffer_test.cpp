#include "audio_buffer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__,    \
                #cond);                                                        \
            return 1;                                                          \
        }                                                                      \
    } while (false)

int main() {
    // Prebuffering: pop returns silence until prebuffer threshold is reached.
    {
        PcmRingBuffer ring(480 * 8, 480 * 3, 480 * 4);
        std::vector<std::int16_t> frame(480, 10000);
        std::vector<std::int16_t> out(480, 123);

        ring.push(frame.data(), frame.size());
        ring.push(frame.data(), frame.size());
        CHECK(ring.pop(out.data(), out.size(), 1.0f) == 0);
        CHECK(out[0] == 0 && out[479] == 0);

        // 3rd frame triggers prebuffer priming + smooth fade-in.
        ring.push(frame.data(), frame.size());
        CHECK(ring.pop(out.data(), out.size(), 1.0f) == 480);
        CHECK(out[0] > 0 && out[0] < 1000);
        CHECK(out[96] == 10000);
        CHECK(out[479] == 10000);
    }

    // Partial pull (PipeWire 1024-sample quantum > 480-sample frame) drains smoothly
    // and fades down to silence on underrun without a hard step.
    {
        PcmRingBuffer ring(480 * 8, 480 * 2, 480 * 4);
        std::vector<std::int16_t> frame(480, 12000);
        ring.push(frame.data(), frame.size());
        ring.push(frame.data(), frame.size()); // 960 samples primed

        std::vector<std::int16_t> out(1024, 0);
        const auto taken = ring.pop(out.data(), out.size(), 0.5f);
        CHECK(taken == 960);
        CHECK(out[200] == 6000);
        // Tail fade from 6000 down toward 0 across samples 960..1023.
        CHECK(out[960] < 6000 && out[960] > 0);
        CHECK(out[1023] < out[960]);
    }

    // Overflow trims cleanly to target without unbounded growth.
    {
        PcmRingBuffer ring(480 * 8, 480 * 2, 480 * 4);
        std::vector<std::int16_t> frame(480, 4000);
        for (int i = 0; i < 20; ++i) ring.push(frame.data(), frame.size());
        CHECK(ring.size() <= 480 * 8);
        CHECK(ring.size() >= 480 * 4);
    }

    return 0;
}
