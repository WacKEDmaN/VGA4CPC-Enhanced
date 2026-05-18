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

int main(void) {
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    fb_init();
    // Paint the "no signal" test pattern. The capture loop will
    // overwrite it line-by-line once the CPC starts sending video,
    // and will repaint it whenever sync is lost for >3 seconds.
    fb_paint_test_pattern();

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
