// The umbrella has to compile on its own, or the will-fail probes would pass for
// the wrong reason, the umbrella being broken rather than the helper excluded.
#include <operator_sdk.h>
int main() {
    return 0;
}
