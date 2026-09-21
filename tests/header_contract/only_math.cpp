// math.h has to compile on its own, so building this probe is the check.
#include <operator_sdk/math.h>
int main() {
    volatile int32_t v = op::sdk::math::map_linear(5, 0, 10, 0, 100);
    (void)v;
    return 0;
}
