#include "stream_events.h"

#include <iostream>

int main() {
    using namespace p2premote;
    StreamEvents events;
    const auto check = [](bool ok, const char* message) {
        if (!ok) std::cerr << message << '\n';
        return ok;
    };

    events.Push({StreamEventType::cursor, {}, "old"});
    events.Push({StreamEventType::input_ready, {}, {}});
    events.Push({StreamEventType::cursor, {}, "new"});
    auto batch = events.Drain();
    if (!check(batch.size() == 2 && batch[0].type == StreamEventType::input_ready && batch[1].payload == "new",
            "Coalescing moved the new cursor before a pending state transition")) return 1;

    for (int i = 0; i < 256; ++i) events.Push({StreamEventType::minirtc_signal, {}, {}});
    events.Push({StreamEventType::stats, {}, {}});
    events.Push({StreamEventType::cursor, {}, {}});
    if (!check(events.Drain().size() == 256, "A disposable snapshot overflowed a full queue")) return 1;

    events.Push({StreamEventType::stats, {}, {}});
    for (int i = 0; i < 256; ++i) events.Push({StreamEventType::minirtc_signal, {}, {}});
    batch = events.Drain();
    if (!check(batch.size() == 256 && batch.back().type == StreamEventType::minirtc_signal,
            "The full queue did not make room for a signaling event")) return 1;
    return 0;
}
