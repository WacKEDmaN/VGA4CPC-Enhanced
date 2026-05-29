# VGA4CPC-Enhanced

A direct **Amstrad CPC → VGA scan-doubler** firmware for the
[grzegorz-gr/vga4cpc](https://github.com/grzegorz-gr/vga4cpc) PCB
(Raspberry Pi Pico based).

Captures the live CPC RGB+CSYNC signal, line-doubles it, and emits
**576p50** or **800×600p60** VGA — full-resolution including
borders/overscan, with correct colour and a steady picture.

The CPC's RGB comes through TLV3202 comparators that misbehave when
sampled at exactly the CPC's 16 MHz pixel rate (the red channel reads
mid-level as black after a high-red border). This firmware sidesteps
that by sampling at **14.222 MHz** — deliberately off 16 MHz so the
effect decoheres — while keeping the sample *clock* locked to a whole
number of cycles per CPC line so the image stays steady. The captured
samples are streamed straight to the VGA output, which stretches them
across the visible width. See *How it works* below for the details.

This is a from-scratch reimplementation of the capture and output
pipeline using custom PIO programs on both PIO blocks. The VGA-output
PIO programs (`hsync_50/60`, `vsync_50/60`, `rgb_50/60`) and the
capture PIOs (`rgbin_50`/`rgbin_60`, `vsyncgen`) are derived from the
upstream grzegorz-gr/vga4cpc firmware — full credit to **gregg** for
the original timings and PIO designs. `rgbin` was split into two
per-mode programs so each can have its own back-porch timing.

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
   - **closed / LOW** → 576p50 (CEA-861 720×576p50)
   - **open / HIGH**  → 800×600p60 (VESA DMT)

   The firmware notices the switch position changing at runtime and
   reboots itself cleanly to apply the new mode (~250 ms blank, then
   the new mode comes up). No manual reset needed.

### BOOTSEL button — runtime settings

The BOOTSEL button on the Pico drives two settings, distinguished by
hold duration:

#### Short press (< 1.5 s) — scanlines toggle

Toggles the CRT-style scanlines effect on or off. With scanlines on,
every 2nd output row is replaced by a fully black line, giving the
classic 50%-density CRT look.

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
Cycles through four trim levels:

| Trim level | Pixels trimmed | When to use                              |
|:----------:|:--------------:|:-----------------------------------------|
| 0 (default) | 0  (off)      | No trim — full capture shown             |
| 1          | 32             | Mild garbage on the right edge           |
| 2          | 64             | More noticeable garbage (most games)     |
| 3          | 96             | Heavy CRTC-trickery games (Prehistorik 2) |

#### Persistence

Both settings are **persisted to the last 4 KB sector of the Pico's
flash**, so they survive power cycles — set your preferences once and
the firmware boots into the same state next time. First-ever boot
(blank persistence sector) starts with scanlines off and trim off.

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

- **`sys_clock` is per-mode**, both PLL-locked to exact values:
  - **50 Hz:** `set_sys_clock_khz(128000)` → exact 128 MHz. `rgb_50`
    clkdiv 1+47/256 ≈ 27.04 MHz pixel for CEA-861 720×576p50.
  - **60 Hz:** `set_sys_clock_khz(160000)` → exact 160 MHz. `rgb_60`
    clkdiv 1.0 → 40 MHz pixel = VESA DMT 800×600p60 exact.
- **Capture runs at 14.222 MHz**, not 16 MHz. The rgbin SM clock is
  128 MHz (60 Hz: clkdiv 1.25 from 160 MHz; 50 Hz: clkdiv 1.0 from
  128 MHz) and the read loop is 9 SM cycles per sample → 128/9 =
  14.222 MHz. This is deliberately off the CPC's 16 MHz pixel clock so
  the comparator colour bug decoheres (see *How it works*). 128 MHz is
  exactly 8192 cycles per 64 µs CPC line, so the sample phase is
  line-locked and the image is steady. Both `rgbin_*` programs are
  independent so each can carry its own back-porch timing.
- **PIO1** runs the capture side: `vsyncgen` discriminates CSYNC into a
  VSYNC level on GPIO 10 (wired back to GPIO 7 on the PCB);
  `rgbin` is a per-line, HSYNC-aligned sampler that pushes one
  byte per sample to its RX FIFO. 720 samples per line (`CAP_VISIBLE_W`)
  span the full active line and are streamed straight to the output.
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

### Horizontal sizing

The rgbin SM captures **720 samples per line** (`CAP_VISIBLE_W`) at
14.222 MHz, spanning the ~50 µs active line. Those 720 samples are sent
straight to the VGA output, which stretches them across the visible
width via its pixel clock — no software resampling:

- **50 Hz** (CEA-861 720×576p50): `rgb_50` runs at 27 MHz, so 720
  samples fill the 720-pixel HACT exactly, 1:1.
- **60 Hz** (DMT 800×600p60): `rgb_60` runs at 40 MHz, so 720 samples
  fill 720 of the 800-pixel DMT window — leaving a small (~80 px) black
  border on the right. (Cosmetic; a future revision can widen this.)

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

## How it works — beating the comparator colour bug

