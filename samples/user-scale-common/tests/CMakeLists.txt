# Host-side doctest suites for the User Scale shared code.
#
# Two suites live here. test_user_scale_logic unit-tests the scale math in
# user_scale.h, which needs no mode, so it includes the header and links only
# doctest. test_user_scale_body drives the whole shared body, so it needs a mode to
# drive: user_scale_test_mode.cpp wires the body up as one and is built into a
# static library (user_scale_body_host) with -fno-exceptions and -fno-rtti, the
# shape the mode has on the device. Both executables add back -fexceptions and
# -frtti for doctest.
#
# The two User Scale modes build on these headers, so a break here shows up in
# both. Each TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".
#
# The sprites carry no suite of their own. Their sizes are checked where they are
# declared and their shapes are the master image.

add_executable(test_user_scale_logic test_user_scale_logic.cpp)
target_include_directories(test_user_scale_logic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # doctest.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(test_user_scale_logic PRIVATE cxx_std_20)
target_compile_options(test_user_scale_logic PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_library(user_scale_body_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/user_scale_test_mode.cpp
)
target_include_directories(user_scale_body_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(user_scale_body_host PUBLIC cxx_std_20)
target_compile_options(user_scale_body_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_user_scale_body test_user_scale_body.cpp)
target_link_libraries(test_user_scale_body PRIVATE
    user_scale_body_host
    operator_sdk_sim
)
target_include_directories(test_user_scale_body PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(test_user_scale_body PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_user_scale_logic_round_up
         COMMAND test_user_scale_logic "--test-case=a note rounds up to the nearest enabled degree")
add_test(NAME test_user_scale_logic_octave_wrap
         COMMAND test_user_scale_logic "--test-case=a note above every enabled degree wraps into the next octave")
add_test(NAME test_user_scale_logic_nothing_enabled
         COMMAND test_user_scale_logic "--test-case=a scale with nothing enabled passes notes through")
add_test(NAME test_user_scale_logic_chromatic
         COMMAND test_user_scale_logic "--test-case=a chromatic scale lands every note where it started")
add_test(NAME test_user_scale_logic_root_turns_scale
         COMMAND test_user_scale_logic "--test-case=Root turns C major into D major")
add_test(NAME test_user_scale_logic_transpose
         COMMAND test_user_scale_logic "--test-case=Transpose shifts the output whatever the Root")
add_test(NAME test_user_scale_logic_preset_load
         COMMAND test_user_scale_logic "--test-case=a preset loads as the scale it is written as")
add_test(NAME test_user_scale_logic_preset_table
         COMMAND test_user_scale_logic "--test-case=every preset has a distinct shape and a name that fits")

add_test(NAME test_user_scale_body_scale_from_preset
         COMMAND test_user_scale_body "--test-case=the notes come out in the scale CurrentScale names")
add_test(NAME test_user_scale_body_scale_from_custom
         COMMAND test_user_scale_body "--test-case=a scale the player drew is read out of CustomScale")
add_test(NAME test_user_scale_body_init_seed
         COMMAND test_user_scale_body "--test-case=init opens on the scale the params were left holding")
add_test(NAME test_user_scale_body_live_params
         COMMAND test_user_scale_body "--test-case=the params are read fresh on every dispatch")
add_test(NAME test_user_scale_body_pentatonic
         COMMAND test_user_scale_body "--test-case=the notes come out in a pentatonic minor scale")
add_test(NAME test_user_scale_body_two_key_fold
         COMMAND test_user_scale_body "--test-case=two keys folding onto one note sound it once and hold it")
add_test(NAME test_user_scale_body_fold_release_order
         COMMAND test_user_scale_body "--test-case=the fold holds whichever of the two keys lifts first")
add_test(NAME test_user_scale_body_three_key_fold
         COMMAND test_user_scale_body "--test-case=three keys folding in a pentatonic scale hold the note to the last release")
add_test(NAME test_user_scale_body_unknown_release
         COMMAND test_user_scale_body "--test-case=a note-off for a key that was never pressed still comes out mapped")
add_test(NAME test_user_scale_body_root_moves_under_hold
         COMMAND test_user_scale_body "--test-case=a Root change under a held key leaves the release on the pitch that sounded")
add_test(NAME test_user_scale_body_two_inputs
         COMMAND test_user_scale_body "--test-case=the same note held on two inputs sounds and stops on each of them")
add_test(NAME test_user_scale_body_screen_opens
         COMMAND test_user_scale_body "--test-case=the screen opens on the title and the keyboard")
add_test(NAME test_user_scale_body_title_focus
         COMMAND test_user_scale_body "--test-case=the title takes focus with an underline and holds its place")
add_test(NAME test_user_scale_body_bottom_row
         COMMAND test_user_scale_body "--test-case=the bottom row keeps its labels where they are")
add_test(NAME test_user_scale_body_scroll_fields
         COMMAND test_user_scale_body "--test-case=Scroll moves between the fields and comes round again")
add_test(NAME test_user_scale_body_key_cursor
         COMMAND test_user_scale_body "--test-case=Change moves the key cursor and comes round again")
add_test(NAME test_user_scale_body_cycle_scale
         COMMAND test_user_scale_body "--test-case=Change on the title cycles the scale")
add_test(NAME test_user_scale_body_edit_copies_preset
         COMMAND test_user_scale_body "--test-case=editing a preset copies it into the custom scale")
add_test(NAME test_user_scale_body_custom_survives
         COMMAND test_user_scale_body "--test-case=a custom scale survives a cycle away and back")
add_test(NAME test_user_scale_body_reset_field
         COMMAND test_user_scale_body "--test-case=Enter puts a focused field back where it started")
add_test(NAME test_user_scale_body_keyboard_as_it_sounds
         COMMAND test_user_scale_body "--test-case=the keyboard draws the scale as it sounds")
add_test(NAME test_user_scale_body_edit_flips_drawn_note
         COMMAND test_user_scale_body "--test-case=editing a key flips the note it draws")
add_test(NAME test_user_scale_body_back_turns_view
         COMMAND test_user_scale_body "--test-case=Back turns the keyboard between the two views")
add_test(NAME test_user_scale_body_playing_follows_root
         COMMAND test_user_scale_body "--test-case=the playing follows Root in either keyboard view")
add_test(NAME test_user_scale_body_root_view_edit
         COMMAND test_user_scale_body "--test-case=with the keyboard drawn from its root an edit flips the key itself")

set_tests_properties(
    test_user_scale_logic_round_up
    test_user_scale_logic_octave_wrap
    test_user_scale_logic_nothing_enabled
    test_user_scale_logic_chromatic
    test_user_scale_logic_root_turns_scale
    test_user_scale_logic_transpose
    test_user_scale_logic_preset_load
    test_user_scale_logic_preset_table
    test_user_scale_body_scale_from_preset
    test_user_scale_body_scale_from_custom
    test_user_scale_body_init_seed
    test_user_scale_body_live_params
    test_user_scale_body_pentatonic
    test_user_scale_body_two_key_fold
    test_user_scale_body_fold_release_order
    test_user_scale_body_three_key_fold
    test_user_scale_body_unknown_release
    test_user_scale_body_root_moves_under_hold
    test_user_scale_body_two_inputs
    test_user_scale_body_screen_opens
    test_user_scale_body_title_focus
    test_user_scale_body_bottom_row
    test_user_scale_body_scroll_fields
    test_user_scale_body_key_cursor
    test_user_scale_body_cycle_scale
    test_user_scale_body_edit_copies_preset
    test_user_scale_body_custom_survives
    test_user_scale_body_reset_field
    test_user_scale_body_keyboard_as_it_sounds
    test_user_scale_body_edit_flips_drawn_note
    test_user_scale_body_back_turns_view
    test_user_scale_body_playing_follows_root
    test_user_scale_body_root_view_edit
    PROPERTIES LABELS "sdk;samples"
)
