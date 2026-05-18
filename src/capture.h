#pragma once
#include <stdbool.h>
#include <stdint.h>

// Initialise PIO programs, claim a DMA channel, register IRQ handlers.
// Call from Core 0 before launching Core 1.
void capture_init(void);

// Arm DMA and enable both PIO state machines simultaneously.
// Call from Core 0 after capture_init().
void capture_start(void);

// Stop both PIO state machines and abort the capture DMA channel.
void capture_stop(void);

// Returns true if a CPC HSYNC was observed within the last 2 ms.
bool capture_signal_present(void);

// Owns the calling core forever. Polls CSYNC/VSYNC and stages one DMA
// per CPC line into the framebuffer. Call AFTER capture_init().
void capture_run_forever(void);
