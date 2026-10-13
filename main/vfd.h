#pragma once
// =====================================================================
//  cpuburn  --  optional external VFD, driven to its busiest pattern
// ---------------------------------------------------------------------
//  Some Tanmatsu units carry a NE-HCS12SS59T-R1 I2C VFD (12-character
//  ASCII) jumpered to address 0x13 (the default 0x10 collides with an
//  on-board device on the internal bus). It is looked for on the CATT
//  port I2C bus first, then on the internal I2C bus -- the same probe as
//  the launcher's vfd.c and SynthMiner's VFD, which this is ported from.
//
//  WHAT IT DOES here: F3 toggles the tube between switched off -- how it
//  is left at startup, and how the app leaves it -- and switched on with
//  all twelve characters showing the busiest glyph the module's font has.
//  This is a load diagnostic, and a tube lit to the max is the
//  display-side counterpart of loading both cores: it is the pattern that
//  draws the most filament and grid current, so the tube can be part of
//  what the power readings on screen are measuring.
//
//  All I2C traffic runs in a small background task, so the frame loop
//  never waits on the bus: the app only publishes the state it wants and
//  the task writes the display when that changes. Without a VFD the task
//  exits after the probe and every call here is a cheap no-op.
// =====================================================================

#include <stdbool.h>

// Probe both buses and start the task, leaving the tube switched off. Call
// once from app_main after BSP initialization. Non-blocking.
void vfd_init(void);

// Publish the state the tube should be in: on, all characters at the
// busiest glyph, or off. Cheap; safe from the input loop.
void vfd_set_enabled(bool enabled);

// What F3 was last asked for. Reported on screen whether or not a tube was
// found, so a missing display is visible instead of looking like a dead key.
bool vfd_is_enabled(void);

// Whether the probe found a display at all. False until the probe has run.
bool vfd_is_present(void);

// Blank and switch off the display, then stop the task. Blocks until the task
// is done (bounded), so call it right before leaving for the launcher. A
// no-op when no VFD was found.
void vfd_shutdown(void);
