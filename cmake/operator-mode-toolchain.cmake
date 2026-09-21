# Operator SDK Cortex-M33 PIC toolchain.
#
# Use this file as CMAKE_TOOLCHAIN_FILE when cross-compiling Operator mode
# binaries. It selects arm-none-eabi-gcc / g++ and applies the exact flag set
# the firmware loader expects:
#
#   -mcpu=cortex-m33 -mthumb                           (Cortex-M33 thumb)
#   -mfloat-abi=softfp -mfpu=fpv5-sp-d16               (FPU in-function, soft-float boundary)
#   -fPIE -msingle-pic-base                            (position-independent executable)
#   -mpic-register=r9 -mno-pic-data-is-text-relative   (GOT base kept in r9)
#   -fno-exceptions -fno-rtti                          (zero-overhead C++, no unwind)
#   -ffreestanding -ffunction-sections -fdata-sections (gc-sections friendly)
#   -Os                                                (optimize for Operator-sized pool)
#
# Usage (from a mode's CMakeLists.txt):
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$ENV{OPERATOR_SDK}/cmake/operator-mode-toolchain.cmake
#
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

add_compile_options(
    -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16
    # A mode compiles C sources alongside its C++ ones, so the C++-only flags take a
    # language scope.
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    -fno-exceptions
    -ffreestanding -ffunction-sections -fdata-sections
    -Os
    -fPIE -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative
    -Wall -Wextra
)
