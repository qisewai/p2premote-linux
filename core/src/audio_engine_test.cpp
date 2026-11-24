#include "audio_engine.h"

#include <iostream>
#include <vector>

using namespace p2premote;

int main() {
    const auto check = [](bool ok, const char* msg) {
        if (!ok) std::cerr << msg << '\n';
        return ok;
    };

    AudioRingBuffer ring(audio_frame_samples * 6, audio_frame_samples * 2, audio_frame_samples * 3);
    std::vector<std::int16_t> frame1(audio_frame_samples, 1000);
    std::vector<std::int16_t> frame2(audio_frame_samples, 2000);
    std::vector<std::int16_t> out(audio_frame_samples, 999);

    bool passed = true;
    // Before prebuffer (2 frames), Pop must emit silence and return 0.
    ring.Push(frame1.data(), frame1.size());
    passed &= check(ring.Pop(out.data(), out.size(), 1.0f) == 0 && out[0] == 0,
        "Pop did not wait for prebuffer");

    // Second frame primes the buffer; Pop with 0.5 volume scales accurately.
    ring.Push(frame2.data(), frame2.size());
    passed &= check(ring.Pop(out.data(), out.size(), 0.5f) == audio_frame_samples && out[0] == 500,
        "Volume scaling failed");
    passed &= check(ring.Pop(out.data(), out.size(), 1.0f) == audio_frame_samples && out[0] == 2000,
        "Second frame mismatch");

    // Overflow drops oldest samples down to trim target so latency stays bounded.
    for (int i = 1; i <= 11; ++i) {
        std::vector<std::int16_t> f(audio_frame_samples, static_cast<std::int16_t>(i * 100));
        ring.Push(f.data(), f.size());
    }
    passed &= check(ring.Size() == audio_frame_samples * 3, "Ring buffer did not trim to target on overflow");
    passed &= check(ring.Pop(out.data(), out.size(), 1.0f) == audio_frame_samples && out[0] == 900,
        "Ring buffer did not drop oldest frames on overflow");

    ring.Clear();
    passed &= check(ring.Size() == 0, "Clear did not empty ring buffer");
    return passed ? 0 : 1;
}
