cmake_minimum_required(VERSION 3.13)

# =====================================================================
# VGA4CPC-Enhanced — direct CPC→VGA scan doubler (no pico-extras).
#
# Custom PIO programs handle VGA timing (hsync/vsync/rgb) directly.
# This gives us full bandwidth at the DAC and avoids the bottlenecks
# of pico-extras scanvideo's composable-opcode pipeline.
#
# Pin mapping (PCB fixed — do not change):
#   GPIO 0-5  = CPC RGB input (B_LO, B_HI, G_LO, G_HI, R_LO, R_HI)
#   GPIO 6    = CSYNC input (active low)
#   GPIO 10   = vsyncgen output (CSYNC → VSYNC discriminator)
#   GPIO 12   = HSYNC output
#   GPIO 13   = VSYNC output
#   GPIO 14-19= 6-bit R-2R DAC
# =====================================================================

set(PICO_SDK_PATH
    "C:/Program Files/Raspberry Pi/Pico SDK v1.5.1/pico-sdk"
    CACHE PATH "Pico SDK path")

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(vga4cpc_enhanced C CXX ASM)
set(CMAKE_C_STANDARD 11)

# Single firmware. Scanlines are now a runtime option — press the
# Pico's BOOTSEL button to toggle them on/off while running.

pico_sdk_init()

add_executable(vga4cpc_enhanced
    src/main.c
    src/framebuffer.c
    src/capture.c
    src/vga_output.c
)

# CPC capture PIO programs
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/rgbin.pio)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/vsyncgen.pio)
# VGA output PIO programs (50 Hz and 60 Hz variants)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/hsync_50.pio)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/vsync_50.pio)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/rgb_50.pio)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/hsync_60.pio)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/vsync_60.pio)
pico_generate_pio_header(vga4cpc_enhanced ${CMAKE_CURRENT_LIST_DIR}/src/rgb_60.pio)

target_include_directories(vga4cpc_enhanced PRIVATE src)
target_compile_options(vga4cpc_enhanced PRIVATE -O2)

target_link_libraries(vga4cpc_enhanced PRIVATE
    pico_stdlib
    hardware_dma
    hardware_flash
    hardware_pio
    hardware_pwm
    hardware_irq
    hardware_timer
    hardware_clocks
)

pico_add_extra_outputs(vga4cpc_enhanced)
