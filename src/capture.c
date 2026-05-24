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
#include "vga_output.h"

#include "rgbin.pio.h"
#include "vsyncgen.pio.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"
#include "pico/platform.h"

#include <string.h>

// =====================================================================
// BOOTSEL button read.
//
// The BOOTSEL button on the Pico isn't on a normal GPIO — it's wired to
// the QSPI chip-select pin of the flash chip. To read it we have to
// (briefly) put that pin into Hi-Z, sample the SIO HI input register,
// then restore the override. During the brief Hi-Z window the flash
// can't be read, so this function must live in RAM and must not call
// into any flash-resident code. Standard pico-sdk recipe.
// =====================================================================
static bool __no_inline_not_in_flash_func(get_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1u;

    uint32_t flags = save_and_disable_interrupts();

    // Set chip select to Hi-Z so the button pull-up wins.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Tiny settling delay — must not call sleep_us() (that lives in flash).
    for (volatile int i = 0; i < 1000; ++i) { }

    // Button pulls the pin LOW when pressed.
    bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    // Restore CS to its normal driven state.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

// =====================================================================
// Persistent settings (last flash sector)
//
// One 4 KB sector at the very end of flash is reserved for user
// settings — currently just the scanlines density level. Layout:
//
//   offset 0..3   magic (PERSIST_MAGIC) — distinguishes a valid record
//                 from blank flash (0xFF…FF) or stale data from a
//                 previous firmware version.
//   offset 4      scanlines level (0..3; out-of-range -> 0)
//   offset 5..    reserved
//
// flash_range_erase() / flash_range_program() are themselves RAM-
// resident in the SDK and handle XIP suspend/resume internally, but
// while they're running ALL flash reads stall. Interrupts must be
// off and Core 1 (if used) paused. Cost: ~50 ms per save. We accept
// that brief capture-loop pause; the output DMA reads from RAM so
// the picture itself is unaffected.
// =====================================================================
#define PERSIST_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define PERSIST_FLASH_ADDR   ((const uint8_t *)(XIP_BASE + PERSIST_FLASH_OFFSET))
#define PERSIST_MAGIC        0xCAFE4CB7u
#define SCANLINES_LEVEL_MAX  1   // 0 = off, 1 = every 2nd line dark
#define TRIM_LEVEL_MAX       3

// Right-edge trim in pixels for each trim level. Trims overwrite the
// rightmost N captured pixels of each line with the value of the
// leftmost pixel — effectively clamping the right border to the same
// colour as the left border, hiding post-active garbage from games
// like Prehistorik 2 that output bytes the monitor's overscan would
// normally have hidden.
static const int TRIM_PIXELS[4] = { 0, 32, 64, 96 };

// Module-level state shared between BOOTSEL handler and sanitize_line.
static int      s_scanlines_level = 0;
static int      s_trim_level      = 0;
static volatile int s_trim_pixels = 0;   // cached TRIM_PIXELS[s_trim_level]

static void persist_load(void) {
    uint32_t magic;
    memcpy(&magic, PERSIST_FLASH_ADDR, sizeof(magic));
    if (magic != PERSIST_MAGIC) {
        s_scanlines_level = 0;
        s_trim_level      = 0;
    } else {
        uint8_t sl = PERSIST_FLASH_ADDR[4];
        uint8_t tr = PERSIST_FLASH_ADDR[5];
        s_scanlines_level = (sl <= SCANLINES_LEVEL_MAX) ? (int)sl : 0;
        s_trim_level      = (tr <= TRIM_LEVEL_MAX)      ? (int)tr : 0;
    }
    s_trim_pixels = TRIM_PIXELS[s_trim_level];
}

static void persist_save(void) {
    uint8_t buf[FLASH_PAGE_SIZE];
    memset(buf, 0xFF, sizeof(buf));             // keep unused bytes erased
    uint32_t magic = PERSIST_MAGIC;
    memcpy(buf, &magic, sizeof(magic));
    buf[4] = (uint8_t)(s_scanlines_level & 0xFFu);
    buf[5] = (uint8_t)(s_trim_level      & 0xFFu);

    uint32_t flags = save_and_disable_interrupts();
    flash_range_erase(PERSIST_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PERSIST_FLASH_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(flags);
}

// =====================================================================
// BOOTSEL press handler — short press toggles scanlines, long press
// cycles right-edge trim. Action fires on release, classified by hold
// duration. Long-press threshold = 1.5 s.
//
// Mechanical switches bounce — typically 1-10 ms of fast on/off
// chatter at every press/release edge. Without debouncing, the first
// bounce-released spike (a few ms after pressing) would be classified
// as a short press, cycling scanlines before the user has even let
// go. We ignore any edge that arrives within DEBOUNCE_US of the
// previous accepted edge — that masks out all the chatter without
// touching the real edges' timestamps, so hold-duration measurement
// stays accurate.
// =====================================================================
#define LONG_PRESS_US 1500000u
#define DEBOUNCE_US    20000u

static bool     s_bootsel_was_pressed   = false;
static uint32_t s_bootsel_press_start_us = 0;
static uint32_t s_bootsel_last_edge_us   = 0;

static void poll_bootsel(void) {
    bool now = get_bootsel_button();
    if (now == s_bootsel_was_pressed) return;       // no edge

    uint32_t now_us = time_us_32();
    if ((now_us - s_bootsel_last_edge_us) < DEBOUNCE_US) return;  // bounce
    s_bootsel_last_edge_us = now_us;

    if (now) {
        // Edge: just pressed — remember when.
        s_bootsel_press_start_us = now_us;
    } else {
        // Edge: just released — classify and act.
        uint32_t held = now_us - s_bootsel_press_start_us;
        if (held > LONG_PRESS_US) {
            s_trim_level = (s_trim_level + 1) % (TRIM_LEVEL_MAX + 1);
            s_trim_pixels = TRIM_PIXELS[s_trim_level];
        } else {
            s_scanlines_level ^= 1;
            vga_output_set_scanlines(s_scanlines_level);
        }
        persist_save();
    }
    s_bootsel_was_pressed = now;
}

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

void capture_init(bool is_50hz) {
    if (vsg_offset == 0xFFFFu)
        vsg_offset = pio_add_program(CAP_PIO, &vsyncgen_program);
    if (rgb_offset == 0xFFFFu)
        rgb_offset = pio_add_program(CAP_PIO, &rgbin_program);

    vsyncgen_program_init(CAP_PIO, SM_VSYNCGEN, vsg_offset,
                          PIN_CSYNC, PIN_VSYNC_GEN, is_50hz);
    rgbin_program_init(CAP_PIO, SM_RGBIN, rgb_offset,
                       PIN_RGB_IN_BASE, is_50hz);

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
// which matches our 2-2-2 output pinout exactly (bits[5:4]=R, [3:2]=G,
// [1:0]=B). The VGA SM emits the raw byte directly — no palette LUT.
//
// "Level-2" sanitisation
// ----------------------
// The CPC's three native voltage levels per channel produce thermometer
// codes 00, 01, 11 (LOW/MID/HIGH on the output network). The fourth
// code, 10 (HI bit set but LO not), is NOT a valid CPC level — but it
// can appear briefly at sharp colour transitions if the rgbin SM samples
// while the two comparators are mid-flip (HI has swung, LO hasn't yet).
//
// On the PCB's equal-weighted summing resistor network, `10` produces
// the *same* MID voltage as `01`, so a transient `10` would briefly
// look like a "dim" pixel — visible as a one-pixel halo next to every
// text edge.
//
// Fix: after each line's DMA completes, force LO := LO | HI per channel.
//   byte |= (byte >> 1) & 0x15
//   00 → 00, 01 → 01, 10 → 11 (promoted), 11 → 11
// This relies on the *intent* of a `10` glitch always being "the bright
// value was caught mid-transition, the LO bit hasn't risen yet" — so
// promoting to `11` (HIGH) is the right call.
// Applied 32 bits at a time to the *previous* line, while the current
// line's DMA is still running (~50 µs in flight, completed well before
// we touch it). Costs ~6 µs per line; fits in the post-DMA idle window.
// ---------------------------------------------------------------------
static inline void __not_in_flash_func(sanitize_line)(uint8_t *line8) {
    // Level-2 sanitiser: promote any spurious `10` thermometer codes
    // (an impossible CPC voltage level) up to `11`. 32-bit word pass.
    uint32_t *w = (uint32_t *)line8;
    for (int i = 0; i < FB_W / 4; i++) {
        uint32_t v = w[i];
        w[i] = v | ((v >> 1) & 0x15151515u);
    }
    // Right-edge trim: overwrite the rightmost s_trim_pixels bytes with
    // the value of the leftmost pixel (assumed to be the border colour).
    // Hides post-active garbage from games whose CRTC programming
    // outputs bytes past the intended visible area.
    int trim = s_trim_pixels;
    if (trim > 0) {
        uint8_t border = line8[0];
        int start = FB_W - trim;
        if (start < 0) start = 0;
        for (int x = start; x < FB_W; x++) line8[x] = border;
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

    // Snapshot the 50/60 Hz switch (GPIO 26) position at boot. If the
    // user flips the switch during runtime, the VGA output PIO programs
    // and DMA pipeline need reloading — easiest reliable way is a
    // clean watchdog reboot. Main.c already configured the pin as a
    // pulled-up input before we got here.
    bool boot_is_50hz = !gpio_get(PIN_SWITCH);

    // Load persisted scanline + trim levels from flash. BOOTSEL presses
    // are classified by hold duration:
    //   short release (<1.5 s) → toggle scanlines on / off
    //   long  release (≥1.5 s) → cycle right-edge trim 0..3 (0/32/64/96 px)
    persist_load();
    vga_output_set_scanlines(s_scanlines_level);

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
                // BOOTSEL is checked here too so the user can adjust
                // scanlines/trim while staring at the test card.
                // Sub-throttled to ~every 16 outer ticks (~ms) to keep
                // the QSPI-read cost negligible.
                if ((poll & 0xFFFFu) == 0u) poll_bootsel();
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
                // BOOTSEL is checked here too so the user can adjust
                // scanlines/trim while staring at the test card.
                // Sub-throttled to ~every 16 outer ticks (~ms) to keep
                // the QSPI-read cost negligible.
                if ((poll & 0xFFFFu) == 0u) poll_bootsel();
            }
        }
        last_vsync_us = time_us_32();
        // VSYNC is alive again — clear the latch so the next no-signal
        // period will repaint the card.
        test_pattern_shown = false;

        // Sync is good → LED solid at half brightness.
        pwm_set_chan_level(led_slice, led_chan, LED_HALF);

        // BOOTSEL check — short release cycles scanlines, long release
        // (≥1.5 s hold) cycles the right-edge trim. Both persist to flash.
        poll_bootsel();

        // 50/60 Hz switch check — reboot cleanly if the user has flipped
        // the slide switch since power-on. Output PIOs are mode-specific
        // and can't be hot-swapped, but a watchdog reboot reads the new
        // switch position at startup and comes up in the new mode. The
        // GPIO is well past any mechanical bounce by the time we look
        // (the slide switch transitions in well under 20 ms, and we
        // only poll here at the per-frame cadence), so no debouncing
        // is needed.
        if ((!gpio_get(PIN_SWITCH)) != boot_is_50hz) {
            watchdog_reboot(0, 0, 0);
            while (1) tight_loop_contents();   // unreachable
        }

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
