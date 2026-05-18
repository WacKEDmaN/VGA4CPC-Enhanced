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

CPC BASIC 1.1 prompt on an Awa 128K — the two build variants side by side:

| NORMAL | NORMAL_SCANLINES |
|:---:|:---:|
| ![NORMAL build — clean line-doubled output](images/VGA4CPC_NORMAL.png) | ![NORMAL_SCANLINES build — CRT-style dark gap between scanlines](images/VGA4CPC_SCANLINES.png) |

Running real CPC software — Batman intro logo and a mode-0 cutscene
illustrating the full colour range and borders the scan-doubler
preserves:

| | |
|:---:|:---:|
| ![Batman intro logo](images/demo_batman_logo.png) | ![Mode-0 cutscene from Batman](images/demo_batman_scene.png) |

---

## Hardware

- Raspberry Pi Pico (RP2040)
- grzegorz-gr/vga4cpc PCB (TLV3202 dual comparators + 6-bit R-2R DAC)
- 50/60 Hz slide switch on GPIO 26
- LED on GPIO 25 (status / diagnostic)

### Pin map

| GPIO  | Function                                  |
|-------|-------------------------------------------|
| 0–5   | CPC RGB input (B_LO, B_HI, G_LO, G_HI, R_LO, R_HI) |
| 6     | CSYNC input (active low)                  |
| 10    | vsyncgen output (wire to GPIO 7 on PCB)   |
| 12    | VGA HSYNC                                 |
| 13    | VGA VSYNC                                 |
| 14–19 | 6-bit R-2R DAC (R/G/B × 2 bits each)      |
| 25    | LED                                       |
| 26    | 50/60 Hz mode switch (closed/LOW = 50 Hz) |

---

## Build

Requirements:
- Raspberry Pi Pico SDK v1.5.1 (default path: `C:\Program Files\Raspberry Pi\Pico SDK v1.5.1`)
- CMake + Ninja + arm-none-eabi-gcc (all bundled with the SDK installer)

### Windows one-click

```cmd
build.cmd
```

Builds both variants into `dist\`:
- `vga4cpc_enhanced_NORMAL.uf2`
- `vga4cpc_enhanced_NORMAL_SCANLINES.uf2`

### Manual

```sh
mkdir build && cd build
cmake -G Ninja -DSCANLINES=OFF ..   # or -DSCANLINES=ON
ninja
```

The `SCANLINES=ON` variant inserts a black line between each pair of
output lines for a CRT-style scanline look.

---

## Flash & use

Prebuilt firmware images are committed to [`dist/`](dist/) — grab one
directly from there if you don't want to build from source:

- [`dist/vga4cpc_enhanced_NORMAL.uf2`](dist/vga4cpc_enhanced_NORMAL.uf2)
- [`dist/vga4cpc_enhanced_NORMAL_SCANLINES.uf2`](dist/vga4cpc_enhanced_NORMAL_SCANLINES.uf2)

1. Hold BOOTSEL while plugging the Pico into USB, then drop one of the
   `.uf2` files from `dist/` onto the `RPI-RP2` drive.
2. Plug the VGA cable into your monitor.
3. Connect the CPC's RGB cable to the PCB.
4. Pick the output mode with the slide switch on GPIO 26:
   - **closed / LOW** → 576p50 (CEA-861, ~50.08 Hz, scaled to ~800px wide)
   - **open / HIGH**  → 800×600p60 (DMT)

The LED blinks at ~1 Hz when CPC frames are being captured. Solid ON
means it's waiting for a VSYNC signal (no CPC connected or vsyncgen
not wired through).

A test pattern (8 SMPTE-style bars) is shown until the CPC starts
sending video.

---

## Architecture

```
   CPC                Pico (128 MHz)                VGA
   ───                ──────────────                ───

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
                              PIO0 SM2 (rgb) ──► GPIO 14-19 ──► 6-bit DAC
                              PIO0 SM0 (hsync) ► GPIO 12     ─┐
                              PIO0 SM1 (vsync) ► GPIO 13     ─┘
```

- **`sys_clock` = 128 MHz** — chosen so 128 ÷ 8 = 16 MHz exact CPC
  capture clock, matching the CPC pixel rate.
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
exact bit order the on-board DAC wants:

```
  bit 5 4 3 2 1 0
       R R G G B B
