# Host-side doctest suite for the CC LFO sample.
#
# main.cpp is built into a static library (cc_lfo_host) with -fno-exceptions and
# -fno-rtti, the shape the mode has on the device, so the tests exercise it as it
# really runs. Each test executable links that library plus operator_sdk_sim and
# adds back -fexceptions and -frtti for doctest. Each TEST_CASE has its own
# add_test() entry below and a matching name in set_tests_properties(), which is
# what ctest runs. The entries are labeled "sdk;samples".
#
# Two suites live here. test_cc_lfo drives the whole mode through the sim, and
# test_phase_math unit-tests the pure phase helpers by forward-declaring the
# file-scope functions in main.cpp.

add_library(cc_lfo_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(cc_lfo_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # the sample dir, so the tests can include "waveforms.h"
)
target_compile_features(cc_lfo_host PUBLIC cxx_std_20)
target_compile_options(cc_lfo_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_cc_lfo test_cc_lfo.cpp)
target_link_libraries(test_cc_lfo PRIVATE
    cc_lfo_host
    operator_sdk_sim
)
target_include_directories(test_cc_lfo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via cc_lfo_host
)
target_compile_options(test_cc_lfo PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_cc_lfo_disabled_silent
         COMMAND test_cc_lfo "--test-case=slot disabled emits no CC")
add_test(NAME test_cc_lfo_sine_lut_phase0
         COMMAND test_cc_lfo "--test-case=sine LUT phase=0 -> 127")
add_test(NAME test_cc_lfo_sine_lut_peak
         COMMAND test_cc_lfo "--test-case=sine LUT phase=256 -> 254")
add_test(NAME test_cc_lfo_saw_monotonic
         COMMAND test_cc_lfo "--test-case=saw waveform ramps 0..127 linearly")
add_test(NAME test_cc_lfo_sync_on_subdivision
         COMMAND test_cc_lfo "--test-case=sync on maps rate to subdivision")
add_test(NAME test_cc_lfo_sync_off_hz
         COMMAND test_cc_lfo "--test-case=sync off uses Hz via get_tick")
add_test(NAME test_cc_lfo_range_clamp
         COMMAND test_cc_lfo "--test-case=low/high clamp CC range")
add_test(NAME test_cc_lfo_cc_number
         COMMAND test_cc_lfo "--test-case=CC# routes correctly")
add_test(NAME test_cc_lfo_four_slots_independent
         COMMAND test_cc_lfo "--test-case=4 slots run independently")
add_test(NAME test_cc_lfo_sample_hold_steps
         COMMAND test_cc_lfo "--test-case=sample-and-hold changes value per step only")
add_test(NAME test_cc_lfo_sync_slow_option
         COMMAND test_cc_lfo "--test-case=sync slow option 16 sweeps far slower than 1/4")
add_test(NAME test_cc_lfo_throttle_two_even
         COMMAND test_cc_lfo "--test-case=fair-share throttle splits the budget evenly across 2 LFOs at the same rate")
add_test(NAME test_cc_lfo_throttle_no_starve
         COMMAND test_cc_lfo "--test-case=fair-share throttle keeps a fast LFO from starving a slow sibling")
add_test(NAME test_cc_lfo_source_off_free
         COMMAND test_cc_lfo "--test-case=Source=Off emits no CC and Source=Free emits")
add_test(NAME test_cc_lfo_sync_multimeasure_full_sine
         COMMAND test_cc_lfo "--test-case=sync multi-measure (2=192) sweeps the full sine across two bars")
add_test(NAME test_cc_lfo_sync_rate_change_continuous
         COMMAND test_cc_lfo "--test-case=sync rate change stays continuous (no value jump)")
add_test(NAME test_cc_lfo_free_rate_change_continuous
         COMMAND test_cc_lfo "--test-case=free-run rate change stays continuous (no value jump)")
add_test(NAME test_cc_lfo_interp_smooth_between_pulses
         COMMAND test_cc_lfo "--test-case=sync interpolation moves phase between pulses (smooth not stair-stepped)")
add_test(NAME test_cc_lfo_interp_clock_stop_freezes
         COMMAND test_cc_lfo "--test-case=sync interpolation freezes phase when the clock stops with no overshoot")
add_test(NAME test_cc_lfo_transport_start_reanchors
         COMMAND test_cc_lfo "--test-case=sync Start (0xFA) re-anchors phase to 0 at the downbeat")
add_test(NAME test_cc_lfo_transport_stop_reanchors
         COMMAND test_cc_lfo "--test-case=sync Stop (0xFC) also re-anchors phase to 0")
add_test(NAME test_cc_lfo_transport_continue_ignored
         COMMAND test_cc_lfo "--test-case=sync Continue (0xFB) does not reset phase")
add_test(NAME test_cc_lfo_transport_free_unaffected
         COMMAND test_cc_lfo "--test-case=free-run (Hz) phase is not affected by transport")

set_tests_properties(
    test_cc_lfo_disabled_silent
    test_cc_lfo_sine_lut_phase0
    test_cc_lfo_sine_lut_peak
    test_cc_lfo_saw_monotonic
    test_cc_lfo_sync_on_subdivision
    test_cc_lfo_sync_off_hz
    test_cc_lfo_range_clamp
    test_cc_lfo_cc_number
    test_cc_lfo_four_slots_independent
    test_cc_lfo_sample_hold_steps
    test_cc_lfo_sync_slow_option
    test_cc_lfo_throttle_two_even
    test_cc_lfo_throttle_no_starve
    test_cc_lfo_source_off_free
    test_cc_lfo_sync_multimeasure_full_sine
    test_cc_lfo_sync_rate_change_continuous
    test_cc_lfo_free_rate_change_continuous
    test_cc_lfo_interp_smooth_between_pulses
    test_cc_lfo_interp_clock_stop_freezes
    test_cc_lfo_transport_start_reanchors
    test_cc_lfo_transport_stop_reanchors
    test_cc_lfo_transport_continue_ignored
    test_cc_lfo_transport_free_unaffected
    PROPERTIES LABELS "sdk;samples"
)

add_executable(test_cc_lfo_phase_math test_phase_math.cpp)
target_link_libraries(test_cc_lfo_phase_math PRIVATE
    cc_lfo_host
    operator_sdk_sim
)
target_include_directories(test_cc_lfo_phase_math PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via cc_lfo_host
)
target_compile_options(test_cc_lfo_phase_math PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_cc_lfo_phase_off_ramp
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_off is a linear ramp at 1 Hz")
add_test(NAME test_cc_lfo_phase_off_1sec
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_off at 1 Hz and 1 second is 1000 mod 1024")
add_test(NAME test_cc_lfo_phase_off_double_wrap
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_off wraps twice at 2 Hz and lands back on 0")
add_test(NAME test_cc_lfo_interp_edge_locked
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_on_interp with frac=0 gives the exact integer phase at each edge")
add_test(NAME test_cc_lfo_interp_mid_pulse
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_on_interp lands a mid-pulse fraction between adjacent edges")
add_test(NAME test_cc_lfo_interp_many_phases
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_on_interp sub-pulse steps resolve far more phases than the integer path")
add_test(NAME test_cc_lfo_interp_no_overshoot
         COMMAND test_cc_lfo_phase_math "--test-case=phase_sync_on_interp never overshoots the next edge with its sub-pulse sweep")
add_test(NAME test_cc_lfo_reanchor_no_shift
         COMMAND test_cc_lfo_phase_math "--test-case=reanchor_offset makes no shift when the base is unchanged")
add_test(NAME test_cc_lfo_reanchor_continuous
         COMMAND test_cc_lfo_phase_math "--test-case=reanchor_offset keeps the emitted phase unchanged at the change instant")
add_test(NAME test_cc_lfo_ratebeat_endpoints
         COMMAND test_cc_lfo_phase_math "--test-case=ratebeat_to_bpc24 maps the endpoints and the 1/4 reference correctly")
add_test(NAME test_cc_lfo_ratebeat_monotonic
         COMMAND test_cc_lfo_phase_math "--test-case=ratebeat_to_bpc24 lists options strictly slow to fast")

set_tests_properties(
    test_cc_lfo_phase_off_ramp
    test_cc_lfo_phase_off_1sec
    test_cc_lfo_phase_off_double_wrap
    test_cc_lfo_interp_edge_locked
    test_cc_lfo_interp_mid_pulse
    test_cc_lfo_interp_many_phases
    test_cc_lfo_interp_no_overshoot
    test_cc_lfo_reanchor_no_shift
    test_cc_lfo_reanchor_continuous
    test_cc_lfo_ratebeat_endpoints
    test_cc_lfo_ratebeat_monotonic
    PROPERTIES LABELS "sdk;samples"
)
