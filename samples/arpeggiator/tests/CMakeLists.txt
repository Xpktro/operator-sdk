# Host-side doctest suite for the Arpeggiator sample.
#
# main.cpp is built into a static library (arpeggiator_host) with -fno-exceptions
# and -fno-rtti, the shape the mode has on the device, so the tests exercise it as
# it really runs. The test executable links that library plus operator_sdk_sim and
# adds back -fexceptions and -frtti for doctest. Each TEST_CASE has its own
# add_test() entry below and a matching name in set_tests_properties(), which is
# what ctest runs. The entries are labeled "sdk;samples".

add_library(arpeggiator_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(arpeggiator_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_compile_features(arpeggiator_host PUBLIC cxx_std_20)
target_compile_options(arpeggiator_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_arpeggiator test_arpeggiator.cpp)
target_link_libraries(test_arpeggiator PRIVATE
    arpeggiator_host
    operator_sdk_sim
)
target_include_directories(test_arpeggiator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # doctest.h, the SDK include comes via arpeggiator_host
)
target_compile_options(test_arpeggiator PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_arpeggiator_style_up
         COMMAND test_arpeggiator "--test-case=style=Up cycles held notes ascending")
add_test(NAME test_arpeggiator_style_down
         COMMAND test_arpeggiator "--test-case=style=Down cycles descending")
add_test(NAME test_arpeggiator_style_updown
         COMMAND test_arpeggiator "--test-case=style=Up-Down bounces")
add_test(NAME test_arpeggiator_style_random
         COMMAND test_arpeggiator "--test-case=style=Random picks from the held notes and repeats a run")
add_test(NAME test_arpeggiator_octave_shift
         COMMAND test_arpeggiator "--test-case=octave=+1 shifts output up by 12")
add_test(NAME test_arpeggiator_fill_full
         COMMAND test_arpeggiator "--test-case=fill=100 note-off at next note-on time")
add_test(NAME test_arpeggiator_fill_half
         COMMAND test_arpeggiator "--test-case=fill=50 holds note half the step")
add_test(NAME test_arpeggiator_chord_preserves_intervals
         COMMAND test_arpeggiator "--test-case=chord on preserves semitone distances")
add_test(NAME test_arpeggiator_triplet_rate
         COMMAND test_arpeggiator "--test-case=rate 8/beat with triplet")
add_test(NAME test_arpeggiator_silent_when_no_notes
         COMMAND test_arpeggiator "--test-case=no held notes = silent")
add_test(NAME test_arpeggiator_notes_only_passthrough
         COMMAND test_arpeggiator "--test-case=note suppression removes notes and passes non-note messages through")
add_test(NAME test_arpeggiator_at_flood_chord_intact
         COMMAND test_arpeggiator "--test-case=an aftertouch flood disturbs neither the held chord nor the arp output")

set_tests_properties(
    test_arpeggiator_style_up
    test_arpeggiator_style_down
    test_arpeggiator_style_updown
    test_arpeggiator_style_random
    test_arpeggiator_octave_shift
    test_arpeggiator_fill_full
    test_arpeggiator_fill_half
    test_arpeggiator_chord_preserves_intervals
    test_arpeggiator_triplet_rate
    test_arpeggiator_silent_when_no_notes
    test_arpeggiator_notes_only_passthrough
    test_arpeggiator_at_flood_chord_intact
    PROPERTIES LABELS "sdk;samples"
)
