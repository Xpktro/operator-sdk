# Host-side doctest suite for the Delay sample.
#
# main.cpp is built into a static library (delay_host) with -fno-exceptions and
# -fno-rtti, the shape the mode has on the device, so the tests exercise it as it
# really runs. The test executable links that library plus operator_sdk_sim and
# adds back -fexceptions and -frtti for doctest. Each TEST_CASE has its own
# add_test() entry below and a matching name in set_tests_properties(), which is
# what ctest runs. The entries are labeled "sdk;samples".

add_library(delay_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(delay_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(delay_host PUBLIC cxx_std_20)
target_compile_options(delay_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_delay test_delay.cpp)
target_link_libraries(test_delay PRIVATE
    delay_host
    operator_sdk_sim
)
target_include_directories(test_delay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # doctest.h, the SDK include comes via delay_host
)
target_compile_options(test_delay PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_delay_sync_division
         COMMAND test_delay "--test-case=sync division maps to pulses")
add_test(NAME test_delay_sync_dotted_triplet
         COMMAND test_delay "--test-case=sync dotted and triplet pulses")
add_test(NAME test_delay_freerun_ms
         COMMAND test_delay "--test-case=free-run ms delay fires at tick")
add_test(NAME test_delay_note_relative
         COMMAND test_delay "--test-case=sync echo is note-relative not grid-quantized")
add_test(NAME test_delay_held_defers
         COMMAND test_delay "--test-case=held note defers echoes until release")
add_test(NAME test_delay_gate_equals_delta_alternating
         COMMAND test_delay "--test-case=gate equals delta same-pitch echo stream is strictly alternating")
add_test(NAME test_delay_feedback_decay
         COMMAND test_delay "--test-case=feedback decays velocity")
add_test(NAME test_delay_velocity_floor
         COMMAND test_delay "--test-case=velocity below 1 stops trail")
add_test(NAME test_delay_repeats_cap
         COMMAND test_delay "--test-case=repeats caps echo count")
add_test(NAME test_delay_transpose
         COMMAND test_delay "--test-case=transpose shifts echoes and drops out of range")
add_test(NAME test_delay_retrigger
         COMMAND test_delay "--test-case=same-pitch retrigger releases old note")
add_test(NAME test_delay_pool_steal
         COMMAND test_delay "--test-case=pool full steals oldest with note-off")
add_test(NAME test_delay_stop_flush
         COMMAND test_delay "--test-case=transport stop flushes pending")
add_test(NAME test_delay_nonnote_passthrough
         COMMAND test_delay "--test-case=non-note passes through dry")
add_test(NAME test_delay_destroy_release
         COMMAND test_delay "--test-case=destroy releases all voices")
add_test(NAME test_delay_held_backtoback
         COMMAND test_delay "--test-case=held note echoes are back-to-back delta-length after release")
add_test(NAME test_delay_cross_voice_overlap
         COMMAND test_delay "--test-case=cross-voice transposed echoes never overlap on one pitch")
add_test(NAME test_delay_bypass
         COMMAND test_delay "--test-case=bypass flushes voices and passes input through")
add_test(NAME test_delay_channel_filter
         COMMAND test_delay "--test-case=channel filter delays only the selected channel")

set_tests_properties(
    test_delay_sync_division
    test_delay_sync_dotted_triplet
    test_delay_freerun_ms
    test_delay_note_relative
    test_delay_held_defers
    test_delay_gate_equals_delta_alternating
    test_delay_feedback_decay
    test_delay_velocity_floor
    test_delay_repeats_cap
    test_delay_transpose
    test_delay_retrigger
    test_delay_pool_steal
    test_delay_stop_flush
    test_delay_nonnote_passthrough
    test_delay_destroy_release
    test_delay_held_backtoback
    test_delay_cross_voice_overlap
    test_delay_bypass
    test_delay_channel_filter
    PROPERTIES LABELS "sdk;samples"
)
