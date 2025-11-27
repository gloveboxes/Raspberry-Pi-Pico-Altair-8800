cmake_minimum_required(VERSION 3.13)

# 💡 Select board BEFORE importing SDK (safer)
set(PICO_BOARD pico2_w)

# Import Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(altair C CXX ASM)
pico_sdk_init()

add_executable(altair
    main.c
    Altair8800/intel8080.c
    Altair8800/memory.c
    Altair8800/pico_disk.c
)

# Make sure all CYW43 headers are visible and include Altair8800 directory
target_include_directories(altair PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Altair8800
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/include
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/include
    ${PICO_SDK_PATH}/lib/cyw43-driver/src        # 👈 this one is for cyw43.h
)

# USB stdio on, UART stdio off
pico_enable_stdio_usb(altair 1)
pico_enable_stdio_uart(altair 0)

# Link stdlib + CYW43 (for onboard LED)
target_link_libraries(altair
    pico_stdlib
    pico_cyw43_arch_none
)

# Generate .uf2, .elf, etc.
pico_add_extra_outputs(altair)