The TLV3202 comparators on the VGA4CPC PCB convert the CPC's analog
RGB into a 6-bit thermometer code (2 bits per channel). On the
reference hardware the **red channel** misbehaves in a very specific
way: after a sustained **high-red border**, the red comparator's
mid-level output reads as **black** (R_LO drops) for the rest of the
line — so mid-red paper that follows a bright-red border turns black.
Green and blue are unaffected.

The decisive observation: the bug appears **only when the sample rate
equals the CPC's 16 MHz pixel rate exactly**, at *any* sample phase.
Shift the sample point within the pixel — bug stays. Change the sample
*rate* even slightly off 16 MHz — bug vanishes, solidly. That rules out
a sub-pixel timing issue: it's a **rate-coherence** effect (sampling
synchronously with the CPC's pixel clock reinforces the comparator
glitch; sampling at an incommensurate rate averages it out).

So this firmware samples at **14.222 MHz** instead of 16 MHz:

- The rgbin SM clock is **128 MHz**; the read loop is **9 SM cycles per
  sample** → 128 / 9 = 14.222 MHz. Off 16 MHz → the colour bug
  decoheres → correct red.
- Crucially the SM *clock* stays 128 MHz, which is **exactly 8192
  cycles per 64 µs CPC line**. Because that's a whole number, the
  sample phase relative to CSYNC is the same every line — so the image
  is **steady**, not the drifting/jittering mess that changing the
  clock *divider* (the old "detune") produced.
- An **8 µs back porch** delays the start of sampling past the red
  comparator's recovery transient after the border.
- **720 samples** span the active line and are streamed straight to the
  output (no resample, no per-line CPU cost); the output pixel clock
  stretches them to the visible width.

This replaced an earlier "detune" approach that changed the clock
*divider* to dodge the bug — that worked for colour but the non-integer
cycles-per-line made the whole image jitter. Moving the rate change
into the sample *loop length* (keeping the clock integer-per-line) is
what gives correct colour **and** a steady picture.

---

## Known limitations

### Faint whole-line drift

A gentle, slow horizontal sway of the whole image remains. The CPC's
clock and the Pico's clock are independent crystals running at slightly
different rates (the CPC is PAL ~50.08 Hz, not exactly 50), so they
beat against each other and the CSYNC-to-sample-clock alignment creeps.
It's far less objectionable than the old per-pixel sparkle, but it's
there. Eliminating it needs true clock recovery — phase-locking the
sample clock to the CPC — which is being explored on a branch and is a
natural fit for the Pico 2 (see *Pico 2 roadmap*).

### 60 Hz right-edge border

720 captured samples fill 720 of the 800-pixel DMT window, leaving a
small (~80 px) black border on the right in 60 Hz mode. Purely
cosmetic; a future revision can widen the output to fill it.

### Right-edge noise from CRTC trickery (Prehistorik 2 etc.)

Some CPC games use CRTC timing tricks that emit pixel data past their
intended visible area. We capture them verbatim → visible as garbage on
the right edge. Use the right-edge trim (BOOTSEL long press) to clamp
the rightmost N captured pixels to border colour.

---

## Optional hardware mods

The colour bug is fixed in firmware (see *How it works*), so no
hardware mod is needed for it. The buffer mod below is still worth
considering for general signal cleanliness: it isolates the Pico's
3.3 V rail from the summing-network's switching current, reducing
analog noise on the input side. Optional.

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
resistors.

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

The PCB is pin-compatible with the **Raspberry Pi Pico 2 (RP2350)**.
The colour bug is already solved on RP2040; the main thing the Pico 2
unlocks is killing the **residual whole-line drift** properly:

| Pico 2 resource           | What it unlocks                                     |
|---------------------------|-----------------------------------------------------|
| 3rd PIO block (12 SMs)    | A dedicated **CSYNC clock-recovery SM** that phase-locks the sample clock to the CPC's pixel clock — the clean fix for the residual drift, with PIO room to spare (RP2040's PIO1 is nearly full). |
| Cortex-M33 @ 150+ MHz × 2 | Headroom for a hardware/software PLL on the capture clock, and for per-line post-processing (e.g. multi-sample averaging) within the 64 µs line budget. |
| 520 KB SRAM (vs 264 KB)   | Double-buffered framebuffer eliminates the output-side tearing race and enables safe post-capture filtering on Core 1. |
| Higher core clock         | More sample-clock resolution for finer phase control. |

A first cut of the clock-recovery loop exists on the
`csync-clock-recovery` branch (a software frequency-locked loop on the
RP2040). The Pico 2's spare PIO block is the natural home for doing it
properly in hardware.

---

## Credits

- **gregg** (grzegorz-gr) — original [`vga4cpc`](https://github.com/grzegorz-gr/vga4cpc) PCB design and the
  PIO programs this firmware is built on. The `hsync_*.pio`,
  `vsync_*.pio`, `rgb_*.pio`, `rgbin_*.pio` and `vsyncgen.pio`
  programs are derived from that project; `rgbin` was split into
  per-mode `rgbin_50` / `rgbin_60` so each can have its own
  back-porch timing.
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
