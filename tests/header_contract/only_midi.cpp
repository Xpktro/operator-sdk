// midi.h has to compile on its own, so building this probe is the check.
#include <operator_sdk/midi.h>
int main() {
    volatile uint8_t cc = op::sdk::midi::kCCModWheel;
    (void)cc;
    return 0;
}
