# --------------------------------------------------------------------------
# Host-side doctest suites for the mock device.
#
# Both executables link operator_sdk_sim. The library is compiled -fno-exceptions
# and -fno-rtti, the same restriction the device puts on a mode, and the test
# executables turn -fexceptions and -frtti back on for doctest's assertion macros.
# Each TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled "sdk".
#
# The --test-case= value is quoted, so a title carrying a space still matches the
# case it names.
#
# Two suites live here. test_sim_harness drives the mock OperatorApi, and
# test_framebuffer_render covers the ASCII and SVG renderers behind it.
# --------------------------------------------------------------------------

add_executable(test_sim_harness test_sim_harness.cpp)
target_link_libraries(test_sim_harness PRIVATE operator_sdk_sim)
target_include_directories(test_sim_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # doctest.h + operator_sdk_sim.h
)
target_compile_options(test_sim_harness PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME sim_harness_send_and_drain
         COMMAND test_sim_harness "--test-case=a sent message comes back out of the output it was sent to")
add_test(NAME sim_harness_tick
         COMMAND test_sim_harness "--test-case=the tick accumulates the microseconds it is advanced by")
add_test(NAME sim_harness_framebuffer
         COMMAND test_sim_harness "--test-case=a draw lands in the framebuffer that is captured")
add_test(NAME sim_harness_storage
         COMMAND test_sim_harness "--test-case=a seeded file reads back through storage")
add_test(NAME sim_harness_clock_state
         COMMAND test_sim_harness "--test-case=the clock state reads back what it is set to")
add_test(NAME sim_harness_pulse_count
         COMMAND test_sim_harness "--test-case=the pulse count reads back in pulses and in beats")
add_test(NAME sim_harness_filename_empty
         COMMAND test_sim_harness "--test-case=a picked filename is empty until one is picked")
add_test(NAME sim_harness_filename_reads_back
         COMMAND test_sim_harness "--test-case=a picked filename reads back")
add_test(NAME sim_harness_filename_per_slot
         COMMAND test_sim_harness "--test-case=each param slot holds its own picked filename")
add_test(NAME sim_harness_filename_truncates
         COMMAND test_sim_harness "--test-case=a picked filename longer than the buffer is truncated and terminated")
add_test(NAME sim_harness_filename_no_buffer
         COMMAND test_sim_harness "--test-case=a picked filename read with no buffer to read into fails")
add_test(NAME sim_harness_filename_cleared
         COMMAND test_sim_harness "--test-case=clearing a picked filename empties the slot")
add_test(NAME sim_harness_output_inactive
         COMMAND test_sim_harness "--test-case=an output taken away refuses a send")
add_test(NAME sim_harness_clock_period
         COMMAND test_sim_harness "--test-case=the last requested clock period reads back")
add_test(NAME sim_harness_param_defaults
         COMMAND test_sim_harness "--test-case=declared param defaults are seeded into their slots")

set_tests_properties(
    sim_harness_send_and_drain
    sim_harness_tick
    sim_harness_framebuffer
    sim_harness_storage
    sim_harness_clock_state
    sim_harness_pulse_count
    sim_harness_filename_empty
    sim_harness_filename_reads_back
    sim_harness_filename_per_slot
    sim_harness_filename_truncates
    sim_harness_filename_no_buffer
    sim_harness_filename_cleared
    sim_harness_output_inactive
    sim_harness_clock_period
    sim_harness_param_defaults
    PROPERTIES LABELS "sdk"
)

# --------------------------------------------------------------------------
# The framebuffer renderers (op::sim::to_ascii / to_svg and the MockDisplay
# wrappers). These reach the harness's private src/ headers directly, so that
# directory is on the include path.
# --------------------------------------------------------------------------
add_executable(test_framebuffer_render test_framebuffer_render.cpp)
target_link_libraries(test_framebuffer_render PRIVATE operator_sdk_sim)
target_include_directories(test_framebuffer_render PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # doctest.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src       # mock_display.h + framebuffer_render.h
)
target_compile_options(test_framebuffer_render PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME framebuffer_render_ascii_golden
         COMMAND test_framebuffer_render "--test-case=render_ascii_golden")
add_test(NAME framebuffer_render_ascii_wrapper
         COMMAND test_framebuffer_render "--test-case=render_ascii_wrapper_matches_free_fn")
add_test(NAME framebuffer_render_svg_empty
         COMMAND test_framebuffer_render "--test-case=render_svg_empty_has_no_rect")
add_test(NAME framebuffer_render_svg_run
         COMMAND test_framebuffer_render "--test-case=render_svg_run_length_rect")
add_test(NAME framebuffer_render_svg_scale
         COMMAND test_framebuffer_render "--test-case=render_svg_scale_multiplies_dimensions")

set_tests_properties(
    framebuffer_render_ascii_golden
    framebuffer_render_ascii_wrapper
    framebuffer_render_svg_empty
    framebuffer_render_svg_run
    framebuffer_render_svg_scale
    PROPERTIES LABELS "sdk"
)
