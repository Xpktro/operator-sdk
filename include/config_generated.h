#pragma once
// Hard limit values encoded in the firmware.
//
// These are fixed limits the firmware enforces on every mode. They are
// provided here so your mode can reference them at build time. You do not
// change them.

#include <cstdint>

namespace op::config {

// Hard limits enforced by the firmware.
constexpr uint8_t kMaxParamsPerMode      = 36;
constexpr uint8_t kMaxParamValuesPerMode = 64;
constexpr uint32_t kMaxModeBinarySize    = 262144;  // 256 KB per mode

}  // namespace op::config
