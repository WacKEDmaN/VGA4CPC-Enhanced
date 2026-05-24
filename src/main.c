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
#include "hardware_config.h"
#include "framebuffer.h"
#include "capture.h"
#include "vga_output.h"

int main(void) {
    // Read the 50/60 Hz switch *before* picking sys_clock — the 50 Hz
    // path keeps the upstream 128 MHz timing untouched, while 60 Hz
    // wants 160 MHz for exact-DMT 800×600p60.
    gpio_init(PIN_SWITCH);
    gpio_set_dir(PIN_SWITCH, GPIO_IN);
    gpio_pull_up(PIN_SWITCH);
    sleep_ms(1);
    bool is_50hz = !gpio_get(PIN_SWITCH);

    // Proper PLL-locked sys_clock for both modes — no clock_configure
    // tricks. 50 Hz mode used to lie to the SDK to get an "accidental"
    // ~125 MHz sys for capture-clock detune; now that capture has its
    // own per-mode detune via the rgbin SM clkdiv, all PIO timings can
    // run at their honest design values and the picture stops being
    // squashed by the 2.34 % undersample.
    //
    //   50 Hz: exact 128 MHz → hsync_50/vsync_50 clkdiv 18+240/256
    //          → 6.76 MHz SM (CEA-861-spec 31.25 kHz line rate);
    //          rgb_50 clkdiv 1.0 → 128 MHz SM → 32 MHz pixel for the
    //          800-into-720 overscan trick; capture clkdiv detuned
    //          to break the lock-on bug.
    //   60 Hz: exact 160 MHz → 40 MHz pixel for DMT 800×600p60.
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
