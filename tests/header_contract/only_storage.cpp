// storage.h has to compile on its own, so building this probe is the check.
// op::write_file is inline and unreferenced here, so op::api needs no definition.
#include <operator_sdk/storage.h>
int main() {
    volatile int32_t busy = kStorageBusy;
    (void)busy;
    return 0;
}
