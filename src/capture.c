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

#include "rgbin_50.pio.h"
#include "rgbin_60.pio.h"
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
#define PERSIST_MAGIC        0xCAFE4CC8u   // bump → revert oversample, back to 1× capture
#define SCANLINES_LEVEL_MAX  1   // 0 = off, 1 = every 2nd line dark
#define TRIM_LEVEL_MAX       3

// =====================================================================
// Runtime-tunable capture-clock divider (both modes).
//
// Each idx selects an rgbin SM clkdiv (integer part = 1, fractional
// part from the per-mode table). Picked at boot from is_50hz; cycled
// at runtime via BOOTSEL short press.
//
// 60 Hz mode — sys_clock = 160 MHz:
//   idx 0 frac 64 (1.250 ) → 128.00 MHz SM → 16.000 MHz EXACT
//   idx 1 frac 65          → 127.60 MHz SM → 15.950 MHz (−0.31 %)
//   idx 2 frac 66          → 127.20 MHz SM → 15.901 MHz (−0.62 %)
//   idx 3 frac 67          → 126.81 MHz SM → 15.851 MHz (−0.93 %)
//   idx 4 frac 68          → 126.42 MHz SM → 15.803 MHz (−1.23 %)
//   idx 5 frac 69          → 126.04 MHz SM → 15.755 MHz (−1.53 %)
//   idx 6 frac 70          → 125.66 MHz SM → 15.707 MHz (−1.83 %)
//   idx 7 frac 71          → 125.27 MHz SM → 15.659 MHz (−2.13 %)
//   idx 8 frac 72          → 124.88 MHz SM → 15.610 MHz (−2.44 %)
//
// 50 Hz mode — sys_clock = 128 MHz:
//   idx 0 frac 16          → 120.47 MHz SM → 15.059 MHz (−5.88 %) ← bug still
//   idx 1 frac 20          → 118.84 MHz SM → 14.855 MHz (−7.16 %) ← bug still
//   idx 2 frac 22          → 118.07 MHz SM → 14.759 MHz (−7.76 %)
//   idx 3 frac 23          → 117.67 MHz SM → 14.709 MHz (−8.07 %)
//   idx 4 frac 24          → 117.27 MHz SM → 14.659 MHz (−8.38 %) ← default (confirmed working)
//   idx 5 frac 25          → 116.88 MHz SM → 14.610 MHz (−8.69 %)
//   idx 6 frac 26          → 116.50 MHz SM → 14.562 MHz (−8.99 %)
//   idx 7 frac 28          → 115.74 MHz SM → 14.467 MHz (−9.58 %)
//   idx 8 frac 30          → 114.99 MHz SM → 14.374 MHz (−10.2 %) ← near half-height limit
//
// 50 Hz sweet spot is narrow on the reference HW: below frac ~22 the
// colour bug returns (insufficient phase smearing); above frac ~30
// the 800-sample loop overruns the 64 µs CPC line and the capture
// drops every other HSYNC (half-height image). Table is concentrated
// on the working band so cycling lands cleanly on usable values.
//
// At idx 0 the SM locks to the CPC pixel clock and the sample point
// sits at a fixed phase — usually the comparator-flip window =
// colour bug, but with zero jitter. Idx 1+ detunes the sample clock
// so the sample point drifts across CPC pixels every line; bug gone,
// inherent jitter introduced. The optimum is the SMALLEST idx that
// reliably breaks lock on the target hardware. Default = idx 2.
// =====================================================================
static const uint8_t DETUNE_FRACS_60[] = {
    64, 65, 66, 67, 68, 69, 70, 71, 72
};
static const uint8_t DETUNE_FRACS_50[] = {
    16, 20, 22, 23, 24, 25, 26, 28, 30
};
#define DETUNE_COUNT        (sizeof(DETUNE_FRACS_60) / sizeof(DETUNE_FRACS_60[0]))
// Separate defaults per mode so we can geometry-test 50 Hz at exact
// 16 MHz lock (idx 0 = colour bug shows, but the image dimensions
// should be 100 % correct because nothing's undersampled).
#define DETUNE_DEFAULT_IDX_60  2
#define DETUNE_DEFAULT_IDX_50  4   // frac 24 = 14.66 MHz (−8.38 %),
                                   // confirmed bug-free + full-height on
                                   // reference HW; mid-table for headroom
#define DETUNE_DEFAULT_IDX     (s_is_50hz ? DETUNE_DEFAULT_IDX_50 \
                                          : DETUNE_DEFAULT_IDX_60)

// Right-edge trim in pixels for each trim level. Trims overwrite the
// rightmost N captured pixels of each line with the value of the
// leftmost pixel — effectively clamping the right border to the same
// colour as the left border, hiding post-active garbage from games
// like Prehistorik 2 that output bytes the monitor's overscan would
// normally have hidden.
static const int TRIM_PIXELS[4] = { 0, 32, 64, 96 };

// Module-level state shared between BOOTSEL handler and the capture
// pipeline.
static int      s_scanlines_level = 0;
static int      s_trim_level      = 0;
static volatile int     s_trim_pixels  = 0;   // cached TRIM_PIXELS[s_trim_level]
static int      s_detune_idx      = 0;   // real value picked in persist_load() after s_is_50hz is set
static bool     s_is_50hz         = false;    // snapshot of capture_init arg

