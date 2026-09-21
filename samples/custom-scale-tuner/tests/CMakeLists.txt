# Host-side doctest suite for the Custom Scale Tuner sample.
#
# main.cpp is built into a static library (custom_scale_tuner_host) with
# -fno-exceptions and -fno-rtti, the shape the mode has on the device, so the
# tests exercise it as it really runs. Each test executable links that library
# plus operator_sdk_sim and adds back -fexceptions and -frtti for doctest. Each
# TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".
#
# Three suites live here. test_scala_parser covers the header-only .scl parser
# corner cases, test_retune covers the integer-math retune helpers, and
# test_custom_scale_tuner covers full-mode behavior on a single output.

# --- Shared host library (for test_custom_scale_tuner) --------------------
add_library(custom_scale_tuner_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(custom_scale_tuner_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(custom_scale_tuner_host PUBLIC cxx_std_20)
target_compile_options(custom_scale_tuner_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

# --- test_scala_parser (header-only parser corner cases) ------------------
add_executable(test_scala_parser test_scala_parser.cpp)
target_include_directories(test_scala_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # doctest.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include      # <operator_sdk.h> (for retune.h deps)
)
target_compile_features(test_scala_parser PRIVATE cxx_std_20)
target_compile_options(test_scala_parser PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_scala_parser_12tet
         COMMAND test_scala_parser "--test-case=12-tet scale")
add_test(NAME test_scala_parser_ratio
         COMMAND test_scala_parser "--test-case=ratio scale")
add_test(NAME test_scala_parser_bom
         COMMAND test_scala_parser "--test-case=UTF-8 BOM stripped")
add_test(NAME test_scala_parser_crlf
         COMMAND test_scala_parser "--test-case=CRLF line endings")
add_test(NAME test_scala_parser_comments
         COMMAND test_scala_parser "--test-case=comment lines skipped")
add_test(NAME test_scala_parser_bad_count
         COMMAND test_scala_parser "--test-case=invalid count rejected")
add_test(NAME test_scala_parser_bad_pitch
         COMMAND test_scala_parser "--test-case=malformed pitch rejected")
add_test(NAME test_scala_parser_cents
         COMMAND test_scala_parser "--test-case=entry_to_cents_x1000 cents path")
add_test(NAME test_scala_parser_ratio_math
         COMMAND test_scala_parser "--test-case=entry_to_cents_x1000 ratio path")
add_test(NAME test_scala_parser_cents_100_5
         COMMAND test_scala_parser "--test-case=cents 100.5 parses to cents_x1000 == 100500")
add_test(NAME test_scala_parser_ratio_3_2_tol
         COMMAND test_scala_parser "--test-case=ratio 3/2 returns cents_x1000 within tolerance of 701955")
add_test(NAME test_scala_parser_ratio_2_1_exact
         COMMAND test_scala_parser "--test-case=ratio 2/1 returns cents_x1000 == 1200000 exactly")
add_test(NAME test_scala_parser_ratio_1_1_zero
         COMMAND test_scala_parser "--test-case=ratio 1/1 (bare integer 1) parses to cents_x1000 == 0")
add_test(NAME test_scala_parser_direct_cents
         COMMAND test_scala_parser "--test-case=entry_to_cents_x1000 with direct cents entry")
add_test(NAME test_scala_parser_ratio_9_8_tol
         COMMAND test_scala_parser "--test-case=entry_to_cents_x1000 ratio 9/8 within tolerance of 203910")
add_test(NAME test_scala_parser_neg_cents
         COMMAND test_scala_parser "--test-case=negative cents -50.25 parses to cents_x1000 == -50250")
add_test(NAME test_scala_parser_truncation
         COMMAND test_scala_parser "--test-case=parse_cents_x1000 handles truncation beyond 3 fractional digits")

set_tests_properties(
    test_scala_parser_12tet
    test_scala_parser_ratio
    test_scala_parser_bom
    test_scala_parser_crlf
    test_scala_parser_comments
    test_scala_parser_bad_count
    test_scala_parser_bad_pitch
    test_scala_parser_cents
    test_scala_parser_ratio_math
    test_scala_parser_cents_100_5
    test_scala_parser_ratio_3_2_tol
    test_scala_parser_ratio_2_1_exact
    test_scala_parser_ratio_1_1_zero
    test_scala_parser_direct_cents
    test_scala_parser_ratio_9_8_tol
    test_scala_parser_neg_cents
    test_scala_parser_truncation
    PROPERTIES LABELS "sdk;samples"
)

# --- test_retune (integer-math retune + MPE helpers) ---------------------
add_executable(test_retune test_retune.cpp)
target_include_directories(test_retune PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # doctest.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include      # <operator_sdk.h> (retune.h deps)
)
target_compile_features(test_retune PRIVATE cxx_std_20)
target_compile_options(test_retune PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_retune_nearest_12tet
         COMMAND test_retune "--test-case=nearest_scale_note 12-TET returns zero offset")
add_test(NAME test_retune_nearest_100_5
         COMMAND test_retune "--test-case=nearest_scale_note 100.5c offset is +0.5 cents")
add_test(NAME test_retune_bend_zero
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit integer 0 cents = 8192")
add_test(NAME test_retune_bend_full_up
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit integer +200k cents full up")
add_test(NAME test_retune_bend_full_down
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit integer -200k cents full down")
add_test(NAME test_retune_bend_exact_50k
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit exact 50k/200k case = 10240")
add_test(NAME test_retune_bend_rounding
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit rounds half-away-from-zero")
add_test(NAME test_retune_bend_clamp
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit clamps outside range")
add_test(NAME test_retune_bend_float_overload
         COMMAND test_retune "--test-case=cents_to_pitch_bend_14bit float overload matches the integer primary")
add_test(NAME test_retune_hz_root
         COMMAND test_retune "--test-case=hz_from_note 12-TET root returns exact root freq")
add_test(NAME test_retune_hz_octave
         COMMAND test_retune "--test-case=hz_from_note 12-TET octave within 0.5 Hz")
add_test(NAME test_retune_mpe_round_robin
         COMMAND test_retune "--test-case=MPE allocate round-robin across member channels")
add_test(NAME test_retune_mpe_release
         COMMAND test_retune "--test-case=MPE release frees channel")
add_test(NAME test_retune_mpe_find
         COMMAND test_retune "--test-case=MPE find returns correct channel for allocated note")
add_test(NAME test_retune_mpe_full
         COMMAND test_retune "--test-case=MPE allocate returns sentinel when full")

set_tests_properties(
    test_retune_nearest_12tet
    test_retune_nearest_100_5
    test_retune_bend_zero
    test_retune_bend_full_up
    test_retune_bend_full_down
    test_retune_bend_exact_50k
    test_retune_bend_rounding
    test_retune_bend_clamp
    test_retune_bend_float_overload
    test_retune_hz_root
    test_retune_hz_octave
    test_retune_mpe_round_robin
    test_retune_mpe_release
    test_retune_mpe_find
    test_retune_mpe_full
    PROPERTIES LABELS "sdk;samples"
)

# --- test_custom_scale_tuner (full-mode behavior) -------------------------
add_executable(test_custom_scale_tuner test_custom_scale_tuner.cpp)
target_link_libraries(test_custom_scale_tuner PRIVATE
    custom_scale_tuner_host
    operator_sdk_sim
)
target_include_directories(test_custom_scale_tuner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(test_custom_scale_tuner PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_custom_scale_tuner_single_out
         COMMAND test_custom_scale_tuner "--test-case=retunes single output only")
add_test(NAME test_custom_scale_tuner_malformed
         COMMAND test_custom_scale_tuner "--test-case=rejects malformed .scl")
add_test(NAME test_custom_scale_tuner_mpe
         COMMAND test_custom_scale_tuner "--test-case=MPE mode uses member channels on single output")
add_test(NAME test_custom_scale_tuner_folded_key_keeps_bend
         COMMAND test_custom_scale_tuner "--test-case=a folded second key leaves the first note's bend standing")
add_test(NAME test_custom_scale_tuner_final_note_off_no_bend
         COMMAND test_custom_scale_tuner "--test-case=non-MPE final note-off clears channel and emits no trailing bend")
add_test(NAME test_custom_scale_tuner_nine_notes_one_channel
         COMMAND test_custom_scale_tuner "--test-case=nine notes held on one channel all sound and all stop")
add_test(NAME test_custom_scale_tuner_two_key_fold
         COMMAND test_custom_scale_tuner "--test-case=two keys retuning to one note sound it once and stop it on the last release")
add_test(NAME test_custom_scale_tuner_bend_restore
         COMMAND test_custom_scale_tuner "--test-case=a note-off restores the bend of a note still held on the channel")
add_test(NAME test_custom_scale_tuner_unknown_release
         COMMAND test_custom_scale_tuner "--test-case=a note-off for a key that was never pressed still emits")
add_test(NAME test_custom_scale_tuner_root_key_under_hold
         COMMAND test_custom_scale_tuner "--test-case=a Root Key change under a held note leaves the note-off on the pitch that sounded")

add_test(NAME test_custom_scale_tuner_suppress_note_on
         COMMAND test_custom_scale_tuner "--test-case=non-MPE note-on is consumed and emits exactly one retuned note")
add_test(NAME test_custom_scale_tuner_suppress_note_off
         COMMAND test_custom_scale_tuner "--test-case=non-MPE note-off is consumed and emits exactly one retuned note-off")
add_test(NAME test_custom_scale_tuner_suppress_pitch_wheel
         COMMAND test_custom_scale_tuner "--test-case=pitch wheel is composed with the retune and the raw wheel is consumed")
add_test(NAME test_custom_scale_tuner_passthrough_via_drain
         COMMAND test_custom_scale_tuner "--test-case=non-note messages are left intact for pass-through")

set_tests_properties(
    test_custom_scale_tuner_single_out
    test_custom_scale_tuner_malformed
    test_custom_scale_tuner_mpe
    test_custom_scale_tuner_folded_key_keeps_bend
    test_custom_scale_tuner_final_note_off_no_bend
    test_custom_scale_tuner_nine_notes_one_channel
    test_custom_scale_tuner_two_key_fold
    test_custom_scale_tuner_bend_restore
    test_custom_scale_tuner_unknown_release
    test_custom_scale_tuner_root_key_under_hold
    test_custom_scale_tuner_suppress_note_on
    test_custom_scale_tuner_suppress_note_off
    test_custom_scale_tuner_suppress_pitch_wheel
    test_custom_scale_tuner_passthrough_via_drain
    PROPERTIES LABELS "sdk;samples"
)
