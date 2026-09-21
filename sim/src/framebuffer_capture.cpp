// Copying the framebuffer out for a test to read.
//
// The bytes are the ones MockDisplay draws into. Copying them at the moment of
// capture gives a test a still image it can hash or check against a known one.

#include "operator_sdk_sim.h"
#include "harness_internal.h"

#include <cstring>

namespace op::sim {

void capture_framebuffer(std::uint8_t buf[kFramebufferBytes]) {
    if (!buf) return;
    const std::uint8_t* src = detail::framebuffer_bytes();
    if (!src) {
        std::memset(buf, 0, kFramebufferBytes);
        return;
    }
    std::memcpy(buf, src, kFramebufferBytes);
}

}  // namespace op::sim
