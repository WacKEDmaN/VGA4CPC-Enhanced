#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hardware_config.h"

// Framebuffer storage exposed for DMA configuration (vga_dma_setup
// needs the raw base pointer to build per-line source addresses).
extern uint8_t framebuf[FB_H][FB_STRIDE];

// =====================================================================
// Single-buffer framebuffer: 800 × 288 bytes (1 byte = 2-2-2 DAC value).
// Writer: capture DMA (Core 0, one row per CPC line).
// Reader: VGA output 4-channel DMA chain (each row read twice in a row
//         for scan-doubled output).
// =====================================================================

void fb_init(void);

// Paint the "NO SIGNAL / PLEASE STAND BY" test card into the framebuffer.
// Called once at boot (so the user has something to look at before the
// CPC connects) and again by the capture loop whenever the CPC signal
// has been absent for more than a few seconds.
//
// Layout (top to bottom, totalling FB_H = 288 rows):
//   "NO SIGNAL" banner → castellation row → 6 colour bars → frequency
//   burst (8 stripe-density groups) → 4-step greyscale ramp →
//   "VGA4CPC-ENHANCED" banner → yellow strip with central red marker.
void fb_paint_test_pattern(void);

// Write pointer for line n (0..FB_H-1). Returns NULL if out of range.
uint8_t       *fb_get_write_line(uint16_t n);

// Mark line n ready for the display path to read.
void           fb_commit_line(uint16_t n);

// Read pointer for line n. Always valid; clamps to last line if n >= FB_H.
const uint8_t *fb_get_read_line(uint16_t n);

// Reset all commit bits at the start of each captured frame.
void           fb_new_frame(void);
