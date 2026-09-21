// log.h has to compile on its own, so building this probe is the check. op::log is
// inline and unreferenced here, so op::api needs no definition.
#include <operator_sdk/log.h>
int main() {
    volatile uint8_t lvl = static_cast<uint8_t>(op::LogLevel::Info);
    (void)lvl;
    return 0;
}
