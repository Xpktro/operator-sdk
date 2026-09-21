# Host-side doctest suite for the User Scale (Output) sample.
#
# main.cpp is built into a static library (user_scale_output_host) with
# -fno-exceptions and -fno-rtti, the shape the mode has on the device, so the tests
# exercise it as it really runs. The test executable links that library plus
# operator_sdk_sim and adds back -fexceptions and -frtti for doctest. Each
# TEST_CASE has its own add_test() entry below and a matching name in
# set_tests_properties(), which is what ctest runs. The entries are labeled
# "sdk;samples".

add_library(user_scale_output_host STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_include_directories(user_scale_output_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_features(user_scale_output_host PUBLIC cxx_std_20)
target_compile_options(user_scale_output_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

add_executable(test_user_scale_output test_user_scale_output.cpp)
target_link_libraries(test_user_scale_output PRIVATE
    user_scale_output_host
    operator_sdk_sim
)
target_include_directories(test_user_scale_output PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(test_user_scale_output PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

add_test(NAME test_user_scale_output_shell
         COMMAND test_user_scale_output "--test-case=the shell hands every entry point to the shared body")

set_tests_properties(
    test_user_scale_output_shell
    PROPERTIES LABELS "sdk;samples"
)
