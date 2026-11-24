#include "video_frame_cadence.h"

#include <cstdio>

namespace {

bool CheckResume(int fps, bool desktop) {
    minirtc::VideoFrameCadence cadence;
    const int interval = 1'000'000 / fps;
    auto admit = [&](int time) {
        return cadence.Accept(time, fps, desktop && cadence.CanResumeDesktop(time));
    };

    // A repeat after a second of stillness is followed by a single scroll update.
    constexpr int repeat = 1'000'001;
    constexpr int scroll = repeat + 2'000;
    if (!admit(1) || !admit(repeat) || admit(scroll) != desktop) return false;
    if (admit(scroll + 1'000)) return false;
    for (int frame = 1; frame <= 120; ++frame) {
        const int next = scroll + frame * interval;
        if (!admit(next) || admit(next + 1'000)) return false;
    }

    // A motion frame outside the short resume window follows ordinary admission.
    const int second_repeat = scroll + 120 * interval + 1'000'000;
    const int later = second_repeat + 2 * interval;
    return admit(second_repeat) && !cadence.CanResumeDesktop(later) &&
        admit(later) && !admit(later + 1'000);
}

} // namespace

int main() {
    if (CheckResume(30, true) && CheckResume(60, true) &&
        CheckResume(30, false) && CheckResume(60, false)) return 0;
    std::fputs("Unexpected video frame admission after idle or during motion\n", stderr);
    return 1;
}
