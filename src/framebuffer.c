#include "framebuffer.h"
#include "hardware_config.h"
#include <string.h>

// Full 800 × 288 framebuffer. Each byte is one CPC pixel in the layout
// bits[5:0] = R_HI R_LO G_HI G_LO B_HI B_LO (matches DAC pin order
// GPIO 14..19 when output via PIO `out pins, 6`).
//
// 288 × 800 = 230 400 bytes. Fits in RP2040 SRAM (264 KB).
uint8_t framebuf[FB_H][FB_STRIDE] __attribute__((aligned(4)));

void fb_init(void) {
    memset(framebuf, 0, sizeof(framebuf));
}

uint8_t *fb_get_write_line(uint16_t n) {
    if (n >= FB_H) return NULL;
    return framebuf[n];
}

const uint8_t *fb_get_read_line(uint16_t n) {
    if (n >= FB_H) return framebuf[FB_H - 1];
    return framebuf[n];
}

void fb_commit_line(uint16_t n) {
    (void)n;
}

void fb_new_frame(void) {
}
