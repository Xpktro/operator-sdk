# Host-side doctest suite for the Euclidean Sequencer sample.
#
# main.cpp is built into a static library (euclidean_sequencer_host) with
# -fno-exceptions and -fno-rtti, the shape the mode has on the device, so the
# tests exercise it as it really runs. The test executable links that library plus
# operator_sdk_sim and adds back -fexceptions and -frtti for doctest. Each
# TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".

add_library(euclidean_sequencer_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(euclidean_sequencer_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_compile_features(euclidean_sequencer_host PUBLIC cxx_std_20)
target_compile_options(euclidean_sequencer_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_euclidean_sequencer test_euclidean_sequencer.cpp)
target_link_libraries(test_euclidean_sequencer PRIVATE
    euclidean_sequencer_host
    operator_sdk_sim
)
target_include_directories(test_euclidean_sequencer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # doctest.h, the SDK include comes via euclidean_sequencer_host
)
target_compile_options(test_euclidean_sequencer PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_euclidean_seq_triplet
         COMMAND test_euclidean_sequencer "--test-case=three events over eight steps gives the triplet")
add_test(NAME test_euclidean_seq_zero_events
         COMMAND test_euclidean_sequencer "--test-case=zero events gives all rests")
add_test(NAME test_euclidean_seq_full_pattern
         COMMAND test_euclidean_sequencer "--test-case=events at or above steps fills every step")
add_test(NAME test_euclidean_seq_offset
         COMMAND test_euclidean_sequencer "--test-case=offset rotates the pattern")
add_test(NAME test_euclidean_seq_four_independent
         COMMAND test_euclidean_sequencer "--test-case=four sequences run independently")
add_test(NAME test_euclidean_seq_step_sync
         COMMAND test_euclidean_sequencer "--test-case=steps advance with the measure position")
add_test(NAME test_euclidean_seq_events_zero_silent
         COMMAND test_euclidean_sequencer "--test-case=a sequence with zero events emits no notes")
add_test(NAME test_euclidean_seq_events_clamped
         COMMAND test_euclidean_sequencer "--test-case=events above steps fills every step")
add_test(NAME test_euclidean_seq_idle_clock_silent
         COMMAND test_euclidean_sequencer "--test-case=a sequence loaded with no clock running stays silent")
add_test(NAME test_euclidean_seq_load_inside_step
         COMMAND test_euclidean_sequencer "--test-case=a sequence loaded inside an onset step waits for the next onset")
add_test(NAME test_euclidean_seq_main_rows
         COMMAND test_euclidean_sequencer "--test-case=the main screen renders one row per sequence")
add_test(NAME test_euclidean_seq_main_left_rotate
         COMMAND test_euclidean_sequencer "--test-case=Scroll moves the row selection on the main screen")
add_test(NAME test_euclidean_seq_enter_detail
         COMMAND test_euclidean_sequencer "--test-case=Enter on the main screen opens the detail editor")
add_test(NAME test_euclidean_seq_leave_detail
         COMMAND test_euclidean_sequencer "--test-case=Back on the detail screen returns to main")
add_test(NAME test_euclidean_seq_detail_edit_param
         COMMAND test_euclidean_sequencer "--test-case=Change edits the selected detail param")
add_test(NAME test_euclidean_seq_detail_param_select
         COMMAND test_euclidean_sequencer "--test-case=Scroll cycles the detail param selection")
add_test(NAME test_euclidean_seq_detail_reset
         COMMAND test_euclidean_sequencer "--test-case=Enter on the detail screen resets the sequence")

set_tests_properties(
    test_euclidean_seq_triplet
    test_euclidean_seq_zero_events
    test_euclidean_seq_full_pattern
    test_euclidean_seq_offset
    test_euclidean_seq_four_independent
    test_euclidean_seq_step_sync
    test_euclidean_seq_events_zero_silent
    test_euclidean_seq_events_clamped
    test_euclidean_seq_idle_clock_silent
    test_euclidean_seq_load_inside_step
    test_euclidean_seq_main_rows
    test_euclidean_seq_main_left_rotate
    test_euclidean_seq_enter_detail
    test_euclidean_seq_leave_detail
    test_euclidean_seq_detail_edit_param
    test_euclidean_seq_detail_param_select
    test_euclidean_seq_detail_reset
    PROPERTIES LABELS "sdk;samples"
)
