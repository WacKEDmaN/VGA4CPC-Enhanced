# VGA4CPC-Enhanced — project notes for Claude

This file gives future Claude sessions instant context. Read it first
before touching anything.

## What this is

A Raspberry Pi Pico (RP2040) firmware that turns the **grzegorz-gr/vga4cpc PCB**
into a clean, full-resolution scan-doubler for the Amstrad CPC's
RGB+CSYNC video output. From-scratch reimplementation of the upstream
firmware, using custom PIO programs on both PIO blocks instead of
pico-extras `scanvideo`.

Project owner: **WacKEDmaN** (GitHub). Code, build scripts, and docs
were all written by Claude under their direction.

Repo: <https://github.com/WacKEDmaN/VGA4CPC-Enhanced> — public domain
(Unlicense).

## Current status

**Pico 1 (RP2040) firmware is feature-complete and shipped.** A single
`dist/vga4cpc_enhanced.uf2` contains both normal and scanlines display
modes, runtime-toggleable via the Pico's BOOTSEL button (handled by a
RAM-resident QSPI-CS-read helper in `capture.c`, polled once per CPC
frame). Side-by-side comparison against upstream confirmed this build
shows visibly *less* edge fringing.

The remaining low-grade artefacts are analog (CPC gate-array
transitions, comparator settling, unbuffered R-2R DAC into VGA cable).
Not fixable in firmware on the RP2040 — see README "Known limitations".

## Next planned work: Pico 2 (RP2350) port

The PCB is pin-compatible with Pico 2. User intends to drop one in and
ask us to revisit. The README already has a "Pico 2 roadmap" section
listing what the extra resources unlock:

- **520 KB SRAM** → real double-buffered framebuffer eliminates the
  output-side dual-read race entirely and unlocks safe post-capture
  filtering (impulse / median / temporal) on Core 1.
- **Up to ~200 MHz core clock** → 12 cycles per CPC pixel @ 192 MHz,
  enabling multi-sample-per-pixel with majority voting to reject
  in-flight comparator values.
- **3rd PIO block (12 SMs)** → dedicated CSYNC-tracking / clock
  recovery SM to phase-lock our sample clock to the CPC's pixel clock,
  killing the frame-to-frame sample-phase shimmer.
- **Cortex-M33 @ 150 MHz** → general headroom.

### Concrete tasks when starting the Pico 2 port

1. Add `set(PICO_PLATFORM rp2350)` and `set(PICO_BOARD pico2)` to
   `CMakeLists.txt` (or pass via `-DPICO_PLATFORM`).
2. Verify PIO programs still assemble — RP2350 PIO has minor
   instruction-set differences (extra opcodes, may want to use them).
3. Bump `SYS_CLOCK_KHZ` in `hardware_config.h` to a value that's still
   an exact integer multiple of 16 MHz (e.g. 192000 → 12 cycles/pixel,
   or 256000 → 16 cycles/pixel). Update PIO `clkdiv` values for the
   VGA-output SMs accordingly.
4. **The real win:** turn `framebuf` into a double buffer (two banks,
   pointer swap on VSYNC). Output DMA reads from the "ready" bank;
   capture writes the "in-progress" bank. Eliminates dual-read tearing
   from impulse/temporal filtering on Core 1.
5. Add Core 1 filter pass that previously broke (commit history) due
   to the in-place race. With double buffer it's safe.
6. Optional: 2× sampling per CPC pixel in `rgbin.pio`, push both
   samples, accept-if-equal logic in C.

## Architecture summary

- **PIO0** = VGA output: hsync, vsync, rgb SMs (3 SMs). Custom programs
  in `src/hsync_50.pio`, `src/hsync_60.pio`, etc.
- **PIO1** = CPC capture: vsyncgen (CSYNC → VSYNC discriminator) and
  rgbin (per-line HSYNC-aligned 16 MHz pixel sampler).
- **Core 0** owns `capture_run_forever()` — busy-polls CSYNC/VSYNC,
  fires per-line DMA from rgbin RX FIFO into a framebuffer row, runs
  a cheap level-2 → level-3 sanitiser on the previous row.
