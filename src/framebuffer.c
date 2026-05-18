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

// =====================================================================
// Test pattern — "NO SIGNAL / PLEASE STAND BY" card
// =====================================================================
// Drawn procedurally; no embedded image. Painted once at boot and
// repainted by the capture loop after >3 s of no CPC sync.
// =====================================================================

// Pack RR GG BB DAC levels (each 0..3) into one framebuffer byte.
#define DAC_RGB(r,g,b) (uint8_t)(((r)<<4) | ((g)<<2) | (b))

static const uint8_t COL_BLACK   = DAC_RGB(0,0,0);
static const uint8_t COL_WHITE   = DAC_RGB(3,3,3);
static const uint8_t COL_RED     = DAC_RGB(3,0,0);
static const uint8_t COL_YELLOW  = DAC_RGB(3,3,0);
static const uint8_t COL_GREEN   = DAC_RGB(0,3,0);
static const uint8_t COL_CYAN    = DAC_RGB(0,3,3);
static const uint8_t COL_BLUE    = DAC_RGB(0,0,3);
static const uint8_t COL_MAGENTA = DAC_RGB(3,0,3);

// ---------------------------------------------------------------------
// Minimal 8x8 bitmap font, only the characters we need for the two
// banners ("NO SIGNAL" and "PLEASE STAND BY"). MSB is the leftmost pixel.
// ---------------------------------------------------------------------
typedef struct {
    char ch;
    uint8_t bits[8];
} Glyph;

static const Glyph GLYPHS[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}},
    {'4', {0x0C,0x1C,0x2C,0x6C,0xFF,0x0C,0x0C,0x00}},
    {'A', {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}},
    {'C', {0x7E,0xC3,0xC0,0xC0,0xC0,0xC3,0x7E,0x00}},
    {'D', {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}},
    {'E', {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}},
    {'G', {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}},
    {'H', {0xC3,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0x00}},
    {'I', {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}},
    {'L', {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}},
    {'N', {0xC3,0xE3,0xF3,0xDB,0xCF,0xC7,0xC3,0x00}},
    {'O', {0x7E,0xC3,0xC3,0xC3,0xC3,0xC3,0x7E,0x00}},
    {'P', {0xFC,0x66,0x66,0xFC,0x60,0x60,0x60,0x00}},
    {'S', {0x7E,0xC0,0xC0,0x7E,0x03,0x03,0x7E,0x00}},
    {'T', {0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x00}},
    {'V', {0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x00}},
    {'Y', {0xC3,0xC3,0xC3,0x7E,0x18,0x18,0x18,0x00}},
};

static const uint8_t *find_glyph(char c) {
    for (unsigned i = 0; i < sizeof(GLYPHS)/sizeof(GLYPHS[0]); i++) {
        if (GLYPHS[i].ch == c) return GLYPHS[i].bits;
    }
    return GLYPHS[0].bits; // blank for unknowns
}

// Fill a rectangular region of the framebuffer with `colour`.
static void fill_rect(uint16_t x0, uint16_t y0,
                      uint16_t w,  uint16_t h, uint8_t colour) {
    uint16_t x1 = x0 + w; if (x1 > FB_W) x1 = FB_W;
    uint16_t y1 = y0 + h; if (y1 > FB_H) y1 = FB_H;
    for (uint16_t y = y0; y < y1; y++) {
        uint8_t *row = framebuf[y];
        for (uint16_t x = x0; x < x1; x++) row[x] = colour;
    }
}

// Draw one glyph at (x, y) scaled up by (sx, sy) in `colour`.
static void draw_glyph(uint16_t x, uint16_t y, char c,
                       uint8_t sx, uint8_t sy, uint8_t colour) {
    const uint8_t *g = find_glyph(c);
    for (uint8_t gy = 0; gy < 8; gy++) {
        uint8_t row = g[gy];
        for (uint8_t gx = 0; gx < 8; gx++) {
            if (!(row & (0x80u >> gx))) continue;
            fill_rect(x + gx*sx, y + gy*sy, sx, sy, colour);
        }
    }
}

// Draw a string centred horizontally on the framebuffer at the given y.
// 7 visible glyph cols + 1 spacing col = 8 cols per char (× sx pixels).
static void draw_text_centred(const char *s, uint16_t y,
                              uint8_t sx, uint8_t sy, uint8_t colour) {
    uint16_t n = 0;
    for (const char *p = s; *p; p++) n++;
    uint16_t width = (uint16_t)(n * 8u * sx);
    uint16_t x = (FB_W > width) ? (FB_W - width) / 2u : 0u;
    for (uint16_t i = 0; i < n; i++) {
        draw_glyph(x, y, s[i], sx, sy, colour);
        x += 8u * sx;
    }
}

