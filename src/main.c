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

    // *** 50 Hz colour fix via clock_configure() trick ***
    // clock_configure() does NOT reconfigure the PLL — it just selects
    // AUX (pll_sys at default ~125 MHz) as clk_sys. The 128 MHz argument
    // is what the SDK *believes* the clock is — actual is ~125 MHz.
    // That tiny 3 MHz offset is critical: rgbin sample rate becomes
    // ~15.625 MHz (vs CPC's 16 MHz pixel rate), so sample phase drifts
    // across CPC pixels instead of locking onto pixel-edge transitions.
    // set_sys_clock_khz(128000) would force EXACT 128 MHz and reintroduce
    // the lock-on colour bug.
    //
    // 60 Hz: set_sys_clock_khz(160000) for exact 40 MHz DMT output.
    // Colour bug present in 60 Hz mode but accepted for crisp display.
    if (is_50hz) {
        clock_configure(clk_sys,
                        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                        SYS_CLOCK_KHZ_50HZ * 1000, SYS_CLOCK_KHZ_50HZ * 1000);
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
