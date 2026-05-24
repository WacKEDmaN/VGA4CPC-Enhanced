# VGA4CPC-Enhanced

A direct **Amstrad CPC → VGA scan-doubler** firmware for the
[grzegorz-gr/vga4cpc](https://github.com/grzegorz-gr/vga4cpc) PCB
(Raspberry Pi Pico based).

Captures the live CPC RGB+CSYNC signal at 16 MHz, line-doubles it, and
emits a clean **576p50** or **800×600p60** VGA signal — no jitter, no
dropped lines, full-resolution including borders/overscan.

This is a from-scratch reimplementation of the capture and output
pipeline using custom PIO programs on both PIO blocks. The VGA-output
PIO programs (`hsync_50/60`, `vsync_50/60`, `rgb_50/60`) and the
capture PIOs (`rgbin`, `vsyncgen`) are ported directly from the
upstream grzegorz-gr/vga4cpc firmware — full credit to **gregg** for
the original timings and PIO designs.

---

## Screenshots

> *Captured via: CPC → VGA4CPC PCB (running this firmware) → VGA-to-HDMI
> converter → USB-HDMI capture dongle → PC. Some softness and chroma
> fringing visible in the captures is from the capture chain itself, not
> the scan-doubler output — on a VGA monitor connected directly to the
> PCB the image is appreciably sharper.*

CPC BASIC 1.1 prompt on an Amstrad 6128 — the two display modes side by
side (both ship in the same firmware; press **BOOTSEL** at runtime to
toggle between them):

| Normal mode | Scanlines mode |
|:---:|:---:|
| ![Normal mode — clean line-doubled output](images/VGA4CPC_NORMAL.png) | ![Scanlines mode — CRT-style dark gap between scanlines](images/VGA4CPC_SCANLINES.png) |

Running real CPC software — Batman intro logo and a mode-0 cutscene
illustrating the full colour range and borders the scan-doubler
preserves:

| | |
|:---:|:---:|
| ![Batman intro logo](images/demo_batman_logo.png) | ![Mode-0 cutscene from Batman](images/demo_batman_scene.png) |

### Videos

Click through to YouTube for live captures of the firmware running:

| Normal mode + "no signal" hand-off | Scanlines mode |
|:---:|:---:|
| [![Normal mode live, with the "no signal" card switching in when the CPC powers off](https://img.youtube.com/vi/u6UePIkd_Jg/hqdefault.jpg)](https://www.youtube.com/watch?v=u6UePIkd_Jg) | [![Scanlines mode live, showing the CRT-style dark-gap scanlines](https://img.youtube.com/vi/mkgNcqdlhVY/hqdefault.jpg)](https://www.youtube.com/watch?v=mkgNcqdlhVY) |

A longer, more demanding test — [**SymbOS**](https://www.symbos.org/) running
jitter-free, with the BOOTSEL scanlines toggle shown switching modes on
the fly:

| SymbOS demo + live scanlines toggle |
|:---:|
| [![SymbOS on CPC through the scan-doubler, with the BOOTSEL scanlines toggle](https://img.youtube.com/vi/wtBRFJdr2-E/hqdefault.jpg)](https://www.youtube.com/watch?v=wtBRFJdr2-E) |

---

## Hardware

- Raspberry Pi Pico (RP2040)
- grzegorz-gr/vga4cpc PCB (TLV3202 dual comparators + 27-colour summing resistor network)
- 50/60 Hz slide switch on GPIO 26
- LED on GPIO 25 (status / diagnostic)

### Pin map

| GPIO  | Function                                  |
|-------|-------------------------------------------|
| 0–5   | CPC RGB input (B_LO, B_HI, G_LO, G_HI, R_LO, R_HI) |
| 6     | CSYNC input (active low)                  |
| 10    | vsyncgen output (PCB internally routes this to GPIO 7) |
| 12    | VGA HSYNC                                 |
| 13    | VGA VSYNC                                 |
| 14–19 | 27-colour summing network (R/G/B × 2 equal-weighted bits each) |
| 25    | LED                                       |
| 26    | 50/60 Hz mode switch (closed/LOW = 50 Hz) |

> **Note: GPIO 10 → GPIO 7 is hardwired on the PCB.**
> The capture path generates a clean VSYNC signal in software (PIO1
> SM0 derives it from the noisy composite CSYNC) and drives it out on
> GPIO 10. The rgbin SM then reads that VSYNC back via GPIO 7. The
> VGA4CPC PCB already connects these two pins internally — **nothing
> for the user to wire up**. This is just a heads-up if you're
> repurposing the firmware on a different board, where you'd need to
> create that connection yourself.

---

## Flash & use

**Most users want this section, not the Build section below.** A
single prebuilt firmware image is committed to [`dist/`](dist/):

- [`dist/vga4cpc_enhanced.uf2`](dist/vga4cpc_enhanced.uf2)

1. Hold BOOTSEL while plugging the Pico into USB, then drop the
   `.uf2` file from `dist/` onto the `RPI-RP2` drive.
2. Plug the VGA cable into your monitor.
3. Connect the CPC's RGB cable to the PCB.
4. Pick the output mode with the slide switch on GPIO 26:
   - **closed / LOW** → 576p50 (CEA-861, ~50.08 Hz, scaled to ~800px wide)
   - **open / HIGH**  → 800×600p60 (DMT)

   The firmware notices the switch position changing at runtime and
   reboots itself cleanly to apply the new mode (~250 ms blank, then
   the new mode comes up). No manual reset needed.

### BOOTSEL button — runtime settings

The BOOTSEL button on the Pico drives two settings, distinguished by
hold duration:

#### Short press (< 1.5 s) — scanlines toggle

Each quick tap toggles the CRT-style scanlines effect on or off.
With scanlines on, every 2nd output row is replaced by a fully black
line, giving the classic 50%-density CRT look.

Note: this firmware can't do true CRT-style content-aware dimming
(where every other line shows the same picture but darker) — that
needs a second framebuffer's worth of RAM, more than the RP2040
has spare.

#### Long press (≥ 1.5 s) — right-edge trim

Some CPC games (e.g. Prehistorik 2) use CRTC timing tricks that emit
pixel data past their intended visible area — bytes a real CPC's
monitor would have hidden via overscan. Our scan-doubler captures
them verbatim, which can show up as garbage / coloured lines on the
right edge of the screen.

The right-edge trim overwrites the rightmost N captured pixels of
each line with the value of the leftmost pixel (assumed to be the
border colour) — hiding that garbage while preserving the border.

Hold BOOTSEL for **at least 1.5 seconds** and then release to cycle
through four trim levels:

| Trim level | Pixels trimmed | When to use                              |
|:----------:|:--------------:|:-----------------------------------------|
| 0 (default) | 0  (off)      | No trim — full 800-pixel capture shown   |
| 1          | 32             | Mild garbage on the right edge           |
| 2          | 64             | More noticeable garbage (most games)     |
| 3          | 96             | Heavy CRTC-trickery games (Prehistorik 2) |

The on-screen effect: the rightmost N pixels become a uniform border
colour. Step up one level at a time until the garbage disappears.

#### Persistence

Both settings are **persisted to the last 4 KB sector of the Pico's
flash**, so they survive power cycles — set your preferences once and
the firmware boots into the same state next time. First-ever boot
(blank persistence sector) defaults to Off / 0.

The on-board LED (GPIO 25, PWM-dimmed to ~half brightness) indicates
sync state:
- **Solid (dim)** → CPC sync detected, frames being captured.
- **Flashing at ~1 Hz** → waiting for sync (no CPC connected, CPC
  powered off, or signal loss).

### "No signal" test pattern

![No-signal test card as displayed on the monitor](images/NO_SIGNAL.png)

When no CPC signal is present, the firmware paints a "NO SIGNAL /
VGA4CPC-ENHANCED" test card into the framebuffer so the monitor
isn't left showing a frozen last-frame or a blank screen.

- **At boot**, the card appears immediately. As soon as the CPC starts
  sending video, capture overwrites the card line-by-line and live CPC
  content takes over within one frame (~20 ms).
- **When the CPC is powered off**, the firmware notices the loss of
  HSYNC pulses within ~200 ms, then waits a further **3 seconds** of
  total silence before painting the card. This guard against brief
  hiccups (cable wobble, manual reset, etc.) avoids spurious flashes
  to the "no signal" screen.
- **When the CPC comes back**, live video resumes automatically inside
  one frame — no key press, no reboot required.

The card itself shows a "NO SIGNAL" banner, a castellation row, six
full-bright colour bars (Y/C/G/M/R/B), a frequency burst pattern, a
4-step greyscale ramp, a "VGA4CPC-ENHANCED" banner, and a 27-cell
strip showing every native CPC colour (3³ combinations of the three
per-channel voltage levels). All drawn procedurally; no embedded
image asset. Text is rendered in a verbatim copy of the CPC6128 OS
ROM character set.

---

## Build

Only needed if you want to modify the firmware. Prebuilt UF2s are in
[`dist/`](dist/) — see *Flash & use* above.

Requirements:
- **Raspberry Pi Pico SDK v1.5.1** — install from
  [the official Pico setup guide](https://github.com/raspberrypi/pico-sdk#quick-start-your-own-project).
  The Windows installer bundles everything; on Linux/macOS you'll set
  `PICO_SDK_PATH` to point at your checkout.
- **CMake**, **Ninja**, and **arm-none-eabi-gcc** (all included in the
  Windows Pico installer; install separately on Linux/macOS via your
  package manager).

### Windows one-click

```cmd
build.cmd
```

Builds the single firmware into `dist\vga4cpc_enhanced.uf2`.

The script assumes the default Windows SDK path
(`C:\Program Files\Raspberry Pi\Pico SDK v1.5.1`). Edit the `SDK`
variable at the top of `build.cmd` if yours lives elsewhere.

### Manual (any platform)

```sh
mkdir build && cd build
cmake -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja
```

`PICO_SDK_PATH` can also be set as an environment variable in your
shell so you don't need to repeat it. The `CMakeLists.txt` default
points at the Windows install location, so passing `-DPICO_SDK_PATH`
explicitly is recommended on Linux/macOS.

Scanlines are a **runtime** option now — there's no compile flag for
them. The single UF2 contains both normal and scanlines modes; the
user toggles at runtime via the BOOTSEL button.

---

## Architecture

```
   CPC               Pico (~125 / 160 MHz)            VGA
   ───               ────────────────────             ───

  RGB ──► GPIO 0-5 ──► PIO1 SM1 (rgbin) ──┐
  CSYNC ► GPIO 6  ──► PIO1 SM0 (vsyncgen)─┤
                                          │
                              Core 0 polling loop
                                          │
                              ┌───── DMA (per line) ──► framebuf[288][800]
                              │
                              └───── 4-channel DMA ring (line-doubled,
                                     each fb line streamed twice)
                                          │
                                          ▼
                              PIO0 SM2 (rgb) ──► GPIO 14-19 ──► 27-colour network
                              PIO0 SM0 (hsync) ► GPIO 12     ─┐
                              PIO0 SM1 (vsync) ► GPIO 13     ─┘
```

- **`sys_clock` is mode-dependent:**
  - **50 Hz:** `clock_configure()` selects pll_sys as clk_sys source.
    This call does **not** actually reconfigure the PLL — clk_sys stays
    at the boot-default ~125 MHz. The SDK runtime *believes* it's
    128 MHz (the value we pass in), so timing-derived helpers stay
    consistent, but the physical clock is ~3 MHz lower. That tiny
    mismatch is **load-bearing** — see the Known limitations section
    for why. `set_sys_clock_khz(128000)` would force exact 128 MHz and
    reintroduce the rgbin lock-on colour bug.
  - **60 Hz:** `set_sys_clock_khz(160000)` for exact 160 MHz so the
    output side hits exactly 40 MHz pixel clock (160 ÷ 4 = VESA DMT
    800×600p60 spec). rgbin samples at exactly 16 MHz here, which
    *does* trigger the lock-on bug — accepted trade-off so the
    monitor can lock cleanly. See Known limitations.
- **PIO1** runs the capture side: `vsyncgen` discriminates CSYNC into a
  VSYNC level on GPIO 10 (wired back to GPIO 7 on the PCB);
  `rgbin` is a per-line, HSYNC-aligned sampler that pushes one
  byte per pixel to its RX FIFO.
- **Core 0** owns the capture loop forever: busy-polls CSYNC and
  VSYNC, fires a fresh DMA per CPC line straight from the RX FIFO
  into a framebuffer row.
- **PIO0** runs the VGA output side: three SMs cooperating via PIO
  IRQs to produce HSYNC/VSYNC/RGB.
- **4-channel DMA chain** produces line-doubled output by streaming
  each framebuffer row to the RGB SM's TX FIFO twice in a row — so
  288 captured CPC lines drive 576 VGA output lines, scan-doubling to
  576p without any extra capture or memory cost. (Each CPC line is
  still captured only once; the duplication happens entirely on the
  output side, via the DMA's source-pointer table listing each row
  pointer twice.)

### Pixel byte layout

CPC's 6-bit thermometer-coded RGB comes out of the comparators in the
exact bit order the output network wants:

```
  bit 5 4 3 2 1 0
       R R G G B B
```

So the capture DMA writes raw 6-bit samples into the framebuffer, and
the output DMA streams those same bytes to the GPIO pins. **No palette
lookup is needed.**

The output network on the PCB is **not** a binary-weighted R-2R DAC.
Each channel has two 220 Ω resistors of **equal value** summing two
GPIO pins onto one VGA RGB pin. Equal-weighted means the two bits per
channel collapse into only 3 distinct voltage levels:

| Bits (HI, LO) | Resistor network                 | Output voltage |
|:---:|:---|:---:|
| `00` | both GPIOs low      | 0 V (LOW)               |
| `01` | one GPIO high       | V/2 (MID)               |
| `10` | one GPIO high       | V/2 (MID — *same as `01`*) |
| `11` | both GPIOs high     | V (HIGH)                |

Three levels per channel × three channels = **27 native colours** —
exactly the CPC's native palette. The PCB is sized for the CPC's
27 inks and nothing more; codes `01` and `10` are electrically
indistinguishable at the VGA pin.

The level-2 sanitiser in `capture.c` (`byte |= (byte >> 1) & 0x15`
per word) promotes any transient `10` thermometer code captured at
edges to `11`. On a binary-weighted DAC this would lift "half-bright"
glitches up to "bright"; on the equal-weighted network it lifts MID
to HIGH, which is the right call because the *intent* of a `10`
glitch is always "the bright value was caught mid-transition, the LO
bit hasn't risen yet."

### Horizontal sizing

The CPC produces ~800 active pixels per line at 16 MHz (full visible
including borders). The `rgb_50` PIO sends 800 pixels into a 720-pixel
576p50 timing window — i.e. it overscans by ~11%, which makes the full
CPC display (borders included) fit a typical VGA monitor's visible
area. Monitors still detect a standard 720×576p50 signal.

### Display modes

A single firmware UF2 ships both display modes; the user toggles between
them at runtime via short BOOTSEL presses (see *Flash & use* above):

| Mode             | Description                                   |
|------------------|-----------------------------------------------|
| Normal (default) | Plain colour scan-doubled output              |
| Scanlines        | CRT-style dark gap between every pair of lines|

The scanlines effect is implemented by flipping every odd-indexed
entry of the output-DMA's source-pointer ring between the framebuffer
rows and a single all-black row. Each `line_src[]` slot is a single 32-bit pointer,
written atomically from the capture loop's per-frame BOOTSEL check, so
the swap has no measurable effect on capture / output performance.

Earlier development versions experimented with monochrome / amber /
green "P1/P3 phosphor" effects, but they were abandoned because the
LUT pass through the framebuffer couldn't keep up with the per-CPC-line
capture deadline.

---

## Known limitations

The firmware ships with one of two engineering trade-offs picked per
mode. **You can't have both** "crisp output, no W-aliasing" *and*
"perfect colour capture" simultaneously on RP2040 — the math literally
doesn't work at any clock the chip can hit safely.

### Root cause: rgbin sample-rate lock-on

The TLV3202 comparators on the VGA4CPC PCB convert the CPC's analog
RGB to a 6-bit thermometer code (2 bits per channel, R/G/B). The
comparator output settles ~5–10 ns after the input voltage changes —
the rest of the CPC's 62.5 ns pixel period it's stable. Sampling
inside the transition window catches an indeterminate value. **Some
transients pull bits LOW** (paper reads as black instead of red);
others push bits HIGH; both can happen at different transitions.

If the rgbin sample rate equals the CPC pixel rate **exactly**
(16 MHz), every one of the 800 samples in a line lands at the *same*
phase relative to every CPC pixel boundary. If that phase coincides
with the comparator transition zone, **every captured byte is wrong**,
and the error is consistent across the whole line — so e.g. `INK 0,3:
BORDER 6` shows the paper going solid black.

If the sample rate is close to but *not exactly* 16 MHz, the phase
**drifts** across CPC pixels — some samples land in the transition
zone (corrupt), most don't (correct). Statistically the correct
values dominate and the bug is invisible.

```
  Lock-on (16 MHz exact):     │t│t│t│t│t│t│t│   t = transition phase
                              every sample at the same bad phase

  Drift (15.625 or 16.875):   │s│s│s│s│t│s│s│   s = stable phase
                              one sample in 20-40 hits transition,
                              statistically negligible
```

### The clock arithmetic

To get a usable image we need three things:

1. **rgbin sample rate ≈ 16 MHz** (so 800 samples cover CPC's 50 µs
   active video) but **not exactly 16 MHz** (avoid lock-on).
2. **rgb output rate = monitor's pixel-sample rate** (no W-aliasing /
   jitter on thin diagonals). 50 Hz CEA-861 wants 27 MHz; 60 Hz DMT
   wants 40 MHz.
3. **Integer PIO clkdivs everywhere** (fractional clkdivs introduce
   their own per-cycle jitter).

Achieving (1) and (2) together requires `sys_clock` to be divisible
by *both* the rgbin's per-sample cycle count × ~16 MHz *and* the
output's per-pixel cycle count × {27, 40} MHz.

For **50 Hz**: rgbin at 8 cyc/sample + 27 MHz output → sys_clock
must be a multiple of `LCM(16, 27) = 432 MHz`. Far beyond what
RP2040 can do.

For **60 Hz**: rgbin at 8 cyc/sample + 40 MHz output → sys_clock
must be a multiple of `LCM(16, 40) = 80 MHz`. The natural value is
160 MHz, which works for output (160 ÷ 4 = 40 MHz exact) but forces
rgbin to exactly 16 MHz — i.e. lock-on.

There is no integer-divider solution on RP2040 that satisfies all
three constraints in *either* mode.

### The per-mode compromise this firmware ships

| Mode  | sys_clock                 | rgb output           | rgbin sample rate | Result                                                         |
|-------|---------------------------|----------------------|-------------------|----------------------------------------------------------------|
| 50 Hz | ~125 MHz (boot default, via `clock_configure()` no-op trick) | ~31.25 MHz (4 cyc/pixel at SM 125 MHz) | ~15.625 MHz (drift) | **Colours correct.** Output rate mismatches monitor's 27 MHz → light W-aliasing / shimmer on thin diagonals. |
| 60 Hz | 160 MHz exact             | 40 MHz exact (DMT)   | 16 MHz exact (lock-on) | **Output crisp** — 1:1 lock to monitor's pixel-sample timing. Colour bug present: paper can read black with certain bright borders, brightness can shift when border colour changes. |

50 Hz prioritises correct colour over crisp output because most CPC
content (text, paper background, games) doesn't suffer much from
mild W-aliasing, whereas the colour bug is far more disruptive
visually. 60 Hz prioritises crisp output because at 800×600p60 the
monitor expects exact-DMT timing; deviating even slightly causes
some monitors to refuse the signal or display colour gradients.

### Why we don't just oversample + average

The obvious fix is: sample the CPC twice per pixel (= 32 MHz), then
combine each pair on the CPU. We tried this in 60 Hz mode.

It doesn't work on RP2040 because:

- Pixel-by-pixel combine on the CPU takes ~30 µs per CPC line. The
  CPC line is only 64 µs total, with DMA blocking us for ~50 µs of
  that. The remaining ~14 µs HBP window is too short, and processing
  off-frame creates a 1-line latency that conflicts with our
  single-buffered framebuffer.
- 2-sample combining via OR or AND can't recover the right value
  because transients go *both directions* (HIGH and LOW). True
  majority voting requires 3+ samples per pixel — 48+ MHz raw rate
  — which doesn't divide cleanly from any RP2040-safe `sys_clock`.

Pico 2 (RP2350) at 200+ MHz with a third PIO block makes this
tractable; see the Pico 2 roadmap section.

---

## Optional hardware mods

The Known limitations above are firmware/architectural — a hardware
mod won't make the colour bug or W-aliasing go away on RP2040. But
the buffer mod below is still worth considering: it isolates the
Pico's 3.3 V rail from the summing-network's switching current,
which reduces analog noise on the input side and *may* slightly
improve 60 Hz colour stability. Not required.

### 74HCT245 / 74HC245 buffer — full output decoupling

Drop a logic buffer between the Pico's GPIO 14–19 outputs and the
summing resistor network's inputs. The buffer chip has its own
supply pins and presents a tiny gate capacitance to the Pico, so the
network's current load no longer pulls on the Pico's 3.3 V rail at
all. Quieter rail → quieter 200 mV / 600 mV comparator references
on the input side → slightly cleaner captures.

Note: the buffer mod does **not** unlock more colours — the network
is 27 colours by design (3 levels × 3 channels) and the buffer just
restates the same digital values into the same equal-weighted
resistors. It also does **not** fix the rgbin lock-on bug
(that's a clock-arithmetic issue, not an analog one).

| Component | Spec | Notes |
|-----------|------|-------|
| 74HCT245 (or 74HC245) | Octal bus transceiver, fixed direction | "T" variant has TTL-compatible inputs which are slightly better at 3.3 V drive |
| Power for the buffer | 5 V from Pico's VBUS (pin 40), or a clean 3.3 V from a separate LDO | Buffer outputs drive the resistor network |
| Decoupling | 0.1 µF ceramic at the buffer's VCC pin | Standard practice |

Wire-up:
- `DIR` = VCC (always Pico → network)
- `OE` = GND (always enabled)
- A1..A6 = Pico GPIO 14..19
- B1..B6 = inputs to the 220 Ω summing resistors
- B7, B8 unused (tie to GND or leave floating per datasheet)

This is a PCB-level mod — not just an add-on cap — so it's more
involved. Colour count is unchanged (still 27); the network's
structure determines that, not the source impedance.


---

## Pico 2 roadmap

The PCB is pin-compatible with the **Raspberry Pi Pico 2 (RP2350)**,
which would let us escape the RP2040's clock-arithmetic corner and
fix both engineering trade-offs simultaneously:

| Pico 2 resource           | What it unlocks                                     |
|---------------------------|-----------------------------------------------------|
| Up to 300 MHz core clock  | Hit `sys_clock = 432 MHz` (= `LCM(16, 27)`) so 50 Hz mode can have *exact* 27 MHz output AND drifting rgbin sampling simultaneously — both trade-offs gone in one move. 432 MHz exceeds spec but the silicon usually goes there with vreg bump; even 216 MHz (well within Pico 2 spec) lets rgbin run at 13 cyc/sample = 16.6 MHz drift while rgb_50 runs 8 cyc/pixel = 27 MHz. |
| Cortex-M33 @ 150+ MHz × 2 | Per-line 3-sample majority-vote averaging fits comfortably in the CPC's 64 µs line budget — the proper fix for the 60 Hz colour bug (no lock-on, no oversample loss). |
| 520 KB SRAM (vs 264 KB)   | Double-buffered framebuffer eliminates output-side tearing race; per-line scratch buffer for the 3-sample averaging path. |
| 3rd PIO block (12 SMs)    | Dedicated rgbin variants for each mode without juggling the 4-SM PIO1 budget; potentially a CSYNC clock-recovery SM that phase-locks our sample clock to the CPC's pixel clock (which would solve lock-on at any sample rate). |

The residual *analog* limitations (comparator settle time, rail
droop, etc.) live on the PCB and only a hardware mod can address
them. But every limitation listed in the **Known limitations**
section above is fundamentally an RP2040 clock-arithmetic problem
and would go away on RP2350.

No Pico 2 port work has started — this section is here so anyone
picking up the project knows the way out.

---

## Credits

- **gregg** (grzegorz-gr) — original [`vga4cpc`](https://github.com/grzegorz-gr/vga4cpc) PCB design and the
  PIO programs this firmware is built on. The `hsync_*.pio`,
  `vsync_*.pio`, `rgb_*.pio`, `rgbin.pio` and `vsyncgen.pio` programs
  are taken from that project, with only minor refactoring.
- **Hunter Adams** — the original VGA-from-PIO timing approach the
  upstream firmware credits as its basis.
- **All C code, build scripts, and documentation in this repository
  were written by [Claude Code](https://claude.com/claude-code)
  (Anthropic) under direction from the project owner.**

---

## License

Released into the **public domain** under [The Unlicense](LICENSE).
Do whatever you want with it — no warranty.

The PIO programs in `src/` are adapted from the upstream
[grzegorz-gr/vga4cpc](https://github.com/grzegorz-gr/vga4cpc) project
(which itself has no stated license); the adaptations here are also
public-domain.