static void persist_load(void) {
    uint32_t magic;
    memcpy(&magic, PERSIST_FLASH_ADDR, sizeof(magic));
    if (magic != PERSIST_MAGIC) {
        s_scanlines_level = 0;
        s_trim_level      = 0;
        s_detune_idx      = DETUNE_DEFAULT_IDX;
    } else {
        uint8_t sl = PERSIST_FLASH_ADDR[4];
        uint8_t tr = PERSIST_FLASH_ADDR[5];
        uint8_t di = PERSIST_FLASH_ADDR[7];
        s_scanlines_level = (sl <= SCANLINES_LEVEL_MAX) ? (int)sl : 0;
        s_trim_level      = (tr <= TRIM_LEVEL_MAX)      ? (int)tr : 0;
        s_detune_idx      = (di < DETUNE_COUNT)         ? (int)di
                                                        : DETUNE_DEFAULT_IDX;
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
    // buf[6] unused (was filter-mask, removed)
    buf[7] = (uint8_t)(s_detune_idx      & 0xFFu);

    uint32_t flags = save_and_disable_interrupts();
    flash_range_erase(PERSIST_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PERSIST_FLASH_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(flags);
}

// Write the rgbin SM clkdiv register directly so we can re-tune the
// capture sample rate at runtime without a full PIO re-init. Format:
// bits 31..16 = integer part, 15..8 = fractional part. Glitch-free in
// practice (SM keeps running, next cycle uses new rate). The per-mode
// frac table is picked here so the same idx (0..8) means the same
// fractional offset from "exact 16 MHz lock" in either mode.
static void apply_detune(void) {
    const uint8_t *table = s_is_50hz ? DETUNE_FRACS_50 : DETUNE_FRACS_60;
    uint8_t frac = table[s_detune_idx];
    CAP_PIO->sm[SM_RGBIN].clkdiv =
        (uint32_t)(1u << 16) | ((uint32_t)frac << 8);
}

// =====================================================================
// BOOTSEL press handler — three press classes by hold duration:
//
//   short  (<1.5 s)        cycle DETUNE idx (capture clkdiv frac)
//   medium (1.5 s..5 s)    cycle right-edge TRIM 0..3
//   long   (>5 s)          toggle SCANLINES on/off
//
// All three persist to flash. Action fires on release.
//
// Mechanical switches bounce — typically 1-10 ms of fast on/off
// chatter at every press/release edge. Without debouncing, the first
// bounce-released spike (a few ms after pressing) would be classified
// as a short press before the user has even let go. We ignore any
// edge within DEBOUNCE_US of the previous accepted edge — that masks
// out all the chatter without touching the real edges' timestamps,
// so hold-duration measurement stays accurate.
// =====================================================================
#define MEDIUM_PRESS_US  1500000u
#define LONG_PRESS_US    5000000u
#define DEBOUNCE_US        20000u

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
            s_scanlines_level ^= 1;
            vga_output_set_scanlines(s_scanlines_level);
        } else if (held > MEDIUM_PRESS_US) {
            s_trim_level = (s_trim_level + 1) % (TRIM_LEVEL_MAX + 1);
            s_trim_pixels = TRIM_PIXELS[s_trim_level];
        } else {
            s_detune_idx = (s_detune_idx + 1) % (int)DETUNE_COUNT;
            apply_detune();
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
    s_is_50hz = is_50hz;
    if (vsg_offset == 0xFFFFu)
        vsg_offset = pio_add_program(CAP_PIO, &vsyncgen_program);
    if (rgb_offset == 0xFFFFu) {
        rgb_offset = pio_add_program(CAP_PIO,
            is_50hz ? &rgbin_50_program : &rgbin_60_program);
    }

    vsyncgen_program_init(CAP_PIO, SM_VSYNCGEN, vsg_offset,
                          PIN_CSYNC, PIN_VSYNC_GEN, is_50hz);
    if (is_50hz) {
        rgbin_50_program_init(CAP_PIO, SM_RGBIN, rgb_offset, PIN_RGB_IN_BASE);
    } else {
        rgbin_60_program_init(CAP_PIO, SM_RGBIN, rgb_offset, PIN_RGB_IN_BASE);
    }

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

    // Load persisted state from flash. BOOTSEL press classes:
    //   short  (<1.5 s)     cycle DETUNE idx (capture clkdiv)
    //   medium (1.5–5 s)    cycle right-edge TRIM 0..3
    //   long   (>5 s)       toggle SCANLINES on/off
    persist_load();
    vga_output_set_scanlines(s_scanlines_level);
    // Apply the persisted detune to the rgbin SM now that PIO is up.
    apply_detune();

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
            if (prev_fb) {
                sanitize_line(prev_fb);
            }
            prev_fb = fb;
            line++;
        }
    frame_end:
        // Final line of the frame still needs cleaning before we leave.
        if (prev_fb) {
            while (dma_channel_is_busy((uint)dma_cap_channel))
                tight_loop_contents();
            sanitize_line(prev_fb);
        }
    }
}
