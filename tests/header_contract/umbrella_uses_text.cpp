// This is meant to fail to compile. The umbrella does not pull in text.h, so the
// symbol below is undeclared, and CMake marks the compile WILL_FAIL: a compile that
// succeeds means the umbrella leaked the helper, and the test fails.
#include <operator_sdk.h>
int main() {
    return static_cast<int>(op::sdk::text::kFontStdAdvance);
}
