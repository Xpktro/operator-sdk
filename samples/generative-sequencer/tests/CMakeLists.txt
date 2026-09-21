# Host-side doctest suite for the Generative Sequencer sample.
#
# The sample's two sources are built into a static library
# (generative_sequencer_host) with -fno-exceptions and -fno-rtti, the shape the
# mode has on the device, so the tests exercise it as it really runs. The test
# executable links that library plus operator_sdk_sim and adds back -fexceptions
# and -frtti for doctest. Each TEST_CASE has its own add_test() entry below and a
# matching name in set_tests_properties(), which is what ctest runs. The entries
# are labeled "sdk;samples".

add_library(generative_sequencer_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sequencer_engine.cpp
)
target_include_directories(generative_sequencer_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # the sample dir, so main.cpp finds "sequencer_engine.h"
)
target_compile_features(generative_sequencer_host PUBLIC cxx_std_20)
target_compile_options(generative_sequencer_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_generative_sequencer test_generative_sequencer.cpp)
target_link_libraries(test_generative_sequencer PRIVATE
    generative_sequencer_host
    operator_sdk_sim
)
target_include_directories(test_generative_sequencer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via generative_sequencer_host
)
target_compile_options(test_generative_sequencer PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_generative_sequencer_note_range
         COMMAND test_generative_sequencer "--test-case=every note lands inside the note range")
add_test(NAME test_generative_sequencer_probability_zero
         COMMAND test_generative_sequencer "--test-case=probability 0 plays nothing")
add_test(NAME test_generative_sequencer_probability_full
         COMMAND test_generative_sequencer "--test-case=probability 100 plays a note on every step")
add_test(NAME test_generative_sequencer_notes_released
         COMMAND test_generative_sequencer "--test-case=every note that starts is released")
add_test(NAME test_generative_sequencer_unload_releases
         COMMAND test_generative_sequencer "--test-case=unloading releases the notes still sounding")
add_test(NAME test_generative_sequencer_follows_clock
         COMMAND test_generative_sequencer "--test-case=the sequencer follows the device clock")
add_test(NAME test_generative_sequencer_stopped_clock
         COMMAND test_generative_sequencer "--test-case=a stopped clock leaves the sequencer on its step")
add_test(NAME test_generative_sequencer_all_spreads
         COMMAND test_generative_sequencer "--test-case=All spreads the notes across the outputs in turn")
add_test(NAME test_generative_sequencer_all_skips_inactive
         COMMAND test_generative_sequencer "--test-case=All skips an output that cannot play")
add_test(NAME test_generative_sequencer_chosen_output
         COMMAND test_generative_sequencer "--test-case=a chosen output takes every note")
add_test(NAME test_generative_sequencer_chosen_output_silent
         COMMAND test_generative_sequencer "--test-case=a chosen output that cannot play stays silent")

set_tests_properties(
    test_generative_sequencer_note_range
    test_generative_sequencer_probability_zero
    test_generative_sequencer_probability_full
    test_generative_sequencer_notes_released
    test_generative_sequencer_unload_releases
    test_generative_sequencer_follows_clock
    test_generative_sequencer_stopped_clock
    test_generative_sequencer_all_spreads
    test_generative_sequencer_all_skips_inactive
    test_generative_sequencer_chosen_output
    test_generative_sequencer_chosen_output_silent
    PROPERTIES LABELS "sdk;samples"
)
