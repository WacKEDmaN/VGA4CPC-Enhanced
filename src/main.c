// =====================================================================
// VGA4CPC-Enhanced — direct CPC → VGA scan doubler
//
// Custom-PIO scan-doubler firmware for the grzegorz-gr/vga4cpc PCB
// (Raspberry Pi Pico based). Captures the live CPC RGB+CSYNC signal
// at 16 MHz, line-doubles it, and emits 576p50 or 800×600p60 VGA.
//
//   sys_clock:  128 MHz in 50 Hz mode, 160 MHz in 60 Hz mode (see
//               hardware_config.h for rationale).
//   PIO0     :  VGA output (hsync, vsync, rgb SMs)
//   PIO1     :  CPC capture (vsyncgen + rgb sampler)
//
// The 50/60 Hz output mode is selected by the slide switch on GPIO 26
// at boot (closed/LOW = 576p50, open/HIGH = 800×600p60).
// =====================================================================

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware_config.h"
#include "framebuffer.h"
#include "capture.h"
#include "vga_output.h"

int main(void) {
    // Read the 50/60 Hz switch *before* picking sys_clock — each mode has
    // its own independent overclock (chosen so its clocks divide cleanly).
    gpio_init(PIN_SWITCH);
    gpio_set_dir(PIN_SWITCH, GPIO_IN);
    gpio_pull_up(PIN_SWITCH);
    sleep_ms(1);
    bool is_50hz = !gpio_get(PIN_SWITCH);

    // Both modes are overclocked to shrink the rgbin HSYNC-`wait` tick
    // (the per-line jitter source) while holding a decohered, off-16 MHz
    // capture rate (the colour fix). The two paths are fully independent:
    //
    //   50 Hz: 256 MHz (= 128 × 2). Every old divider just doubles, so all
    //          frequencies are unchanged (27 MHz output, 6.76 MHz sync,
    //          16 MHz vsyncgen) — the capture stays at the exact proven
    //          14.222 MHz (256/18) but the tick is 3.9 ns instead of 7.8.
    //          27 MHz output tolerates the resulting fractional dividers.
    //   60 Hz: 240 MHz. Chosen so EVERY clock divides to a clean integer
    //          (40 MHz DMT output = 240/6, vsyncgen = 240/15) — 40 MHz is
    //          fussy and went fractional/jittery at 256. Capture 14.118 MHz
    //          (240/17, off 16 by more than 14.222 → still decohered),
    //          tick 4.2 ns.
    //
    // Both need a core-voltage bump; 1.30 V covers 256 and 240.
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(2);                            // let the regulator settle
    if (is_50hz) {
        set_sys_clock_khz(SYS_CLOCK_KHZ_50HZ, true);
    } else {
        set_sys_clock_khz(SYS_CLOCK_KHZ_60HZ, true);
    }

    // LED stays off during init — capture_run_forever takes it over
    // and drives it via PWM for the rest of runtime.

    fb_init();
    // Paint the "no signal" test pattern. The capture loop will
    // overwrite it line-by-line once the CPC starts sending video,
    // and will repaint it whenever sync is lost for >3 seconds.
    fb_paint_test_pattern();

    vga_output_init(is_50hz);
    capture_init(is_50hz);
    vga_output_start();

    // Core 0 owns the capture polling loop from here on
    capture_run_forever();
    return 0;
}