void fb_paint_test_pattern(void) {
    // Vertical band layout (rows). Tweak as needed; must sum to FB_H = 288.
    enum {
        ROW_TOP_BANNER   = 0,    H_TOP_BANNER   = 32,
        ROW_TOP_CASTL    = 32,   H_TOP_CASTL    = 13,
        ROW_BARS         = 45,   H_BARS         = 83,
        ROW_GAP1         = 128,  H_GAP1         = 6,
        ROW_FREQ         = 134,  H_FREQ         = 48,
        ROW_GREYS        = 182,  H_GREYS        = 30,
        ROW_GAP2         = 212,  H_GAP2         = 6,
        ROW_BOT_BANNER   = 218,  H_BOT_BANNER   = 41,
        ROW_BOT_STRIPE   = 259,  H_BOT_STRIPE   = 29,
    };

    // ----- Black background everywhere -----
    memset(framebuf, COL_BLACK, sizeof(framebuf));

    // ----- "NO SIGNAL" banner (rows 0..31) -----
    // Black background already; white text scale 3x4 (24 wide × 32 tall).
    // "NO SIGNAL" = 9 chars × 24 = 216 px wide → centred automatically.
    draw_text_centred("NO SIGNAL", ROW_TOP_BANNER, /*sx*/3, /*sy*/4, COL_WHITE);

    // ----- Top castellation (rows 32..44) — alternating black/white 20-px squares -----
    for (uint16_t x = 0; x < FB_W; x += 20) {
        uint8_t c = ((x / 20u) & 1u) ? COL_WHITE : COL_BLACK;
        fill_rect(x, ROW_TOP_CASTL, 20, H_TOP_CASTL, c);
    }

    // ----- Six colour bars (rows 45..127) -----
    // Yellow, Cyan, Green, Magenta, Red, Blue — 800 / 6 ≈ 133 px each
    {
        static const uint8_t bars[6] = {
            DAC_RGB(3,3,0), DAC_RGB(0,3,3), DAC_RGB(0,3,0),
            DAC_RGB(3,0,3), DAC_RGB(3,0,0), DAC_RGB(0,0,3),
        };
        for (int i = 0; i < 6; i++) {
            uint16_t x0 = (uint16_t)((i       * FB_W) / 6u);
            uint16_t x1 = (uint16_t)(((i+1)   * FB_W) / 6u);
            fill_rect(x0, ROW_BARS, (uint16_t)(x1 - x0), H_BARS, bars[i]);
        }
    }

    // ----- Frequency burst (rows 134..181) -----
    // Vertical white-on-black stripes, with stripe width decreasing from
    // 16 px (leftmost group) to 1 px (rightmost group). 8 equal-width
    // groups, each 100 px wide.
    {
        static const uint8_t widths[8] = {16, 12, 8, 6, 4, 3, 2, 1};
        for (int g = 0; g < 8; g++) {
            uint16_t gx0 = (uint16_t)(g * 100);
            uint8_t  w   = widths[g];
            for (uint16_t x = gx0; x < gx0 + 100u; x += (uint16_t)(w * 2)) {
                fill_rect(x, ROW_FREQ, w, H_FREQ, COL_WHITE);
                // gap of equal width remains black (already filled)
            }
        }
    }

    // ----- Greyscale ramp (rows 182..211) — 4 levels × 200 px -----
    for (uint8_t lvl = 0; lvl < 4; lvl++) {
        uint16_t x0 = (uint16_t)(lvl * 200);
        fill_rect(x0, ROW_GREYS, 200, H_GREYS, DAC_RGB(lvl, lvl, lvl));
    }

    // ----- "VGA4CPC-ENHANCED" banner (rows 218..258) -----
    // 16 chars × 24 px = 384 px wide → centred. Text inset 4 rows from
    // band top so it sits centred in the 41-row band.
    draw_text_centred("VGA4CPC-ENHANCED", ROW_BOT_BANNER + 4,
                      /*sx*/3, /*sy*/4, COL_WHITE);

    // ----- Bottom yellow strip with central red marker (rows 259..287) -----
    fill_rect(0, ROW_BOT_STRIPE, FB_W, H_BOT_STRIPE, COL_YELLOW);
    {
        uint16_t cw = 80;                             // red marker width
        uint16_t cx0 = (FB_W - cw) / 2u;
        fill_rect(cx0, ROW_BOT_STRIPE, cw, H_BOT_STRIPE, COL_RED);
    }
}
