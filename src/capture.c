// =====================================================================
// capture.c — CPC RGB capture pipeline (PIO + DMA + Core 0 polling loop)
//
// Owns Core 0 forever once capture_run_forever() is entered. Tracks
// CSYNC/VSYNC edges on the input, configures a fresh per-line DMA from
// the rgbin SM's RX FIFO into a framebuffer row, and runs a cheap
// thermometer-code sanitiser on the previous line while the next
// line's DMA is in flight.
// =====================================================================
#include "capture.h"
#include "hardware_config.h"
#include "framebuffer.h"

#include "rgbin.pio.h"
#include "vsyncgen.pio.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
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
// [1:0]=B). The VGA SM emits the raw byte directly — no palette LUT.
//
// "Level-2" sanitisation
// ----------------------
// The CPC's three native voltage levels per channel produce thermometer
// codes 00, 01, 11 (DAC levels 0/1/3). The fourth code, 10 (HI bit set
// but LO not), is NOT a valid CPC level — but it can appear briefly
// at sharp colour transitions if the rgbin SM samples while the two
// comparators are mid-flip (HI has swung, LO hasn't yet). On the DAC
// it maps to half-bright voltage, so a one-pixel halo appears next to
// every text edge.
//
// Fix: after each line's DMA completes, force LO := LO | HI per channel.
//   byte |= (byte >> 1) & 0x15
//   00 → 00, 01 → 01, 10 → 11 (promoted), 11 → 11
// Applied 32 bits at a time to the *previous* line, while the current
// line's DMA is still running (~50 µs in flight, completed well before
// we touch it). Costs ~6 µs per line; fits in the post-DMA idle window.
// ---------------------------------------------------------------------
static inline void __not_in_flash_func(sanitize_line)(uint8_t *line8) {
    uint32_t *w = (uint32_t *)line8;
    for (int i = 0; i < FB_W / 4; i++) {
        uint32_t v = w[i];
        w[i] = v | ((v >> 1) & 0x15151515u);
    }
}

// "No signal" timeout — if no VSYNC has arrived in this many microseconds,
// the capture loop repaints the test pattern into the framebuffer so the
// monitor doesn't keep showing a frozen last-frame after the CPC powers off.
#define NO_SIGNAL_TIMEOUT_US (3u * 1000u * 1000u)

// Per-line HSYNC watchdog — a normal CPC line is ~64 µs, so 200 ms with no
// HSYNC at all means the CPC has died mid-frame. Bail out of the per-line
// loop so the outer VSYNC waits (which carry the no-signal repaint logic)
// can take over.
#define HSYNC_WATCHDOG_US    (200u * 1000u)

void __not_in_flash_func(capture_run_forever)(void) {
    int line = 0;
    uint32_t last_vsync_us = time_us_32();
    bool     test_pattern_shown = false;   // true while no-signal card is up

    // LED diagnostic (PWM-dimmed to ~half brightness so it's not blindingly
    // bright in a dark room):
    //   ~1 Hz flash  → waiting for sync (CPC off / disconnected)
    //   Solid (dim)  → CPC sync detected, frames being captured
    //
    // GPIO 25 lives on PWM slice 4, channel B. 8-bit wrap (0..255);
    // level 128 = 50% duty cycle.
    enum { LED_HALF = 128, LED_OFF = 0, LED_WRAP = 255 };
    gpio_set_function(PIN_LED, GPIO_FUNC_PWM);
    uint led_slice = pwm_gpio_to_slice_num(PIN_LED);
    uint led_chan  = pwm_gpio_to_channel(PIN_LED);
    pwm_set_wrap(led_slice, LED_WRAP);
    pwm_set_chan_level(led_slice, led_chan, LED_OFF);
    pwm_set_enabled(led_slice, true);

    while (1) {
        // VSYNC_GEN is HIGH during VSYNC blanking, LOW otherwise.
        // Throttled timeout check (every ~4096 polls) so a powered-off CPC
        // triggers a test-pattern repaint after NO_SIGNAL_TIMEOUT_US.
        uint32_t poll = 0;
        while (!gpio_get(PIN_VSYNC_GEN)) {
            if ((++poll & 0xFFFu) == 0u) {
                // Flash the LED at ~1 Hz while we're waiting for sync.
                bool led_on = ((time_us_32() / 500000u) & 1u) == 0u;
                pwm_set_chan_level(led_slice, led_chan,
                                   led_on ? LED_HALF : LED_OFF);
                if (!test_pattern_shown &&
                    (time_us_32() - last_vsync_us) > NO_SIGNAL_TIMEOUT_US) {
                    fb_paint_test_pattern();
                    test_pattern_shown = true;
                }
            }
        }
        poll = 0;
        while (gpio_get(PIN_VSYNC_GEN)) {
            if ((++poll & 0xFFFu) == 0u) {
                // Flash the LED at ~1 Hz while we're waiting for sync.
                bool led_on = ((time_us_32() / 500000u) & 1u) == 0u;
                pwm_set_chan_level(led_slice, led_chan,
                                   led_on ? LED_HALF : LED_OFF);
                if (!test_pattern_shown &&
                    (time_us_32() - last_vsync_us) > NO_SIGNAL_TIMEOUT_US) {
                    fb_paint_test_pattern();
                    test_pattern_shown = true;
                }
            }
        }
        last_vsync_us = time_us_32();
        // VSYNC is alive again — clear the latch so the next no-signal
        // period will repaint the card.
        test_pattern_shown = false;

        // Sync is good → LED solid at half brightness.
        pwm_set_chan_level(led_slice, led_chan, LED_HALF);

        // Black-fill the tail of the previous frame.
        for (int y = line; y < FB_H; y++) {
            uint8_t *fb = fb_get_write_line((uint16_t)y);
            if (fb) memset(fb, 0, FB_W);
        }
        fb_new_frame();
        line = 0;
        uint8_t *prev_fb = NULL;

        while (!gpio_get(PIN_VSYNC_GEN) && line < FB_H) {
            // HSYNC start (CSYNC falling). Bail out if VSYNC arrives early
            // OR if we've sat here so long the CPC clearly died mid-frame.
            uint32_t hsync_poll = 0;
            while (gpio_get(PIN_CSYNC)) {
                if (gpio_get(PIN_VSYNC_GEN)) goto frame_end;
                if ((++hsync_poll & 0xFFFu) == 0u &&
                    (time_us_32() - last_hsync_us) > HSYNC_WATCHDOG_US) {
                    goto frame_end;
                }
            }
            // HSYNC end (CSYNC rising). Same dual-exit logic.
            hsync_poll = 0;
            while (!gpio_get(PIN_CSYNC)) {
                if (gpio_get(PIN_VSYNC_GEN)) goto frame_end;
                if ((++hsync_poll & 0xFFFu) == 0u &&
                    (time_us_32() - last_hsync_us) > HSYNC_WATCHDOG_US) {
                    goto frame_end;
                }
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
            // Clean up the previous line (its DMA finished long ago).
            // Runs concurrently with the just-kicked DMA on *this* line.
            if (prev_fb) sanitize_line(prev_fb);
            prev_fb = fb;
            line++;
        }
    frame_end:
        // Final line of the frame still needs cleaning before we leave.
        if (prev_fb) {
            // Make sure its DMA actually completed (always true in practice
            // — ~50 µs DMA vs ~64 µs line — but cheap to be safe).
            while (dma_channel_is_busy((uint)dma_cap_channel))
                tight_loop_contents();
            sanitize_line(prev_fb);
        }
    }
}
