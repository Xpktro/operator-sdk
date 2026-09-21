#pragma once
// What the harness sources share with each other.
//
// Private. A consumer reaches the harness through operator_sdk_sim.h.

#include <cstdint>

namespace op::sim::detail {

// Implemented by mock_operator_api.cpp, which owns the state.
std::uint8_t* framebuffer_bytes();

}  // namespace op::sim::detail
