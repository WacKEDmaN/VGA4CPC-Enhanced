#include "vga_output.h"
#include "hardware_config.h"
#include "framebuffer.h"

#include "hardware/pio.h"
#include "hardware/dma.h"

#include "hsync_50.pio.h"
#include "vsync_50.pio.h"
#include "rgb_50.pio.h"
#include "hsync_60.pio.h"
#include "vsync_60.pio.h"
#include "rgb_60.pio.h"

#include <string.h>

// =====================================================================
// Direct VGA scan-doubler — replaces pico-extras scanvideo entirely.
//
//   PIO0 SM0 (hsync) — generates HSYNC, sends IRQ 0 at start of each line
//   PIO0 SM1 (vsync) — counts lines, generates VSYNC, sends IRQ 1 on
//                      visible lines (tells RGB SM to start drawing)
//   PIO0 SM2 (rgb)   — pulls one byte per pixel from TX FIFO, drives
//                      the 6 output pins. 800 px per line.
//
// DMA chain (matches reference vga4cpc design):
//   - Channel A loads next write_addr into main channel (= PIO TX FIFO addr)
//   - Channel B loads next transfer_count
//   - Channel C loads next read_addr (and triggers main)
//   - Main channel sends bytes to RGB PIO
//   - Three config channels read from line_dst/line_size/line_src arrays
//   - 12-bit ring (4096 bytes) cycles them through 1024 entries per frame
//
// Each CPC line is sent twice (line-doubling = scan-doubling).
// =====================================================================

// DMA channel assignments (scanvideo is gone now, no contention).
#define DMA_RGB_OUT       1
#define DMA_CFG_A         2
#define DMA_CFG_B         3
#define DMA_CFG_C         4

// 1024 power-of-2 step ring (required for DMA write_increment-wrap)
static uint8_t *line_src[1024]  __attribute__((aligned(4096)));
static void    *line_dst[1024]  __attribute__((aligned(4096)));
static uint32_t line_size[1024] __attribute__((aligned(4096)));
static uint32_t tmp_sink;
static uint8_t  black_line[CPC_ACTIVE_W];

// Captured at init so vga_output_set_scanlines() can rebuild line_src[].
static int g_scanline_count = 0;   // = lines_to_copy * 2
static int g_scanline_skip  = 0;   // = skip_lines (CPC line index offset)

// Programmed-value constants needed by the PIO SMs (too big for `set x, N`).
//
// H_ACTIVE_FRONT_*_M2 is the count pushed to the corresponding hsync_*
// SM's "visibleandfront" loop. The loop runs (osr + 2) SM cycles, so
// the value here is (HACT + HFP) - 2 in *SM cycles*. The 50 Hz and
// 60 Hz hsync SMs run at different clkdivs so their SM-cycles-per-pixel
// ratios differ — these numbers reflect the each-mode-specific timing:
//
//   60 Hz mode (DMT 800×600p60, SM clock 40 MHz, 1 SM cycle = 1 pixel):
//     HACT 800 + HFP 40 = 840 pixels → osr = 838
//
//   50 Hz mode (CEA-861 720×576p50 with 800-px overscan, SM clock
//   6.76 MHz, 1 SM cycle ≈ 4 pixels): keeps original upstream value.
#define V_ACTIVE_60_MINUS1     599u
#define V_ACTIVE_50_MINUS1     575u
#define RGB_ACTIVE_MINUS1      (CPC_ACTIVE_W - 1u)
#define H_ACTIVE_FRONT_60_M2   838u
#define H_ACTIVE_FRONT_50_M2   181u

