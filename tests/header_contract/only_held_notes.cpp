// held_notes.h has to compile on its own, so building this probe is the check. The
// registry is a class template, and a member body that touches Payload is checked
// only once Payload is substituted, so the explicit instantiation is what puts every
// member through the compiler.
#include <operator_sdk/held_notes.h>

template class op::sdk::HeldNotes<>;

int main() {
    return 0;
}