```

So the capture DMA writes raw 6-bit samples into the framebuffer, and
the output DMA streams those same bytes to the DAC pins. **No palette
lookup is needed.** The CPC's three native voltage levels per channel
map to DAC values 0, 1, and 3 — level 2 (half-bright) is unused by the
CPC but available to firmware-generated content (e.g. the boot test
pattern uses it for the white bar).

### Horizontal sizing

The CPC produces ~800 active pixels per line at 16 MHz (full visible
including borders). The `rgb_50` PIO sends 800 pixels into a 720-pixel
576p50 timing window — i.e. it overscans by ~11%, which makes the full
CPC display (borders included) fit a typical VGA monitor's visible
area. Monitors still detect a standard 720×576p50 signal.

### Build variants

Only two variants are shipped:

| Variant            | `SCANLINES` | Description                            |
|--------------------|-------------|----------------------------------------|
| NORMAL             | OFF         | Plain colour scan-doubled output       |
| NORMAL_SCANLINES   | ON          | CRT-style dark gap between scanlines   |

Earlier development versions experimented with monochrome / amber /
green "P1/P3 phosphor" effects, but they were abandoned because the
LUT pass through the framebuffer couldn't keep up with the per-line
capture deadline at 128 MHz.

---

## Known limitations

### Soft colour fringing at sharp colour transitions

If you zoom in on the captured image — particularly at white text on a
coloured background in mode 2 — you'll see a faint coloured halo (2–4
pixels wide) around glyph edges, and the background close to text is
not perfectly uniform.

This is a property of the **analog signal path on the PCB**, not the
firmware. The CPC's gate-array RGB output has nonzero rise/fall times
and some post-transition ringing; the TLV3202 comparators on the
VGA4CPC PCB digitise whatever they see at the sample instant,
including those in-flight values. Because the Pico's capture clock and
the CPC's pixel clock aren't phase-locked to each other, the *exact*
wrong value caught at each transition drifts slightly between frames
— producing a subtle shimmer as well.

Side-by-side testing confirms this firmware shows **noticeably less
fringing than the upstream grzegorz-gr/vga4cpc firmware** on the same
PCB, so what's here is essentially the best the unbuffered
comparator and R-2R DAC path can deliver on the original Pico
(RP2040). On a solid
colour field (e.g. `CLS`) there are no artefacts at all — the issue is
strictly transition-related.

---

## Pico 2 roadmap

The PCB is pin-compatible with the **Raspberry Pi Pico 2 (RP2350)**,
which would enable meaningful improvements:

| Pico 2 resource          | What we'd use it for                                |
|--------------------------|-----------------------------------------------------|
| 520 KB SRAM (vs 264 KB)  | True double-buffered framebuffer — eliminates any output-side tearing race and unlocks safe post-capture filtering on Core 1 |
| Up to ~200 MHz core clock | 12 cycles per CPC pixel @ 192 MHz; could sample twice per pixel with majority voting to reject in-flight comparator values |
| 3rd PIO block (12 SMs)   | Dedicated CSYNC-tracking / clock-recovery SM to phase-lock our sample clock to the CPC's pixel clock |
| Cortex-M33 @ 150 MHz     | Headroom for inline filtering / decoding on either core |

Most importantly, the **frame-to-frame shimmer** (from the unlocked
sample clock) and the **dual-read output race** (from single-buffered
output) would both be solvable. The residual *analog* fringing from
the comparators themselves would remain unless paired with a PCB-level
mod (RC filter on comparator inputs, buffer chip on the DAC outputs,
or shorter CPC cable).

No Pico 2 port work has started — this section is here so anyone
picking up the project knows where the real headroom is.

---

## Credits

- **gregg** (grzegorz-gr) — original [`vga4cpc`](https://github.com/grzegorz-gr/vga4cpc) PCB design and the
  PIO programs this firmware is built on. The `hsync_*.pio`,
  `vsync_*.pio`, `rgb_*.pio`, `rgbin.pio` and `vsyncgen.pio` programs
  are taken from that project, with only minor refactoring.
- **Hunter Adams** — the original VGA-from-PIO timing approach the
  upstream firmware credits as its basis.

---

## License

The PIO programs are derived from the upstream
[grzegorz-gr/vga4cpc](https://github.com/grzegorz-gr/vga4cpc) project
and remain under that project's license. The remaining C code in this
repository is provided as-is for hobbyist use.
