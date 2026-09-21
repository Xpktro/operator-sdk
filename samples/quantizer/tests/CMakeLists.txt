# Host-side doctest suite for the Quantizer sample.
#
# main.cpp is built into a static library (quantizer_host) with -fno-exceptions
# and -fno-rtti, the shape the mode has on the device, so the tests exercise it as
# it really runs. The test executable links that library plus operator_sdk_sim and
# adds back -fexceptions and -frtti for doctest. Each TEST_CASE has its own
# add_test() entry below and a matching name in set_tests_properties(), which is
# what ctest runs. The entries are labeled "sdk;samples".

add_library(quantizer_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(quantizer_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(quantizer_host PUBLIC cxx_std_20)
target_compile_options(quantizer_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_quantizer test_quantizer.cpp)
target_link_libraries(test_quantizer PRIVATE
    quantizer_host
    operator_sdk_sim
)
target_include_directories(test_quantizer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via quantizer_host
)
target_compile_options(test_quantizer PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_quantizer_early_and_late
         COMMAND test_quantizer "--test-case=a note early in the window plays at once and a late one waits")
add_test(NAME test_quantizer_division_spacing
         COMMAND test_quantizer "--test-case=Division sets how far apart the grid points are")
add_test(NAME test_quantizer_triplet_spacing
         COMMAND test_quantizer "--test-case=Triplet moves the grid points closer together")
add_test(NAME test_quantizer_bar_wrap
         COMMAND test_quantizer "--test-case=a held note comes out even when the bar turns over")
add_test(NAME test_quantizer_two_in_a_window
         COMMAND test_quantizer "--test-case=two notes held in the same window both come out")
add_test(NAME test_quantizer_ninth_dropped
         COMMAND test_quantizer "--test-case=a ninth note waiting in one window is dropped")
add_test(NAME test_quantizer_note_off_passes
         COMMAND test_quantizer "--test-case=a note-off passes straight through")
add_test(NAME test_quantizer_non_notes_pass
         COMMAND test_quantizer "--test-case=anything that is not a note passes straight through")
add_test(NAME test_quantizer_released_early
         COMMAND test_quantizer "--test-case=a note released before its grid point never sounds")
add_test(NAME test_quantizer_start_reanchors
         COMMAND test_quantizer "--test-case=Start re-anchors the grid and drops what was waiting")
add_test(NAME test_quantizer_stop_reanchors
         COMMAND test_quantizer "--test-case=Stop re-anchors the grid as Start does")
add_test(NAME test_quantizer_division_change_drops
         COMMAND test_quantizer "--test-case=changing Division drops the notes that were waiting")
add_test(NAME test_quantizer_scroll_division
         COMMAND test_quantizer "--test-case=Scroll cycles the Division and comes back round")
add_test(NAME test_quantizer_change_triplet
         COMMAND test_quantizer "--test-case=Change toggles Triplet")

set_tests_properties(
    test_quantizer_early_and_late
    test_quantizer_division_spacing
    test_quantizer_triplet_spacing
    test_quantizer_bar_wrap
    test_quantizer_two_in_a_window
    test_quantizer_ninth_dropped
    test_quantizer_note_off_passes
    test_quantizer_non_notes_pass
    test_quantizer_released_early
    test_quantizer_start_reanchors
    test_quantizer_stop_reanchors
    test_quantizer_division_change_drops
    test_quantizer_scroll_division
    test_quantizer_change_triplet
    PROPERTIES LABELS "sdk;samples"
)
