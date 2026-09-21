# Host-side doctest suite for the Velocity Remap sample.
#
# main.cpp is built into a static library (velocity_remap_host) with
# -fno-exceptions and -fno-rtti, the shape the mode has on the device, so the
# tests exercise it as it really runs. Each test executable links that library plus
# operator_sdk_sim and adds back -fexceptions and -frtti for doctest. Each
# TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".
#
# Two suites live here. test_velocity_remap drives the whole mode through the sim,
# and test_velocity_curve unit-tests the pure curve builder in velocity_curve.h.
# On the host the C library's expf and logf satisfy the declarations the curve
# builder makes, so it needs no wiring of its own.

add_library(velocity_remap_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(velocity_remap_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # the sample dir, so the tests can include "velocity_curve.h"
)
target_compile_features(velocity_remap_host PUBLIC cxx_std_20)
target_compile_options(velocity_remap_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_velocity_remap test_velocity_remap.cpp)
target_link_libraries(test_velocity_remap PRIVATE
    velocity_remap_host
    operator_sdk_sim
)
target_include_directories(test_velocity_remap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via velocity_remap_host
)
target_compile_options(test_velocity_remap PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_velocity_remap_note_on_remapped
         COMMAND test_velocity_remap "--test-case=a note-on comes out at the velocity the curve maps it to")
add_test(NAME test_velocity_remap_passthrough
         COMMAND test_velocity_remap "--test-case=messages other than note-ons pass through untouched")
add_test(NAME test_velocity_remap_constant
         COMMAND test_velocity_remap "--test-case=Constant sends every note at the Maximum velocity")
add_test(NAME test_velocity_remap_change_edits
         COMMAND test_velocity_remap "--test-case=Change edits the focused field")
add_test(NAME test_velocity_remap_scroll_focus
         COMMAND test_velocity_remap "--test-case=Scroll moves the focus to the next field")
add_test(NAME test_velocity_remap_enter_resets
         COMMAND test_velocity_remap "--test-case=Enter resets the focused field to its default")
add_test(NAME test_velocity_remap_back_toggles_constant
         COMMAND test_velocity_remap "--test-case=Back toggles Constant and holds Minimum at the Maximum")
add_test(NAME test_velocity_remap_constant_hides_minimum
         COMMAND test_velocity_remap "--test-case=Constant hides Minimum so the focus skips it")

set_tests_properties(
    test_velocity_remap_note_on_remapped
    test_velocity_remap_passthrough
    test_velocity_remap_constant
    test_velocity_remap_change_edits
    test_velocity_remap_scroll_focus
    test_velocity_remap_enter_resets
    test_velocity_remap_back_toggles_constant
    test_velocity_remap_constant_hides_minimum
    PROPERTIES LABELS "sdk;samples"
)

add_executable(test_velocity_curve test_velocity_curve.cpp)
target_link_libraries(test_velocity_curve PRIVATE
    velocity_remap_host
    operator_sdk_sim
)
target_include_directories(test_velocity_curve PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via velocity_remap_host
)
target_compile_options(test_velocity_curve PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_velocity_curve_identity
         COMMAND test_velocity_curve "--test-case=the default window passes every velocity through unchanged")
add_test(NAME test_velocity_curve_compand
         COMMAND test_velocity_curve "--test-case=compand widens and narrows the spread around the middle")
add_test(NAME test_velocity_curve_tension
         COMMAND test_velocity_curve "--test-case=tension bends the curve toward loud or soft")
add_test(NAME test_velocity_curve_inverted_window
         COMMAND test_velocity_curve "--test-case=a minimum above the maximum flips the curve")
add_test(NAME test_velocity_curve_monotonic
         COMMAND test_velocity_curve "--test-case=the curve stays non-decreasing across the param range")
add_test(NAME test_velocity_curve_inside_window
         COMMAND test_velocity_curve "--test-case=the curve stays inside the output window")
add_test(NAME test_velocity_curve_reference
         COMMAND test_velocity_curve "--test-case=the single-precision table tracks the double-precision reference")

set_tests_properties(
    test_velocity_curve_identity
    test_velocity_curve_compand
    test_velocity_curve_tension
    test_velocity_curve_inverted_window
    test_velocity_curve_monotonic
    test_velocity_curve_inside_window
    test_velocity_curve_reference
    PROPERTIES LABELS "sdk;samples"
)