- **Core 1** is currently idle (a Core 1 impulse-noise filter was
  prototyped but reverted — it tore with the output DMA's dual-read
  per VGA frame).
- **4-channel DMA chain** on RP2040 streams the framebuffer to PIO0's
  rgb SM, reading each captured row twice in succession for line
  doubling to 576p / 600p.
- **Framebuffer**: single 288 × 800 byte array, 1 byte per pixel in the
  DAC's native `RR GG BB` thermometer-code layout. No palette LUT.

## Important gotchas (don't forget these)

1. **`nop[30]` in `rgbin.pio`** — the post-CSYNC wait must be exactly
   448 cycles (3.5 µs @ 128 MHz). Off-by-9 cycles was the cause of
   visible fringing in early builds. **Match upstream timing
   verbatim** when porting.
2. **GPIO 10 → GPIO 7 is a hard-wired trace on the PCB** — not
   something users do. Vsyncgen drives GPIO 10, rgbin reads GPIO 7.
   On a non-vga4cpc board this connection needs to exist.
3. **No-signal detection has two timeouts**:
   - Inner 200 ms HSYNC watchdog (escapes from a mid-frame stuck
     CSYNC poll when CPC dies mid-line). Without this the inner per-
     line loop blocks forever and the outer timeout never fires.
   - Outer 3 s VSYNC timeout repaints the "NO SIGNAL" card *once*
     (latched by `test_pattern_shown`). Repainting on every check
     causes a periodic tearing flash with output DMA.
4. **Level-2 sanitiser** in `capture.c`: applies `byte |= (byte >> 1) & 0x15151515`
   per word to promote any captured `10` thermometer code (impossible
   on real CPC output, but can appear briefly at edges) to `11`.
   Cheap, ~6 µs per line, runs concurrently with the next line's DMA.
5. **`__not_in_flash_func`** is on the critical-path functions
   (`capture_run_forever`, `sanitize_line`). Keep it on anything in
   the per-line hot path.
6. **DAC pin order matches the captured bit order exactly**:
   `bit 5 = R_HI, 4 = R_LO, 3 = G_HI, 2 = G_LO, 1 = B_HI, 0 = B_LO`
   on both the input (TLV3202 outputs) and output (GPIO 14–19 → R-2R).
   This is why we can stream raw capture bytes straight to the DAC
   with no LUT.

## Build

`build.cmd` on Windows (Pico SDK 1.5.1 at default `C:\Program Files\Raspberry Pi\Pico SDK v1.5.1`).
On Linux/macOS: `cmake -G Ninja -DPICO_SDK_PATH=... -DSCANLINES=OFF .. && ninja`.

`-DSCANLINES=ON` for the CRT-scanlines variant.

## File layout

| File | Role |
|---|---|
| `src/main.c` | Tiny entry point: set clock, init fb, paint test card, init capture + VGA, enter `capture_run_forever()`. |
| `src/hardware_config.h` | All pin / SM / clock constants. **Single source of truth** for these — don't duplicate. |
| `src/capture.c` | Owns Core 0 forever. PIO setup, per-line DMA, level-2 sanitiser, no-signal logic, PWM LED. |
| `src/framebuffer.c/h` | 230 KB single-buffered framebuffer + procedural test card with embedded 8×8 font. |
| `src/vga_output.c/h` | PIO load + 4-channel DMA chain for output. |
| `src/*.pio` | 6 VGA-output programs (hsync/vsync/rgb × 50/60 Hz) + 2 capture programs (rgbin, vsyncgen). All adapted from upstream verbatim — **be very careful with timing tweaks**. |

## Git workflow

User pushes with HTTPS via Windows credential manager — no SSH
configured. Commits attributed to "WacKEDmaN <WacKEDmaN@users.noreply.github.com>"
(global git config set during initial repo creation).

Don't auto-commit; user will say "push to GitHub" when they're ready.
After every code change, do build verification (`build.cmd`) before
considering the task done.
