# Host-side doctest suite for the Note Probability sample.
#
# main.cpp is built into a static library (note_probability_host) with
# -fno-exceptions and -fno-rtti, the shape the mode has on the device, so the
# tests exercise it as it really runs. The test executable links that library plus
# operator_sdk_sim and adds back -fexceptions and -frtti for doctest. Each
# TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".

add_library(note_probability_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(note_probability_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(note_probability_host PUBLIC cxx_std_20)
target_compile_options(note_probability_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_note_probability test_note_probability.cpp)
target_link_libraries(test_note_probability PRIVATE
    note_probability_host
    operator_sdk_sim
)
target_include_directories(test_note_probability PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via note_probability_host
)
target_compile_options(test_note_probability PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_note_probability_all_play
         COMMAND test_note_probability "--test-case=every note plays at probability 100")
add_test(NAME test_note_probability_none_play
         COMMAND test_note_probability "--test-case=no note plays at probability 0")
add_test(NAME test_note_probability_half_play
         COMMAND test_note_probability "--test-case=about half the notes play at probability 50")
add_test(NAME test_note_probability_passes_the_rest
         COMMAND test_note_probability "--test-case=everything other than a note-on passes whatever the probability")

set_tests_properties(
    test_note_probability_all_play
    test_note_probability_none_play
    test_note_probability_half_play
    test_note_probability_passes_the_rest
    PROPERTIES LABELS "sdk;samples"
)
