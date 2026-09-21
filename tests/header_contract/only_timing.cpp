// timing.h has to compile on its own, so building this probe is the check.
#include <operator_sdk/timing.h>
int main() {
    volatile uint32_t us = op::sdk::timing::subdivision_us(op::sdk::timing::kSubdiv_4, 120);
    (void)us;
    return 0;
}
