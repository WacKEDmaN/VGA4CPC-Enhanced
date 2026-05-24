# VGA4CPC-Enhanced

A direct **Amstrad CPC → VGA scan-doubler** firmware for the
[grzegorz-gr/vga4cpc](https://github.com/grzegorz-gr/vga4cpc) PCB
(Raspberry Pi Pico based).

Captures the live CPC RGB+CSYNC signal at ~16 MHz, line-doubles it, and
emits **576p50** or **800×600p60** VGA — full-resolution including
borders/overscan, with a per-mode runtime-tunable capture clock to
manage the unavoidable **colour bug ↔ jitter trade-off** that's
inherent on RP2040 (see *Known limitations* below — locked 16 MHz
sampling = perfect stability but wrong colours; detuned sampling =
correct colours but per-pixel jitter).

This is a from-scratch reimplementation of the capture and output
pipeline using custom PIO programs on both PIO blocks. The VGA-output
PIO programs (`hsync_50/60`, `vsync_50/60`, `rgb_50/60`) and the
capture PIOs (`rgbin_50`/`rgbin_60`, `vsyncgen`) are derived from the
upstream grzegorz-gr/vga4cpc firmware — full credit to **gregg** for
the original timings and PIO designs. `rgbin` was split into two
per-mode programs so each can have its own back-porch wait and detune
range without interfering with the other.

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

The BOOTSEL button on the Pico drives three settings, distinguished by
hold duration:

#### Short press (< 1.5 s) — capture-clock detune

Cycles the rgbin sample-clock divider through a per-mode table of
9 values (idx 0..8). Each value shifts the capture sample rate
slightly off the CPC's 16 MHz pixel rate — see *Known limitations*
below for why this knob exists.

Each mode has a different table, chosen at boot from the 50/60 Hz
switch and tuned to that mode's sys_clock:

| 60 Hz idx | rgbin clkdiv frac | Sample rate | % off 16 MHz |
|:--:|:--:|:--:|:--:|
| 0 | 64  | 16.000 MHz | 0 (locked — colour bug) |
| 1 | 65  | 15.950 MHz | −0.31 % |
| **2** | **66**  | **15.901 MHz** | **−0.62 % (default)** |
| 3 | 67  | 15.851 MHz | −0.93 % |
| 4 | 68  | 15.803 MHz | −1.23 % |
| 5 | 69  | 15.755 MHz | −1.53 % |
| 6 | 70  | 15.707 MHz | −1.83 % |
| 7 | 71  | 15.659 MHz | −2.13 % |
| 8 | 72  | 15.610 MHz | −2.44 % |

| 50 Hz idx | rgbin clkdiv frac | Sample rate | % off 16 MHz |
|:--:|:--:|:--:|:--:|
| 0 | 16 | 15.06 MHz | −5.9 % (often still bug) |
| 1 | 20 | 14.86 MHz | −7.2 % |
| 2 | 22 | 14.76 MHz | −7.8 % |
| 3 | 23 | 14.71 MHz | −8.1 % |
| **4** | **24** | **14.66 MHz** | **−8.4 % (default)** |
| 5 | 25 | 14.61 MHz | −8.7 % |
| 6 | 26 | 14.56 MHz | −9.0 % |
| 7 | 28 | 14.47 MHz | −9.6 % |
| 8 | 30 | 14.37 MHz | −10.2 % (near sample-loop overrun) |

Boot uses the default (bolded) value, which works on the reference
hardware. Different boards may need a different idx — cycle until
the colour bug disappears, then stop. The smaller the detune the
better (less inherent jitter).

50 Hz uses much heavier detune than 60 Hz because at sys=128 MHz
the base clkdiv is near-integer (1.0) and the fractional divider
needs larger frac values to produce enough sample-phase scatter to
escape the comparator-transition window. 60 Hz at sys=160 MHz with
base clkdiv 1.25 inherently has a 5/4 fractional pattern that
provides phase scatter "for free" — so tiny additional detune is
enough.

#### Medium press (1.5–5 s) — right-edge trim

Some CPC games (e.g. Prehistorik 2) use CRTC timing tricks that emit
pixel data past their intended visible area — bytes a real CPC's
monitor would have hidden via overscan. Our scan-doubler captures
them verbatim, which can show up as garbage / coloured lines on the
right edge of the screen.

The right-edge trim overwrites the rightmost N captured pixels of
each line with the value of the leftmost pixel (assumed to be the
border colour) — hiding that garbage while preserving the border.

Cycles through four trim levels:

| Trim level | Pixels trimmed | When to use                              |
|:----------:|:--------------:|:-----------------------------------------|
| 0 (default) | 0  (off)      | No trim — full 800-pixel capture shown   |
| 1          | 32             | Mild garbage on the right edge           |
| 2          | 64             | More noticeable garbage (most games)     |
| 3          | 96             | Heavy CRTC-trickery games (Prehistorik 2) |

#### Long press (> 5 s) — scanlines toggle

Toggles the CRT-style scanlines effect on or off. With scanlines on,
every 2nd output row is replaced by a fully black line, giving the
classic 50%-density CRT look.

Note: this firmware can't do true CRT-style content-aware dimming
(where every other line shows the same picture but darker) — that
needs a second framebuffer's worth of RAM, more than the RP2040
has spare.

#### Persistence

All three settings are **persisted to the last 4 KB sector of the
Pico's flash**, so they survive power cycles — set your preferences
once and the firmware boots into the same state next time. First-ever
boot (blank persistence sector) loads the per-mode defaults shown above.

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
   CPC               Pico (128 / 160 MHz)             VGA
   ───               ────────────────────             ───

  RGB ──► GPIO 0-5 ──► PIO1 SM1 (rgbin_50/60) ──┐
  CSYNC ► GPIO 6  ──► PIO1 SM0 (vsyncgen) ─────┤
                                                │
                                    Core 0 polling loop
                                                │
                                    ┌───── DMA (per line) ──► framebuf[288][800]
                                    │
                                    └───── 4-channel DMA ring (line-doubled,
                                           each fb line streamed twice)
                                                │
                                                ▼
                                    PIO0 SM2 (rgb_50/60) ──► GPIO 14-19 ──► 27-colour network
                                    PIO0 SM0 (hsync)     ──► GPIO 12       ─┐
                                    PIO0 SM1 (vsync)     ──► GPIO 13       ─┘
```

- **`sys_clock` is per-mode**, both PLL-locked to exact values (no
  clock-config tricks):
  - **50 Hz:** `set_sys_clock_khz(128000)` → exact 128 MHz. The 50 Hz
    output PIOs (`hsync_50`, `vsync_50`, `rgb_50`) were designed for
    this rate — `rgb_50` clkdiv 1+47/256 ≈ 27.04 MHz pixel for
    CEA-861 720×576p50 with **1:1 captured-pixel-to-monitor-pixel
    mapping** (no overscan trick, no horizontal squash). 720 of the
    800 captured pixels are sent per line; `FB_X_CROP_50HZ` in
    `hardware_config.h` chooses which 720-wide window (default 72,
    skips the ~64 captured pixels that are CPC HBP).
  - **60 Hz:** `set_sys_clock_khz(160000)` → exact 160 MHz. `rgb_60`
    clkdiv 1.0 → 40 MHz pixel = VESA DMT 800×600p60 exact, all 800
    captured pixels visible.
- **Capture clkdiv is runtime-tunable** for both modes (see BOOTSEL
  short-press above). The 50 Hz and 60 Hz `rgbin_*` PIO programs are
  independent — each has its own back-porch wait length, so changes
  to one don't affect the other.
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

A per-pixel "level-2 sanitiser" promoting transient `10` codes to
`11` lived here briefly but was removed — on this hardware
transients go both directions (LOW and HIGH), so an asymmetric
upward-only promotion didn't reliably help and sometimes hurt. The
detune mechanism (see *Known limitations*) is the only thing that
empirically fixes the bug.

### Horizontal sizing

CPC captures 800 active pixels per line. Each mode handles them
differently:

- **60 Hz** (DMT 800×600p60): all 800 captured pixels map 1:1 to the
  800 visible columns. Nothing dropped.
- **50 Hz** (CEA-861 720×576p50): only 720 visible columns available.
  `rgb_50` pixel clock is tuned to 27.04 MHz (CEA-861 spec) so 1
  captured pixel = 1 monitor pixel — **no horizontal squash**. 720
  of the 800 captured pixels are sent per line; `FB_X_CROP_50HZ` in
  `hardware_config.h` (default 72) chooses which 720-pixel window of
  the captured 800 is shown. The other 80 captured pixels (mostly
  CPC HBP and right-edge border) aren't sent.

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

### The colour bug ↔ jitter trade-off

This is the central limitation of this firmware on RP2040, and the
reason the BOOTSEL short-press detune cycle exists. **You can have a
perfectly stable picture with wrong colours, or correct colours with
some per-pixel jitter — but not both.** The two states are opposite
positions on the same single knob (rgbin sample clock vs CPC pixel
clock), and no firmware filtering or oversampling has been able to
break the trade-off on this hardware.

#### Root cause: comparator transition zone

The TLV3202 comparators on the VGA4CPC PCB convert the CPC's analog
RGB into a 6-bit thermometer code (2 bits per channel). The
comparator output settles ~5–10 ns after the input voltage changes —
the rest of the CPC's 62.5 ns pixel period it's stable. So each
pixel has a brief "transition zone" at its start while the
comparators are mid-flip, where the digital output is indeterminate.
**Transients go both ways** — some pull bits LOW (paper reads black),
others push bits HIGH (paper reads white-ish). Which way depends on
the previous pixel and the analog slew direction.

#### Why exact 16 MHz lock = colour bug

If the rgbin sample rate equals the CPC pixel rate **exactly**
(16 MHz), every one of the 800 samples in a line lands at the *same*
phase relative to every CPC pixel boundary. If that phase happens to
fall in the comparator's transition zone, **every captured byte is
wrong in the same way**, consistently, for the entire frame:

```
  Lock-on (16 MHz exact):     │t│t│t│t│t│t│t│   t = transition zone
                              every sample at the same bad phase
                              → uniformly wrong → "colour bug"
```

The error is fully consistent → **the picture is rock-stable, just
wrong.** Paper that should be cyan reads as black; brightness shifts
when border colour changes; etc. On the reference hardware this
happens at exact 16 MHz lock in both modes.

#### Why detune = no bug, but jitter

Pulling the sample rate slightly off 16 MHz (via PIO clkdiv) means
the sample phase **drifts** across CPC pixels — some samples land in
the transition zone, most don't. Each line, each sample lands at a
slightly different phase:

```
  Drift:                      │s│s│s│s│t│s│s│   s = stable, t = transition
                              most samples correct, occasional one
                              catches a transient
```

Statistically the correct values dominate, so the colour bug
disappears. But because the *exact* phase of each sample is now
sample-dependent, adjacent pixels and adjacent frames have slightly
different sample positions — visible as **per-pixel jitter / sparkle**
on text edges and colour boundaries.

#### Why we can't fix it cleanly on RP2040

The obvious fix is **3-sample majority vote per CPC pixel** (sample
at 48 MHz, output the median of each triplet → bug-immune AND
jitter-immune). On RP2040 this fails because:

- The PIO sample loop is 8 cycles minimum (in + push + jmp); 3
  samples per CPC pixel at 8 cycles each would need 384 MHz SM
  clock — way above silicon limits.
- 4-cycle-per-sample loops are achievable with autopush, but still
  need 192 MHz SM to fit 3× oversample in a 62.5 ns CPC pixel.
- 2-sample combining via OR/AND (which fits the budget) **can't
  recover the right value** because transients are bidirectional —
  there's no "stuck at 0" or "stuck at 1" assumption that lets us
  pick the good sample of a pair.

Pico 2 (RP2350) at 200+ MHz with a third PIO block makes this
tractable — see the *Pico 2 roadmap* section.

#### What this firmware ships

A per-mode runtime-tunable detune (BOOTSEL short press). Each mode
has its own table of 9 detune values from the "perfect lock" position
into the "useful drift" region. Boot defaults are picked from
reference-hardware empirical testing:

- **60 Hz** default: `clkdiv 1+66/256` ≈ 15.90 MHz (−0.62 %). Just
  enough detune to break lock on the reference PCB; residual jitter
  is barely perceptible.
- **50 Hz** default: `clkdiv 1+24/256` ≈ 14.66 MHz (−8.4 %). Much
  heavier detune needed because at sys=128 MHz the base clkdiv is
  near-integer and the fractional divider needs large frac values to
  produce phase scatter — 50 Hz mode therefore has visibly more
  inherent jitter than 60 Hz. The 50 Hz "sweet spot" is narrow:
  below frac ~22 the bug returns; above frac ~30 the 800-sample loop
  overruns the 64 µs CPC line and the capture drops every other
  HSYNC (half-height image).

Cycle the detune via BOOTSEL short press to find the value that works
on your specific hardware. The smaller the detune that still kills
the bug, the less jitter you'll see.

### Right-edge noise from CRTC trickery (Prehistorik 2 etc.)

Some CPC games use CRTC timing tricks that emit pixel data past
their intended visible area. We capture them verbatim → visible as
garbage on the right edge. Cycle the right-edge trim (BOOTSEL medium
press) to clamp the rightmost N captured pixels to border colour.

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
  `vsync_*.pio`, `rgb_*.pio`, `rgbin_*.pio` and `vsyncgen.pio`
  programs are derived from that project; `rgbin` was split into
  per-mode `rgbin_50` / `rgbin_60` so each can have its own
  back-porch wait and detune range.
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
