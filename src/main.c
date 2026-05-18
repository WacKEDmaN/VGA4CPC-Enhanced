// =====================================================================
// VGA4CPC-Enhanced — direct CPC → VGA scan doubler
//
// Custom-PIO scan-doubler firmware for the grzegorz-gr/vga4cpc PCB
// (Raspberry Pi Pico based). Captures the live CPC RGB+CSYNC signal
// at 16 MHz, line-doubles it, and emits 576p50 or 800×600p60 VGA.
//
//   sys_clock = 128 MHz       — exact 16 MHz CPC capture (128 / 8)
//   PIO0                      — VGA output (hsync, vsync, rgb SMs)
//   PIO1                      — CPC capture (vsyncgen + rgb sampler)
//
// The 50/60 Hz output mode is selected by the slide switch on GPIO 26
// at boot (closed/LOW = 576p50, open/HIGH = 800×600p60).
// =====================================================================

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware_config.h"
#include "framebuffer.h"
#include "capture.h"
#include "vga_output.h"

// =====================================================================
// Boot test pattern — written into framebuf at startup so the user has
// something to look at when no CPC signal is connected. As soon as CPC
// content starts arriving the capture path overwrites it line-by-line.
//
// The pixel byte layout is the same one the DAC consumes natively:
//   bits[5:4]=R, [3:2]=G, [1:0]=B   each pair driving 2 R-2R DAC pins.
// CPC's three thermometer levels map to DAC levels 0/1/3 (level 2 is
// unused by CPC but produces the half-bright voltage from the DAC).
// =====================================================================

#define CPC_TH(L)  ((L)==2 ? 3u : (L)==1 ? 1u : 0u)
#define CPC(r,g,b) (uint8_t)((CPC_TH(r)<<4) | (CPC_TH(g)<<2) | CPC_TH(b))

static const uint8_t SMPTE_BARS[8] = {
    CPC(2,2,2),  // White
    CPC(2,2,0),  // Yellow
    CPC(0,2,2),  // Cyan
    CPC(0,2,0),  // Green
    CPC(2,0,2),  // Magenta
    CPC(2,0,0),  // Red
    CPC(0,0,2),  // Blue
    CPC(0,0,0),  // Black
};

static void build_test_pattern(void) {
    // 8 SMPTE bars × 100 px wide × full framebuffer height.
    for (uint16_t y = 0; y < FB_H; y++) {
        uint8_t *line = framebuf[y];
        for (uint16_t x = 0; x < FB_W; x++) {
            line[x] = SMPTE_BARS[x / 100u];
        }
    }
}

int main(void) {
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    fb_init();
    build_test_pattern();

    // 50/60 Hz mode switch on GPIO 26 (pull-up; closed = 50 Hz)
    gpio_init(PIN_SWITCH);
    gpio_set_dir(PIN_SWITCH, GPIO_IN);
    gpio_pull_up(PIN_SWITCH);
    sleep_ms(1);
    bool is_50hz = !gpio_get(PIN_SWITCH);

    vga_output_init(is_50hz);
    capture_init();
    vga_output_start();

    // Core 0 owns the capture polling loop from here on
    capture_run_forever();
    return 0;
}
