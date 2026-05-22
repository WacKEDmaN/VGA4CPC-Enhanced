#pragma once
#include <stdint.h>

// =====================================================================
// VGA4CPC-Enhanced — hardware configuration
// Target: grzegorz-gr/vga4cpc PCB (Raspberry Pi Pico / RP2040)
// =====================================================================

// ---------------------------------------------------------------------
// CPC input pins (TLV3202 comparators on PCB)
//   GPIO 0 = B_LO, GPIO 1 = B_HI
//   GPIO 2 = G_LO, GPIO 3 = G_HI
//   GPIO 4 = R_LO, GPIO 5 = R_HI
//   Captured 6-bit value: bits[5:4]=R_therm, [3:2]=G_therm, [1:0]=B_therm
// ---------------------------------------------------------------------
#define PIN_RGB_IN_BASE  0
#define PIN_CSYNC        6   // CSYNC from CPC (active low, composite sync)
#define PIN_VSYNC_GEN    10  // vsyncgen SM output — HIGH during VSYNC blanking

// ---------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------
#define PIN_LED          25
#define PIN_SWITCH       26  // open/HIGH = 60 Hz, closed/LOW = 50 Hz

// ---------------------------------------------------------------------
// PIO assignments
//   PIO0: VGA output  — hsync, vsync, rgb (3 SMs)
//   PIO1 SM0: vsyncgen  — CSYNC pulse discriminator
//   PIO1 SM1: rgbin     — 16 MHz pixel sampler → RX FIFO
//   PIO1 SM2: csync_detect (unused in this build)
// ---------------------------------------------------------------------
#define VGA_PIO          pio0
#define SM_VGA_HSYNC     0
#define SM_VGA_VSYNC     1
#define SM_VGA_RGB       2

#define CAP_PIO          pio1
#define SM_VSYNCGEN      0
#define SM_RGBIN         1

// VGA output pin bases (matches the upstream grzegorz-gr/vga4cpc PCB layout)
#define PIN_VGA_HSYNC    12
#define PIN_VGA_VSYNC    13
#define PIN_VGA_RGB_BASE 14   // GPIO 14..19 — 6-bit out into 27-colour summing resistor network

// ---------------------------------------------------------------------
// System clock — per-mode, chosen at boot based on the 50/60 Hz switch.
//
//   50 Hz mode: 128 MHz. Lets every PIO program use the original
//               upstream clkdivs verbatim (the 50 Hz path is timing-
//               sensitive and monitors are fussier about it; matching
//               upstream bit-for-bit avoids reintroducing fractional-
//               divider artefacts that show as jittery text edges).
//
//   60 Hz mode: 160 MHz. Divides cleanly to 40 MHz pixel clock for
//               exact DMT 800×600p60 (160/4 with integer clkdiv 4) and
//               keeps the capture path at 16 MHz (rgbin clkdiv 1+64/256
//               → SM = 128 MHz × 8 cycles/pixel = 16 MHz capture).
//
// main.c reads PIN_SWITCH before calling set_sys_clock_khz() so the
// PLL is configured for the right mode before any PIO is loaded.
// rgbin and vsyncgen — which run in both modes — pick their clkdiv
// at program-init time based on the same switch read.
// ---------------------------------------------------------------------
#define SYS_CLOCK_KHZ_50HZ   128000
#define SYS_CLOCK_KHZ_60HZ   160000

// ---------------------------------------------------------------------
// CPC video signal parameters
//   800 pixels/line at 16 MHz = 50 µs active video
//   288 captured lines × 2 = 576 VGA output lines (line-doubled, 576p50)
// ---------------------------------------------------------------------
#define CPC_ACTIVE_W     800
#define CPC_ACTIVE_H     288

// No-signal detection threshold. Generous so the brief idle gap between
// capturing the last framebuffer line and the next CPC VSYNC (~3 ms at
// 50 Hz) doesn't cause signal_present() to flip false and let the test
// pattern bleed through.
#define SIG_ABSENT_US    50000

// ---------------------------------------------------------------------
// Framebuffer: one CPC frame, 1 byte/pixel (2-2-2 thermometer-coded RGB)
// ---------------------------------------------------------------------
#define FB_W             CPC_ACTIVE_W   // 800
#define FB_H             CPC_ACTIVE_H   // 288
#define FB_STRIDE        FB_W

// Horizontal crop: CPC captures 800 px per line, VGA shows 640. With
// FB_X_OFFSET=0 we show pixels 0..639 of the captured line — i.e. from
// the start of the CPC's back-porch-clear active video, which includes
// the left border. The rightmost 160 captured pixels (= the right border
// + front porch) are dropped.
#define FB_X_OFFSET      0
