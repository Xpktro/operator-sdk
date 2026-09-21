// text.h has to compile with nothing else from the SDK included, so building this
// probe is the check. Naming a symbol forces the header body to be parsed.
#include <operator_sdk/text.h>
int main() {
    volatile uint16_t w = op::sdk::text::text_width("ABC");
    (void)w;
    return 0;
}
