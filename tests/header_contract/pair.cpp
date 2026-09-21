// All the helpers have to coexist in one translation unit without clashing, so
// building this probe is the check.
#include <operator_sdk/text.h>
#include <operator_sdk/timing.h>
#include <operator_sdk/midi.h>
#include <operator_sdk/math.h>
int main() {
    volatile int sum = op::sdk::text::kFontStdAdvance + op::sdk::timing::kSubdiv_4
        + op::sdk::midi::kCCModWheel + op::sdk::math::map_linear(5, 0, 10, 0, 100);
    (void)sum;
    return 0;
}
