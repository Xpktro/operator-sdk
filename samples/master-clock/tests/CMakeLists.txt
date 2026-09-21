# Host-side doctest suite for the Master Clock sample.
#
# main.cpp is built into a static library (master_clock_host) with
# -fno-exceptions and -fno-rtti, the shape the mode has on the device, so the
# tests exercise it as it really runs. The test executable links that library plus
# operator_sdk_sim and adds back -fexceptions and -frtti for doctest. Each
# TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".

add_library(master_clock_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(master_clock_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(master_clock_host PUBLIC cxx_std_20)
target_compile_options(master_clock_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_master_clock test_master_clock.cpp)
target_link_libraries(test_master_clock PRIVATE
    master_clock_host
    operator_sdk_sim
)
target_include_directories(test_master_clock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include  # the SDK include comes via master_clock_host
)
target_compile_options(test_master_clock PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_master_clock_idle
         COMMAND test_master_clock "--test-case=a tick with no new pulse sends nothing")
add_test(NAME test_master_clock_pulse_per_output
         COMMAND test_master_clock "--test-case=every pulse sends one clock message to each output")
add_test(NAME test_master_clock_counter_restart
         COMMAND test_master_clock "--test-case=the pulse counter restarting does not flood the outputs")
add_test(NAME test_master_clock_period
         COMMAND test_master_clock "--test-case=BPM sets the clock period")
add_test(NAME test_master_clock_tempo_change_silent
         COMMAND test_master_clock "--test-case=a tempo change sends no clock message of its own")
add_test(NAME test_master_clock_bypass
         COMMAND test_master_clock "--test-case=Bypass stops the clock")
add_test(NAME test_master_clock_bypass_resume
         COMMAND test_master_clock "--test-case=leaving Bypass resumes without a catch-up burst")
add_test(NAME test_master_clock_unload
         COMMAND test_master_clock "--test-case=unloading disarms the clock")

set_tests_properties(
    test_master_clock_idle
    test_master_clock_pulse_per_output
    test_master_clock_counter_restart
    test_master_clock_period
    test_master_clock_tempo_change_silent
    test_master_clock_bypass
    test_master_clock_bypass_resume
    test_master_clock_unload
    PROPERTIES LABELS "sdk;samples"
)
