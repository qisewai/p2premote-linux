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

namespace {

std::vector<std::int16_t> makeStereoFrame(std::int16_t left, std::int16_t right) {
    std::vector<std::int16_t> frame(PcmRingBuffer::frame_samples);
    for (std::size_t f = 0; f < PcmRingBuffer::frame_samples_per_channel; ++f) {
        frame[f * 2] = left;
        frame[f * 2 + 1] = right;
    }
    return frame;
}

} // namespace

int main() {
    constexpr std::size_t frame_n = PcmRingBuffer::frame_samples; // 960 stereo samples (480 frames)

    // Prebuffering: pop returns silence until prebuffer threshold is reached,
    // then fades in each stereo channel independently without swapping L/R.
    {
        PcmRingBuffer ring(frame_n * 8, frame_n * 3, frame_n * 4);
        const auto frame = makeStereoFrame(10000, -6000);
        std::vector<std::int16_t> out(frame_n, 123);

        ring.push(frame.data(), frame.size());
        ring.push(frame.data(), frame.size());
        CHECK(ring.pop(out.data(), out.size(), 1.0f) == 0);
        CHECK(out[0] == 0 && out[frame_n - 1] == 0);

        ring.push(frame.data(), frame.size());
        CHECK(ring.pop(out.data(), out.size(), 1.0f) == frame_n);
        CHECK(out[0] > 0 && out[0] < 1000);
        CHECK(out[1] < 0 && out[1] > -1000);
        CHECK(out[PcmRingBuffer::fade_frames * 2] == 10000);
        CHECK(out[PcmRingBuffer::fade_frames * 2 + 1] == -6000);
        CHECK(out[frame_n - 2] == 10000);
        CHECK(out[frame_n - 1] == -6000);
    }

    // Partial pull (e.g. PipeWire 1024-frame stereo quantum = 2048 samples > 2 frames = 1920 samples)
    // drains smoothly and fades both channels down toward 0 on underrun.
    {
        PcmRingBuffer ring(frame_n * 8, frame_n * 2, frame_n * 4);
        const auto frame = makeStereoFrame(12000, -8000);
        ring.push(frame.data(), frame.size());
        ring.push(frame.data(), frame.size()); // 1920 samples primed

        std::vector<std::int16_t> out(2048, 0);
        const auto taken = ring.pop(out.data(), out.size(), 0.5f);
        CHECK(taken == 1920);
        CHECK(out[400] == 6000);
        CHECK(out[401] == -4000);
        // Tail fade from (+6000, -4000) toward (0, 0).
        CHECK(out[1920] < 6000 && out[1920] > 0);
        CHECK(out[1921] > -4000 && out[1921] < 0);
        CHECK(out[2046] < out[1920]);
        CHECK(out[2047] > out[1921]);
    }

    // Overflow trims cleanly to channel-aligned target without swapping L/R.
    {
        PcmRingBuffer ring(frame_n * 8, frame_n * 2, frame_n * 4);
        const auto frame = makeStereoFrame(5000, -3000);
        for (int i = 0; i < 20; ++i) ring.push(frame.data(), frame.size());
        CHECK(ring.size() <= frame_n * 8);
        CHECK(ring.size() >= frame_n * 4);
        CHECK((ring.size() % PcmRingBuffer::channels) == 0);

        std::vector<std::int16_t> out(frame_n, 0);
        CHECK(ring.pop(out.data(), out.size(), 1.0f) == frame_n);
        CHECK(out[frame_n - 2] == 5000);
        CHECK(out[frame_n - 1] == -3000);
    }

    return 0;
}
