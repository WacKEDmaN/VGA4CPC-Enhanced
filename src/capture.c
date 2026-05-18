#include "capture.h"
#include "hardware_config.h"
#include "framebuffer.h"

#include "rgbin.pio.h"
#include "vsyncgen.pio.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "pico/platform.h"

#include <string.h>

// =====================================================================
// Capture pipeline — straight port of grzegorz-gr/vga4cpc firmware.
//
//   PIO1 SM0 (vsyncgen) — discriminates CSYNC into a VSYNC level on
//                         GPIO 10 (must be wired back to GPIO 7 on the
//                         VGA4CPC PCB).
//   PIO1 SM1 (rgbin)    — per-line HSYNC-aligned sampler, 8 SM cycles
//                         per pixel ≈ 16 MHz. Each pixel is pushed as
//                         one 32-bit word (low byte = 6-bit sample).
//
//   Core 0 capture_run_forever — busy-polls CSYNC (GPIO 6) and VSYNC
//                         (GPIO 10) to drive a fresh DMA per CPC line.
// =====================================================================

static int  dma_cap_channel = -1;
static uint rgb_offset      = 0xFFFFu;
static uint vsg_offset      = 0xFFFFu;

static volatile uint32_t last_hsync_us = 0;

void capture_init(void) {
    if (vsg_offset == 0xFFFFu)
        vsg_offset = pio_add_program(CAP_PIO, &vsyncgen_program);
    if (rgb_offset == 0xFFFFu)
        rgb_offset = pio_add_program(CAP_PIO, &rgbin_program);

    vsyncgen_program_init(CAP_PIO, SM_VSYNCGEN, vsg_offset,
                          PIN_CSYNC, PIN_VSYNC_GEN);
    rgbin_program_init(CAP_PIO, SM_RGBIN, rgb_offset, PIN_RGB_IN_BASE);

    // Tell the rgbin SM how many pixels to sample per line: CPC_ACTIVE_W - 1
    pio_sm_put_blocking(CAP_PIO, SM_RGBIN, CPC_ACTIVE_W - 1u);

    if (dma_cap_channel < 0)
        dma_cap_channel = dma_claim_unused_channel(true);

    pio_enable_sm_mask_in_sync(CAP_PIO,
                               (1u << SM_VSYNCGEN) | (1u << SM_RGBIN));
}

void capture_start(void) {
    // Already enabled in capture_init.
}

void capture_stop(void) {
    pio_sm_set_enabled(CAP_PIO, SM_RGBIN,    false);
    pio_sm_set_enabled(CAP_PIO, SM_VSYNCGEN, false);
    if (dma_cap_channel >= 0)
        dma_channel_abort((uint)dma_cap_channel);
}

bool capture_signal_present(void) {
    return (time_us_32() - last_hsync_us) < SIG_ABSENT_US;
}

// ---------------------------------------------------------------------
// Run loop — owns Core 0 forever.
//
// The captured 6-bit thermometer code is laid out as
//   bit5=R_HI bit4=R_LO bit3=G_HI bit2=G_LO bit1=B_HI bit0=B_LO
// which matches our 2-2-2 DAC pinout exactly (bits[5:4]=R, [3:2]=G,
// [1:0]=B). The VGA SM emits the raw byte directly — no LUT needed.
// ---------------------------------------------------------------------
void __not_in_flash_func(capture_run_forever)(void) {
    int line = 0;
    uint32_t frame_count = 0;

    // LED diagnostic:
    //   Solid ON     → waiting for first VSYNC (vsyncgen PIO not producing edges)
    //   ~1 Hz blink  → frames detected normally
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    while (1) {
        // VSYNC_GEN is HIGH during VSYNC blanking, LOW otherwise.
        while (!gpio_get(PIN_VSYNC_GEN)) tight_loop_contents();
        while (gpio_get(PIN_VSYNC_GEN))  tight_loop_contents();

        frame_count++;
        gpio_put(PIN_LED, (frame_count / 25u) & 1u);

        // Black-fill the tail of the previous frame.
        for (int y = line; y < FB_H; y++) {
            uint8_t *fb = fb_get_write_line((uint16_t)y);
            if (fb) memset(fb, 0, FB_W);
        }
        fb_new_frame();
        line = 0;

        while (!gpio_get(PIN_VSYNC_GEN) && line < FB_H) {
            // HSYNC start (CSYNC falling)
            while (gpio_get(PIN_CSYNC)) {
                if (gpio_get(PIN_VSYNC_GEN)) goto frame_end;
            }
            // HSYNC end (CSYNC rising)
            while (!gpio_get(PIN_CSYNC)) {
                if (gpio_get(PIN_VSYNC_GEN)) goto frame_end;
            }
            last_hsync_us = time_us_32();

            uint8_t *fb = fb_get_write_line((uint16_t)line);
            if (fb) {
                dma_channel_config c = dma_channel_get_default_config((uint)dma_cap_channel);
                channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
                channel_config_set_read_increment(&c, false);
                channel_config_set_write_increment(&c, true);
                channel_config_set_dreq(&c, pio_get_dreq(CAP_PIO, SM_RGBIN, false));
                dma_channel_configure((uint)dma_cap_channel, &c,
                                      fb,
                                      &CAP_PIO->rxf[SM_RGBIN],
                                      FB_W,
                                      true);
            }
            line++;
        }
    frame_end: ;
    }
}
