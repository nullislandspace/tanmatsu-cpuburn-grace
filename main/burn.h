// CPU load generator: one busy task pinned to each core of the ESP32-P4.
//
// The tasks run at priority 1, above the idle tasks but below the services
// graceloader and the BSP bring up. Every busy chunk is followed by a one-tick
// delay: the coprocessor task that delivers keyboard events runs at priority 0
// and would otherwise never get the CPU, which would make the app impossible
// to switch off.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t core;          // the core this measurement belongs to
    uint32_t load_percent;  // busy share of the measured interval
    uint32_t ops_per_s;     // floating point operations per second
} burn_core_stat_t;

// Calibrate the busy loop and create the tasks. They stay idle until
// burn_set_enabled(true). Call after BSP initialization.
void burn_start(void);

void burn_set_enabled(bool enabled);
bool burn_is_enabled(void);

// Target busy share of a burn cycle: busy / (busy + yield). Clamped to a range
// that still leaves a full tick for the priority-0 tasks. This is what the
// generator aims for; burn_sample() reports what it actually achieved.
void burn_set_load_percent(uint32_t percent);
uint32_t burn_get_load_percent(void);
void burn_adjust_load_percent(int32_t step);

// Number of burn tasks, one per core.
uint32_t burn_core_count(void);

// Measurements for the interval since the previous call: measured busy time as
// a percentage of wall time, and measured throughput. Intervals shorter than a
// few hundred milliseconds are noise, so they reuse the previous measurement.
// Returns the total throughput over all cores.
uint32_t burn_sample(burn_core_stat_t* out_stats, uint32_t core_count);
