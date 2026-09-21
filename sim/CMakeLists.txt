# --------------------------------------------------------------------------
# operator_sdk_sim, the mock device a mode is tested against.
#
# It builds from the mocks (mock_display, mock_store, mock_font) and the
# public headers, and nothing else. There is no device backend to link and no
# graphics dependency, so it builds and runs anywhere a C++20 host compiler does.
# --------------------------------------------------------------------------

add_library(operator_sdk_sim STATIC
    src/mock_operator_api.cpp
    src/framebuffer_capture.cpp
    src/framebuffer_render.cpp
    src/mock_display.cpp
    src/mock_store.cpp
    src/mock_font.cpp
)

target_include_directories(operator_sdk_sim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include     # <operator_sdk.h>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src            # harness_internal.h + mocks
)

target_compile_features(operator_sdk_sim PUBLIC cxx_std_20)
# The device compiles a mode with no exceptions and no RTTI, so the mock is built
# the same way and a mode meets the same flags under test.
target_compile_options(operator_sdk_sim PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

# The tests carry the ctest label "sdk".
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
