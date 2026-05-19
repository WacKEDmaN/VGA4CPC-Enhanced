#pragma once
#include <stdbool.h>

// Initialise PIO0 with hsync/vsync/rgb state machines (50 or 60 Hz mode)
// and configure the 4-channel DMA chain that streams framebuffer lines
// to the RGB SM's TX FIFO. Lines are line-doubled (each CPC line shown
// twice on VGA), so the chain cycles through framebuf in a 1024-step
// power-of-2 ring.
void vga_output_init(bool is_50hz);

// Enable the SMs and trigger the DMA chain. Call after vga_output_init().
void vga_output_start(void);

// Toggle the CRT-style scanlines effect on or off at runtime. Rewrites
// the odd-indexed entries of the DMA source-pointer ring; the swap is
// effectively atomic per slot, so there's no torn output during the
// transition. Safe to call from anywhere except inside an active DMA
// chain step.
void vga_output_set_scanlines(bool enabled);
