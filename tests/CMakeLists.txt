# Host-side unit tests for the SDK's own code. They run on the host toolchain, so
# they check the logic without a device in the loop. The ARM link is checked by the
# build's own freestanding gate.

cmake_minimum_required(VERSION 3.28)

add_executable(test_freestanding_intrinsics test_freestanding_intrinsics.c)
target_compile_definitions(test_freestanding_intrinsics PRIVATE
    OPERATOR_SDK_HOST_TEST=1)
# The shim is compiled into this target with its names rewritten, so the host's own
# memcpy/memset/memmove are not clobbered at link time and the algorithm is what runs.
target_compile_options(test_freestanding_intrinsics PRIVATE
    -Dmemcpy=op_test_memcpy
    -Dmemset=op_test_memset
    -Dmemmove=op_test_memmove)
target_sources(test_freestanding_intrinsics PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src/freestanding_intrinsics.c)

add_test(NAME sdk_freestanding_intrinsics
         COMMAND test_freestanding_intrinsics)
set_tests_properties(sdk_freestanding_intrinsics PROPERTIES LABELS "sdk")

# --- freestanding single-precision math ---
# Checks expf, logf and powf against the host's <cmath> to within 1e-4 relative
# error over the domains a mode feeds them. The shim is compiled in under renamed
# symbols, so it does not clash with the host libc, and <cmath> is the reference.
add_executable(test_freestanding_math test_freestanding_math.cpp)
target_sources(test_freestanding_math PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src/freestanding_math.c)
target_compile_options(test_freestanding_math PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Dexpf=op_test_expf>
    $<$<COMPILE_LANGUAGE:C>:-Dlogf=op_test_logf>
    $<$<COMPILE_LANGUAGE:C>:-Dpowf=op_test_powf>
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
target_include_directories(test_freestanding_math PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../sim/include   # doctest.h
    ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_features(test_freestanding_math PRIVATE cxx_std_20)

add_test(NAME sdk_freestanding_math
         COMMAND test_freestanding_math)
set_tests_properties(sdk_freestanding_math PROPERTIES LABELS "sdk")

# --- clock interpolation math ---
# Drives op::sdk::detail::interpolate_step over synthetic pulse and tick streams.
# doctest.h comes from sim/include, and the host build wants -fexceptions and
# -frtti for doctest.
add_executable(test_clock_interpolator test_clock_interpolator.cpp)
target_include_directories(test_clock_interpolator PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../sim/include   # doctest.h
    ${CMAKE_CURRENT_LIST_DIR}/../include       # operator_sdk/timing.h + abi headers
)
target_compile_features(test_clock_interpolator PRIVATE cxx_std_20)
target_compile_options(test_clock_interpolator PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME clock_interp_steady_tempo
         COMMAND test_clock_interpolator "--test-case=steady tempo: frac sweeps monotonically between pulse edges")
add_test(NAME clock_interp_pulse_period
         COMMAND test_clock_interpolator "--test-case=pulse period: last_period publishes the median inter-pulse interval")
add_test(NAME clock_interp_cold_start
         COMMAND test_clock_interpolator "--test-case=cold start: first pulses return frac 0 with no divide-by-zero")
add_test(NAME clock_interp_tempo_change
         COMMAND test_clock_interpolator "--test-case=tempo change: median-of-3 period adapts within two pulses")
add_test(NAME clock_interp_stop_restart
         COMMAND test_clock_interpolator "--test-case=clock stop/restart: HeldStale freeze then fresh period re-derivation")
add_test(NAME clock_interp_idle_resume
         COMMAND test_clock_interpolator "--test-case=idle resume: NoClock and Mode both drop the stale ring on resume")
add_test(NAME clock_interp_no_clock_freeze
         COMMAND test_clock_interpolator "--test-case=no-clock freeze: NoClock and Mode states hold the last-known position")
add_test(NAME clock_interp_forward_clamp
         COMMAND test_clock_interpolator "--test-case=forward clamp: frac stays strictly below 1.0 and never snaps back")
add_test(NAME clock_interp_counter_wrap
         COMMAND test_clock_interpolator "--test-case=counter wrap: a pulse value below last_pulse re-syncs without a glitch")
add_test(NAME clock_interp_per_instance
         COMMAND test_clock_interpolator "--test-case=per-instance independence: two InterpState structs never interfere")

set_tests_properties(
    clock_interp_steady_tempo
    clock_interp_pulse_period
    clock_interp_cold_start
    clock_interp_tempo_change
    clock_interp_stop_restart
    clock_interp_idle_resume
    clock_interp_no_clock_freeze
    clock_interp_forward_clamp
    clock_interp_counter_wrap
    clock_interp_per_instance
    PROPERTIES LABELS "sdk"
)

# --- step boundary tracker ---
# StepTracker reads no clock and no ABI, so the cases feed it step indices and
# check the edge it reports, including the first observation that takes a step as a
# baseline without crossing into it.
add_executable(test_step_tracker test_step_tracker.cpp)
target_include_directories(test_step_tracker PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../sim/include   # doctest.h
    ${CMAKE_CURRENT_LIST_DIR}/../include       # operator_sdk/timing.h
)
target_compile_features(test_step_tracker PRIVATE cxx_std_20)
target_compile_options(test_step_tracker PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME step_tracker_first_observation
         COMMAND test_step_tracker "--test-case=the first observation is adopted and crosses nothing")
add_test(NAME step_tracker_step_zero
         COMMAND test_step_tracker "--test-case=step zero is adopted like any other first step")
add_test(NAME step_tracker_held_position
         COMMAND test_step_tracker "--test-case=a held position reports unchanged for as long as it holds")
add_test(NAME step_tracker_crossing
         COMMAND test_step_tracker "--test-case=a changed index crosses once and then holds")
add_test(NAME step_tracker_wrap
         COMMAND test_step_tracker "--test-case=a grid that wraps to zero crosses like any other move")
add_test(NAME step_tracker_backward_jump
         COMMAND test_step_tracker "--test-case=a backward jump crosses so a rewound transport keeps firing")
add_test(NAME step_tracker_skipped_step
         COMMAND test_step_tracker "--test-case=a skipped step crosses once for the step it lands on")
add_test(NAME step_tracker_reset
         COMMAND test_step_tracker "--test-case=reset arms the next observation to be adopted")
add_test(NAME step_tracker_independence
         COMMAND test_step_tracker "--test-case=two trackers on their own grids never interfere")

set_tests_properties(
    step_tracker_first_observation
    step_tracker_step_zero
    step_tracker_held_position
    step_tracker_crossing
    step_tracker_wrap
    step_tracker_backward_jump
    step_tracker_skipped_step
    step_tracker_reset
    step_tracker_independence
    PROPERTIES LABELS "sdk"
)

# --- held-note registry ---
# HeldNotes reads no clock and no ABI, so the cases press and release keys directly
# and check what it reports. The cases run both a payload-free registry and one
# carrying a float, so an empty and a real payload each go through the template.
add_executable(test_held_notes test_held_notes.cpp)
target_include_directories(test_held_notes PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../sim/include   # doctest.h
    ${CMAKE_CURRENT_LIST_DIR}/../include       # operator_sdk/held_notes.h
)
target_compile_features(test_held_notes PRIVATE cxx_std_20)
target_compile_options(test_held_notes PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME held_notes_lone_press
         COMMAND test_held_notes "--test-case=a lone press sounds and its release stops the note")
add_test(NAME held_notes_unknown_release
         COMMAND test_held_notes "--test-case=a release for a key that was never pressed is not found")
add_test(NAME held_notes_repeat_press
         COMMAND test_held_notes "--test-case=a repeat press of a held key keeps the pitch it was emitted on")
add_test(NAME held_notes_two_key_fold
         COMMAND test_held_notes "--test-case=two keys folding onto one note sound it once and stop it on the last release")
add_test(NAME held_notes_fold_release_order
         COMMAND test_held_notes "--test-case=the fold holds whichever of the two keys lifts first")
add_test(NAME held_notes_three_key_fold
         COMMAND test_held_notes "--test-case=three keys folding onto one note hold it until the last of them lifts")
add_test(NAME held_notes_two_ports
         COMMAND test_held_notes "--test-case=the same note on two ports is two notes")
add_test(NAME held_notes_two_channels
         COMMAND test_held_notes "--test-case=the same note on two channels is two notes")
add_test(NAME held_notes_capacity
         COMMAND test_held_notes "--test-case=a press past capacity still sounds and its release is not found")
add_test(NAME held_notes_newest_on_channel
         COMMAND test_held_notes "--test-case=newest_on_channel finds the last press on the channel whatever port it came from")
add_test(NAME held_notes_clear
         COMMAND test_held_notes "--test-case=clear leaves nothing held")
add_test(NAME held_notes_payload
         COMMAND test_held_notes "--test-case=a payload comes back exactly as it went in")

set_tests_properties(
    held_notes_lone_press
    held_notes_unknown_release
    held_notes_repeat_press
    held_notes_two_key_fold
    held_notes_fold_release_order
    held_notes_three_key_fold
    held_notes_two_ports
    held_notes_two_channels
    held_notes_capacity
    held_notes_newest_on_channel
    held_notes_clear
    held_notes_payload
    PROPERTIES LABELS "sdk"
)

# --- param visibility resolver ---
# The resolver maps a param's .watch and .parent references to their slots. This
# builds a table with the same display name repeated across sections and checks that
# each gated child finds its own sibling and section by identity, not by the first
# name that matches.
add_executable(test_param_visibility_resolver test_param_visibility_resolver.cpp)
target_include_directories(test_param_visibility_resolver PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../sim/include   # doctest.h
    ${CMAKE_CURRENT_LIST_DIR}/../include       # operator_sdk/params.h + abi headers
)
target_compile_features(test_param_visibility_resolver PRIVATE cxx_std_20)
target_compile_options(test_param_visibility_resolver PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME param_vis_bundle_shape
         COMMAND test_param_visibility_resolver "--test-case=shared-name collision: staged bundle has the expected 9-spec shape")
add_test(NAME param_vis_watcher_identity
         COMMAND test_param_visibility_resolver "--test-case=shared-name collision: each gated child watches its own sibling Enable")
add_test(NAME param_vis_child_parent_identity
         COMMAND test_param_visibility_resolver "--test-case=shared-name collision: each child's parent resolves to its own section")
add_test(NAME param_vis_enable_parent_identity
         COMMAND test_param_visibility_resolver "--test-case=shared-name collision: each Enable's parent resolves to its own section")
add_test(NAME param_vis_ungated_always_visible
         COMMAND test_param_visibility_resolver "--test-case=shared-name collision: an ungated watcher (the Enable itself) stays always-visible")
add_test(NAME param_vis_watcher_index_high
         COMMAND test_param_visibility_resolver "--test-case=watcher index: 36-spec table stages and the last group hits index >= 32")
add_test(NAME param_vis_watcher_index_identity
         COMMAND test_param_visibility_resolver "--test-case=watcher index: per-sibling identity holds across all 12 groups")

set_tests_properties(
    param_vis_bundle_shape
    param_vis_watcher_identity
    param_vis_child_parent_identity
    param_vis_enable_parent_identity
    param_vis_ungated_always_visible
    param_vis_watcher_index_high
    param_vis_watcher_index_identity
    PROPERTIES LABELS "sdk"
)

# --- the opt-in helper headers ---
# Behavior and value coverage for the helper headers a mode opts into. The two
# things a single test cannot express, that each header stands alone and that the
# umbrella does not pull them in, are checked below by the compile-only targets and
# the will-fail probes.
add_executable(test_sdk_helpers test_sdk_helpers.cpp)
target_include_directories(test_sdk_helpers PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../sim/include   # doctest.h
    ${CMAKE_CURRENT_LIST_DIR}/../include       # operator_sdk/{text,timing,midi,math,log,storage}.h
)
target_compile_features(test_sdk_helpers PRIVATE cxx_std_20)
target_compile_options(test_sdk_helpers PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)
add_test(NAME sdk_helpers COMMAND test_sdk_helpers)
set_tests_properties(sdk_helpers PROPERTIES LABELS "sdk")

# --- each header stands alone ---
# Each helper has to compile with nothing else from the SDK included, so the compile
# is the check: a header that grows an undeclared dependency fails to build. The
# flags are a mode's, -fno-exceptions and -fno-rtti.
foreach(h text timing midi log math storage held_notes)
    add_executable(sdk_selfcontained_${h} header_contract/only_${h}.cpp)
    target_include_directories(sdk_selfcontained_${h} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_features(sdk_selfcontained_${h} PRIVATE cxx_std_20)
    target_compile_options(sdk_selfcontained_${h} PRIVATE
        -fno-exceptions -fno-rtti)
    add_test(NAME sdk_selfcontained_${h} COMMAND sdk_selfcontained_${h})
    set_tests_properties(sdk_selfcontained_${h} PROPERTIES LABELS "sdk")
endforeach()

# All the helpers coexist in one translation unit without clashing.
add_executable(sdk_helpers_coexist header_contract/pair.cpp)
target_include_directories(sdk_helpers_coexist PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_features(sdk_helpers_coexist PRIVATE cxx_std_20)
target_compile_options(sdk_helpers_coexist PRIVATE -fno-exceptions -fno-rtti)
add_test(NAME sdk_helpers_coexist COMMAND sdk_helpers_coexist)
set_tests_properties(sdk_helpers_coexist PROPERTIES LABELS "sdk")

# The umbrella has to compile on its own, or the will-fail probes below would pass
# for the wrong reason.
add_executable(sdk_umbrella_compiles header_contract/umbrella_compiles.cpp)
target_include_directories(sdk_umbrella_compiles PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_features(sdk_umbrella_compiles PRIVATE cxx_std_20)
add_test(NAME sdk_umbrella_compiles COMMAND sdk_umbrella_compiles)
set_tests_properties(sdk_umbrella_compiles PROPERTIES LABELS "sdk")

# --- the umbrella does not leak the helpers ---
# A file that includes only <operator_sdk.h> cannot name a helper symbol, since the
# umbrella does not pull the opt-in helpers in and a mode links only what it uses.
# Each probe references one helper symbol, and WILL_FAIL means a compile that
# succeeds, the umbrella having leaked it, fails the test.
foreach(h text timing midi)
    add_test(NAME sdk_umbrella_excludes_${h}
             COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only
                     -I${CMAKE_CURRENT_LIST_DIR}/../include
                     ${CMAKE_CURRENT_LIST_DIR}/header_contract/umbrella_uses_${h}.cpp)
    set_tests_properties(sdk_umbrella_excludes_${h} PROPERTIES
        WILL_FAIL TRUE LABELS "sdk")
endforeach()
