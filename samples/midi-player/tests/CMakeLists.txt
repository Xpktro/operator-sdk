# Host-side doctest suite for the MIDI Player sample.
#
# main.cpp is built into a static library (midi_player_host) with -fno-exceptions
# and -fno-rtti, the shape the mode has on the device, so the tests exercise it as
# it really runs. Each test executable links that library plus operator_sdk_sim and
# adds back -fexceptions and -frtti for doctest. Each TEST_CASE has its own
# add_test() entry below and a matching name in set_tests_properties(), which is
# what ctest runs. The entries are labeled "sdk;samples".
#
# Two suites live here. test_midi_player drives the whole mode through the sim,
# and test_smf_parser unit-tests the SMF parser in smf_parser.h.

add_library(midi_player_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(midi_player_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # the sample dir, so the tests can include "smf_parser.h"
)
target_compile_features(midi_player_host PUBLIC cxx_std_20)
target_compile_options(midi_player_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_midi_player test_midi_player.cpp)
target_link_libraries(test_midi_player PRIVATE
    midi_player_host
    operator_sdk_sim
)
target_include_directories(test_midi_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via midi_player_host
)
target_compile_options(test_midi_player PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_midi_player_silent_until_play
         COMMAND test_midi_player "--test-case=a loaded file stays silent until the transport starts")
add_test(NAME test_midi_player_file_tempo
         COMMAND test_midi_player "--test-case=playing sends the file's notes in time with its own tempo")
add_test(NAME test_midi_player_external_clock
         COMMAND test_midi_player "--test-case=playing follows the device clock when Clock Source is Ext.")
add_test(NAME test_midi_player_sub_pulse
         COMMAND test_midi_player "--test-case=Ext. playback moves between the clock's pulse edges")
add_test(NAME test_midi_player_large_file
         COMMAND test_midi_player "--test-case=a large multi-track file plays through its buffer refills")
add_test(NAME test_midi_player_pause_silences
         COMMAND test_midi_player "--test-case=pausing silences the notes still ringing")
add_test(NAME test_midi_player_pause_quiet
         COMMAND test_midi_player "--test-case=pausing with nothing ringing sends no silencing")
add_test(NAME test_midi_player_paused_sends_nothing
         COMMAND test_midi_player "--test-case=a paused transport sends nothing")
add_test(NAME test_midi_player_resume
         COMMAND test_midi_player "--test-case=resuming picks the song up where it paused")
add_test(NAME test_midi_player_stop
         COMMAND test_midi_player "--test-case=stopping silences the notes and rewinds to the top")
add_test(NAME test_midi_player_end_of_song
         COMMAND test_midi_player "--test-case=reaching the end holds until play starts the song again")
add_test(NAME test_midi_player_scroll
         COMMAND test_midi_player "--test-case=Scroll moves the selection onto stop")
add_test(NAME test_midi_player_change_clock_source
         COMMAND test_midi_player "--test-case=Change cycles the Clock Source")
add_test(NAME test_midi_player_switch_clock_source
         COMMAND test_midi_player "--test-case=switching the Clock Source mid-song keeps the position")
add_test(NAME test_midi_player_malformed
         COMMAND test_midi_player "--test-case=a malformed file plays nothing")
add_test(NAME test_midi_player_no_file
         COMMAND test_midi_player "--test-case=the transport does nothing with no file loaded")

set_tests_properties(
    test_midi_player_silent_until_play
    test_midi_player_file_tempo
    test_midi_player_external_clock
    test_midi_player_sub_pulse
    test_midi_player_large_file
    test_midi_player_pause_silences
    test_midi_player_pause_quiet
    test_midi_player_paused_sends_nothing
    test_midi_player_resume
    test_midi_player_stop
    test_midi_player_end_of_song
    test_midi_player_scroll
    test_midi_player_change_clock_source
    test_midi_player_switch_clock_source
    test_midi_player_malformed
    test_midi_player_no_file
    PROPERTIES LABELS "sdk;samples"
)

add_executable(test_smf_parser test_smf_parser.cpp)
target_link_libraries(test_smf_parser PRIVATE
    midi_player_host
    operator_sdk_sim
)
target_include_directories(test_smf_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via midi_player_host
)
target_compile_options(test_smf_parser PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_smf_parser_type0
         COMMAND test_smf_parser "--test-case=parses a Type 0 header")
add_test(NAME test_smf_parser_type1
         COMMAND test_smf_parser "--test-case=parses a Type 1 header")
add_test(NAME test_smf_parser_type2_rejected
         COMMAND test_smf_parser "--test-case=rejects a Type 2 file")
add_test(NAME test_smf_parser_bad_magic
         COMMAND test_smf_parser "--test-case=rejects a file whose header magic is wrong")
add_test(NAME test_smf_parser_track_cap_clamp
         COMMAND test_smf_parser "--test-case=clamps a track count above the cap")
add_test(NAME test_smf_parser_track_cap_load
         COMMAND test_smf_parser "--test-case=loads only as many tracks as the cap allows")
add_test(NAME test_smf_parser_oversize
         COMMAND test_smf_parser "--test-case=rejects a file that declares more bytes than the cap")
add_test(NAME test_smf_parser_delta_single
         COMMAND test_smf_parser "--test-case=decodes a single-byte delta")
add_test(NAME test_smf_parser_delta_multi
         COMMAND test_smf_parser "--test-case=decodes a multi-byte delta")
add_test(NAME test_smf_parser_delta_longest
         COMMAND test_smf_parser "--test-case=decodes the longest delta the format allows")
add_test(NAME test_smf_parser_delta_too_long
         COMMAND test_smf_parser "--test-case=rejects a delta that runs past four bytes")
add_test(NAME test_smf_parser_running_status
         COMMAND test_smf_parser "--test-case=carries running status across channel-voice events")
add_test(NAME test_smf_parser_running_status_dropped
         COMMAND test_smf_parser "--test-case=drops running status after a system message")
add_test(NAME test_smf_parser_tempo_meta
         COMMAND test_smf_parser "--test-case=reads the tempo meta event")
add_test(NAME test_smf_parser_end_of_track_meta
         COMMAND test_smf_parser "--test-case=reads the end-of-track meta event")
add_test(NAME test_smf_parser_time_signature_meta
         COMMAND test_smf_parser "--test-case=reads the time-signature meta event")
add_test(NAME test_smf_parser_unknown_meta
         COMMAND test_smf_parser "--test-case=skips a meta event it does not act on")

set_tests_properties(
    test_smf_parser_type0
    test_smf_parser_type1
    test_smf_parser_type2_rejected
    test_smf_parser_bad_magic
    test_smf_parser_track_cap_clamp
    test_smf_parser_track_cap_load
    test_smf_parser_oversize
    test_smf_parser_delta_single
    test_smf_parser_delta_multi
    test_smf_parser_delta_longest
    test_smf_parser_delta_too_long
    test_smf_parser_running_status
    test_smf_parser_running_status_dropped
    test_smf_parser_tempo_meta
    test_smf_parser_end_of_track_meta
    test_smf_parser_time_signature_meta
    test_smf_parser_unknown_meta
    PROPERTIES LABELS "sdk;samples"
)
