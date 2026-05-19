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

// Set the CRT-style scanline density at runtime.
//   0 = OFF      no scanlines (smooth line-doubled output)
//   1 = LIGHT    a black line every 6 output rows  (~17%)
//   2 = MEDIUM   a black line every 4 output rows  (25%)
//   3 = HEAVY    a black line every 2 output rows  (50%, classic CRT)
// Rewrites the odd-indexed entries of the DMA source-pointer ring;
// each entry is a single 32-bit pointer written atomically, so no
// torn output during the transition. Safe to call from anywhere
// except inside an active DMA chain step.
void vga_output_set_scanlines(int level);
