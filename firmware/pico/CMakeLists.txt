cmake_minimum_required(VERSION 3.13)

# Needs the Pico SDK: set PICO_SDK_PATH to a checkout of
# https://github.com/raspberrypi/pico-sdk (with submodules), or pass
# -DPICO_SDK_FETCH_FROM_GIT=ON to have CMake fetch it into the build folder.
include(pico_sdk_import.cmake)

project(claude_pad C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(claude_pad
    main.c
    ../src/pad_fw.c
    ../../Sources/PadCore/core.c   # the same key names and numbering the Mac daemon uses
)

pico_generate_pio_header(claude_pad ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_include_directories(claude_pad PRIVATE
    ../src
    ../../Sources/PadCore/include
)
target_compile_options(claude_pad PRIVATE -Wall -Wextra)
# On ARM, enums are unsigned, so core.c's range checks like `k >= 0` trip
# -Wtype-limits. They're correct (and needed on other compilers), so quiet
# the warning for that one file rather than editing it.
set_source_files_properties(../../Sources/PadCore/core.c PROPERTIES COMPILE_OPTIONS -Wno-type-limits)
target_link_libraries(claude_pad PRIVATE pico_stdlib hardware_pio)

# Serial goes out over both USB (the Mac) and UART0 on GP0/GP1 (Wokwi, or a
# USB-serial adapter for debugging).
pico_enable_stdio_usb(claude_pad 1)
pico_enable_stdio_uart(claude_pad 1)

pico_add_extra_outputs(claude_pad) # also writes claude_pad.uf2, which you drag onto the board
