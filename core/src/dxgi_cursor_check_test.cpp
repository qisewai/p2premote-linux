#include "dxgi_cursor_check.h"

#include <cstdio>

int main() {
    p2premote::DxgiCursorCheck check;
    const bool single_mismatch_ignored = !check.Observe(true, 10) && !check.Observe(false, 25);
    const bool isolated_mismatches_ignored = !check.Observe(true, 100) && !check.Observe(true, 350);
    const bool confirmation_delayed = !check.Observe(true, 400);
    const bool sustained_mismatch_detected = check.Observe(true, 430);
    const bool visible_report_resets = !check.Observe(false, 450) && !check.Observe(true, 460);
    if (single_mismatch_ignored && isolated_mismatches_ignored && confirmation_delayed
            && sustained_mismatch_detected && visible_report_resets) return 0;
    std::fputs("DXGI pointer transition was not debounced as expected\n", stderr);
    return 1;
}
