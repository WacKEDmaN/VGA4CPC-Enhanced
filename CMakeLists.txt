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

# ---------------------------------------------------------------------
# Build variants
#
#   -DSCANLINES=ON | OFF   (default OFF)
#
# Examples:
#   cmake -G Ninja ..                       # NORMAL
#   cmake -G Ninja -DSCANLINES=ON ..        # NORMAL + CRT scanlines
# ---------------------------------------------------------------------
option(SCANLINES "Enable CRT-style scanlines" OFF)

if(SCANLINES)
    set(VARIANT_NAME "NORMAL_SCANLINES")
else()
    set(VARIANT_NAME "NORMAL")
endif()
message(STATUS "Building VGA4CPC-Enhanced variant: ${VARIANT_NAME}")

pico_sdk_init()

add_executable(vga4cpc_enhanced
    src/main.c
    src/framebuffer.c
    src/capture.c
    src/vga_output.c
)

if(SCANLINES)
    target_compile_definitions(vga4cpc_enhanced PRIVATE WITH_SCANLINES=1)
endif()

# Rename output binary to reflect variant
set_target_properties(vga4cpc_enhanced PROPERTIES
    OUTPUT_NAME "vga4cpc_enhanced_${VARIANT_NAME}"
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
    hardware_pio
    hardware_irq
    hardware_timer
    hardware_clocks
)

pico_add_extra_outputs(vga4cpc_enhanced)