void vga_output_init(bool is_50hz) {
    memset(black_line, 0, sizeof(black_line));

    // Claim our DMA channels before capture_init runs so it picks something else.
    dma_channel_claim(DMA_RGB_OUT);
    dma_channel_claim(DMA_CFG_A);
    dma_channel_claim(DMA_CFG_B);
    dma_channel_claim(DMA_CFG_C);

    // ------- Load + configure PIO SMs ----------------------------------
    uint hsync_off = pio_add_program(VGA_PIO,
        is_50hz ? &hsync_50_program : &hsync_60_program);
    uint vsync_off = pio_add_program(VGA_PIO,
        is_50hz ? &vsync_50_program : &vsync_60_program);
    uint rgb_off   = pio_add_program(VGA_PIO,
        is_50hz ? &rgb_50_program   : &rgb_60_program);

    if (is_50hz) {
        hsync_50_program_init(VGA_PIO, SM_VGA_HSYNC, hsync_off, PIN_VGA_HSYNC);
        vsync_50_program_init(VGA_PIO, SM_VGA_VSYNC, vsync_off, PIN_VGA_VSYNC);
        rgb_50_program_init  (VGA_PIO, SM_VGA_RGB,   rgb_off,   PIN_VGA_RGB_BASE);
        pio_sm_put_blocking(VGA_PIO, SM_VGA_HSYNC, H_ACTIVE_FRONT_50_M2);
        pio_sm_put_blocking(VGA_PIO, SM_VGA_VSYNC, V_ACTIVE_50_MINUS1);
    } else {
        hsync_60_program_init(VGA_PIO, SM_VGA_HSYNC, hsync_off, PIN_VGA_HSYNC);
        vsync_60_program_init(VGA_PIO, SM_VGA_VSYNC, vsync_off, PIN_VGA_VSYNC);
        rgb_60_program_init  (VGA_PIO, SM_VGA_RGB,   rgb_off,   PIN_VGA_RGB_BASE);
        pio_sm_put_blocking(VGA_PIO, SM_VGA_HSYNC, H_ACTIVE_FRONT_60_M2);
        pio_sm_put_blocking(VGA_PIO, SM_VGA_VSYNC, V_ACTIVE_60_MINUS1);
    }
    pio_sm_put_blocking(VGA_PIO, SM_VGA_RGB, RGB_ACTIVE_MINUS1);

    // ------- Build line_dst / line_size / line_src arrays ---------------
    // 50 Hz: skip first 8 CPC lines (top blanking margin), output 576 VGA lines.
    // 60 Hz: no skip, output 600 VGA lines (CPC × 2 = 576, pad 24 black).
    const int skip_lines = is_50hz ? 8 : 0;
    const int lines_to_copy = (int)CPC_ACTIVE_H - skip_lines;
    const int dst_screen_height = is_50hz ? 576 : 600;

    // CPC × 2 = line-doubled output (each CPC line shown on 2 VGA lines).
    // Initialised as plain NORMAL mode; runtime toggle handled by
    // vga_output_set_scanlines() which rewrites the odd-indexed entries.
    g_scanline_count   = lines_to_copy * 2;
    g_scanline_skip    = skip_lines;
    for (int i = 0; i < lines_to_copy * 2; i++) {
        line_dst[i]  = &(VGA_PIO->txf[SM_VGA_RGB]);
        line_src[i]  = framebuf[i / 2 + skip_lines];
        line_size[i] = CPC_ACTIVE_W;
    }
    // Pad with black lines to fill VGA visible
    for (int i = lines_to_copy * 2; i < dst_screen_height; i++) {
        line_dst[i]  = &(VGA_PIO->txf[SM_VGA_RGB]);
        line_src[i]  = black_line;
        line_size[i] = CPC_ACTIVE_W;
    }
    // Padding to fill the 1024-step ring (DMA writes one dummy byte each)
    for (int i = dst_screen_height; i < 1024; i++) {
        line_dst[i]  = &tmp_sink;
        line_src[i]  = framebuf[0];      // any valid address
        line_size[i] = 1;
    }

    // ------- Configure the 4-channel DMA chain --------------------------
    // Channel A: writes line_dst entries into main channel's write_addr.
    {
        dma_channel_config c = dma_channel_get_default_config(DMA_CFG_A);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_chain_to(&c, DMA_CFG_B);
        channel_config_set_ring(&c, false, 12);
        dma_channel_configure(DMA_CFG_A, &c,
            &dma_hw->ch[DMA_RGB_OUT].al3_write_addr,
            line_dst, 1, false);
    }
    // Channel B: writes line_size entries into main channel's trans_count.
    {
        dma_channel_config c = dma_channel_get_default_config(DMA_CFG_B);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_chain_to(&c, DMA_CFG_C);
        channel_config_set_ring(&c, false, 12);
        dma_channel_configure(DMA_CFG_B, &c,
            &dma_hw->ch[DMA_RGB_OUT].al3_transfer_count,
            line_size, 1, false);
    }
    // Channel C: writes line_src entries into main channel's read_addr_trig.
    {
        dma_channel_config c = dma_channel_get_default_config(DMA_CFG_C);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_ring(&c, false, 12);
        dma_channel_configure(DMA_CFG_C, &c,
            &dma_hw->ch[DMA_RGB_OUT].al3_read_addr_trig,
            line_src, 1, false);
    }
    // Main channel: streams one line of bytes to RGB SM's TX FIFO.
    {
        dma_channel_config c = dma_channel_get_default_config(DMA_RGB_OUT);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(VGA_PIO, SM_VGA_RGB, true));
        channel_config_set_chain_to(&c, DMA_CFG_A);
        dma_channel_configure(DMA_RGB_OUT, &c,
            &(VGA_PIO->txf[SM_VGA_RGB]),
            framebuf[0],
            CPC_ACTIVE_W, false);
    }
}

void vga_output_start(void) {
    pio_enable_sm_mask_in_sync(VGA_PIO,
        (1u << SM_VGA_HSYNC) | (1u << SM_VGA_VSYNC) | (1u << SM_VGA_RGB));
    dma_channel_start(DMA_CFG_A);
}

// Toggle the CRT-style scanlines effect on or off at runtime. Rewrites
// the odd-indexed entries of the DMA source-pointer ring; the swap is
// effectively atomic per slot (a single aligned 32-bit pointer store
// on Cortex-M0+), so there's no torn output during the transition.
// Worst case is one VGA frame of mixed normal/scanlined rows during
// the switch (~20 ms at 50 Hz), which is imperceptible.
void vga_output_set_scanlines(int enabled) {
    for (int i = 1; i < g_scanline_count; i += 2) {
        line_src[i] = enabled ? black_line
                              : framebuf[i / 2 + g_scanline_skip];
    }
}
