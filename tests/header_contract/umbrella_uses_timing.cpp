// This is meant to fail to compile. The umbrella does not pull in timing.h, and
// CMake marks the compile WILL_FAIL.
#include <operator_sdk.h>
int main() {
    return static_cast<int>(op::sdk::timing::kSubdiv_4);
}
