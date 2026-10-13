// CPU load generator: one busy task pinned to each core of the ESP32-P4.
//
// The tasks run at priority 1, above the idle tasks but below the services
// graceloader and the BSP bring up. Every busy chunk is followed by a one-tick
// delay: the coprocessor task that delivers keyboard events runs at priority 0
// and would otherwise never get the CPU, which would make the app impossible
// to switch off.
//
// Chunk size is controlled by a throughput estimate that is measured from the
// chunks that actually ran, never assumed. burn_seed makes each chunk depend
// on the previous one: without it the compiler folds the whole busy loop away
// (it is a pure function of a constant argument) and the measurement becomes
// meaningless.
#include <inttypes.h>
#include <string.h>
#include "burn.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static char const TAG[] = "burn";

#define BURN_TASK_STACK    4096
#define BURN_TASK_PRIORITY 1
#define BURN_MIN_LOAD      10
// Not 100 %: every burn cycle has to leave a tick for the priority-0 tasks.
#define BURN_MAX_LOAD      95
#define BURN_DEFAULT_LOAD  90
// One tick: the window the priority-0 tasks get at the end of every chunk.
#define BURN_YIELD_US      (1000000 / CONFIG_FREERTOS_HZ)
// Four independent multiply-add chains: enough to keep the FPU busy even
// though a single chain has a latency of several cycles.
#define BURN_CHAINS         4
// Chunks shorter than this are too noisy to calibrate against.
#define BURN_MIN_MEASURE_US 1000
#define BURN_MIN_ITERATIONS 512
// Shorter sample windows are dominated by scheduling noise and make the
// reported load and throughput jump around.
#define BURN_MIN_SAMPLE_US  200000

typedef struct {
    volatile uint32_t ops;      // operations completed since the task started
    volatile uint32_t busy_us;  // measured busy time accumulated
    volatile uint32_t checksum; // keeps the accumulators alive
    uint32_t          core;
} burn_core_t;

static burn_core_t cores[portNUM_PROCESSORS];
static uint32_t    core_count = 0;

static volatile bool     burn_enabled = false;
static volatile uint32_t burn_load = BURN_DEFAULT_LOAD;
static volatile uint32_t burn_busy_us = 0;      // target busy time per chunk
static volatile uint32_t burn_iterations = 0;   // iterations per chunk
static volatile float    burn_iters_per_us = 0.0f;
static volatile uint32_t burn_seed = 1;

// Counters the previous sample was taken against
static uint32_t last_ops[portNUM_PROCESSORS];
static uint32_t last_busy_us[portNUM_PROCESSORS];
static burn_core_stat_t last_stats[portNUM_PROCESSORS];
static uint32_t last_total_ops = 0;
static int64_t  last_sample_us = 0;

// One busy chunk: BURN_CHAINS independent chains of fused multiply-adds.
static uint32_t burn_work(uint32_t iterations, uint32_t seed) {
    float       a = (float)seed;
    float       b = (float)seed + 1.0f;
    float       c = (float)seed + 2.0f;
    float       d = (float)seed + 3.0f;
    float const mul = 1.00000011920928955078125f;

    for (uint32_t i = 0; i < iterations; i++) {
        a = a * mul + 0.5f;
        b = b * mul + 0.25f;
        c = c * mul + 0.125f;
        d = d * mul + 0.0625f;
    }

    return (uint32_t)a ^ (uint32_t)b ^ (uint32_t)c ^ (uint32_t)d;
}

// Size the next chunk from the measured throughput and the target busy time.
static void burn_recompute(void) {
    float const per_us = burn_iters_per_us;
    if (per_us <= 0.0f) {
        return;
    }

    uint32_t iterations = (uint32_t)(per_us * (float)burn_busy_us);
    if (iterations < BURN_MIN_ITERATIONS) {
        iterations = BURN_MIN_ITERATIONS;
    }
    burn_iterations = iterations;
}

// Fold one measured chunk into the throughput estimate.
static void burn_learn(uint32_t iterations, int64_t spent_us) {
    if (spent_us < BURN_MIN_MEASURE_US) {
        return;
    }

    float const measured = (float)iterations / (float)spent_us;
    float const current = burn_iters_per_us;
    if (current <= 0.0f) {
        burn_iters_per_us = measured;
    } else {
        burn_iters_per_us = current * 0.8f + measured * 0.2f;
    }
    burn_recompute();
}

static void burn_apply_load(uint32_t percent) {
    if (percent < BURN_MIN_LOAD) {
        percent = BURN_MIN_LOAD;
    }
    if (percent > BURN_MAX_LOAD) {
        percent = BURN_MAX_LOAD;
    }

    // busy / (busy + yield) == percent / 100
    burn_load = percent;
    burn_busy_us = (uint32_t)((uint64_t)BURN_YIELD_US * percent / (100 - percent));
    burn_recompute();
}

// Measure the throughput once so the first chunks are in the right ballpark.
static void burn_estimate(void) {
    uint32_t const iterations = 4096;
    uint32_t const chunks     = 100;

    uint32_t checksum = burn_seed;
    int64_t  start    = esp_timer_get_time();
    for (uint32_t i = 0; i < chunks; i++) {
        checksum = burn_work(iterations, checksum);
    }
    int64_t const elapsed = esp_timer_get_time() - start;

    burn_seed         = checksum;
    cores[0].checksum = checksum;

    if (elapsed >= BURN_MIN_MEASURE_US) {
        burn_iters_per_us = (float)(iterations * chunks) / (float)elapsed;
    } else {
        burn_iters_per_us = 10.0f;
    }

    ESP_LOGI(TAG, "Throughput estimate %.1f iterations/us", (double)burn_iters_per_us);
}

static void burn_task(void* arg) {
    burn_core_t* self = (burn_core_t*)arg;

    while (1) {
        if (!burn_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t const iterations = burn_iterations;
        uint32_t const seed       = burn_seed;

        int64_t const start = esp_timer_get_time();
        uint32_t checksum = burn_work(iterations, seed);
        int64_t spent = esp_timer_get_time() - start;
        if (spent < 1) {
            spent = 1;
        }

        burn_seed = checksum;
        self->checksum = checksum;
        self->busy_us += (uint32_t)spent;
        self->ops += iterations * BURN_CHAINS;
        burn_learn(iterations, spent);

        // Let the coprocessor keyboard task and the idle tasks (priority 0)
        // have the core for a tick. Without this the keyboard stops working
        // and the load cannot be switched off any more.
        vTaskDelay(1);
    }
}

void burn_start(void) {
    core_count = portNUM_PROCESSORS;
    burn_apply_load(burn_load);
    burn_estimate();
    burn_recompute();

    for (uint32_t i = 0; i < core_count; i++) {
        cores[i].core = i;
        if (xTaskCreatePinnedToCore(burn_task, "cpuburn", BURN_TASK_STACK, &cores[i], BURN_TASK_PRIORITY, NULL, i) !=
            pdTRUE) {
            ESP_LOGE(TAG, "Failed to create the burn task for core %" PRIu32, i);
        }
    }

    ESP_LOGI(TAG, "Target %" PRIu32 " %%: %" PRIu32 " us busy + %" PRIu32 " us yield, %" PRIu32 " iterations per chunk",
             burn_load, burn_busy_us, (uint32_t)BURN_YIELD_US, burn_iterations);
}

void burn_set_enabled(bool enabled) {
    burn_enabled = enabled;
    ESP_LOGI(TAG, "CPU load %s", enabled ? "started" : "stopped");
}

bool burn_is_enabled(void) {
    return burn_enabled;
}

void burn_set_load_percent(uint32_t percent) {
    burn_apply_load(percent);
}

uint32_t burn_get_load_percent(void) {
    return burn_load;
}

void burn_adjust_load_percent(int32_t step) {
    int32_t percent = (int32_t)burn_load + step;
    if (percent < BURN_MIN_LOAD) {
        percent = BURN_MIN_LOAD;
    }
    if (percent > BURN_MAX_LOAD) {
        percent = BURN_MAX_LOAD;
    }
    burn_apply_load((uint32_t)percent);
}

uint32_t burn_core_count(void) {
    return core_count;
}

uint32_t burn_sample(burn_core_stat_t* out_stats, uint32_t core_count_requested) {
    int64_t now = esp_timer_get_time();
    int64_t elapsed_us = now - last_sample_us;
    uint32_t const count = core_count < core_count_requested ? core_count : core_count_requested;

    if (last_sample_us == 0) {
        // The first sample only establishes the baseline counters.
        last_sample_us = now;
        for (uint32_t i = 0; i < core_count; i++) {
            last_ops[i] = cores[i].ops;
            last_busy_us[i] = cores[i].busy_us;
        }
        for (uint32_t i = 0; i < count; i++) {
            last_stats[i] = (burn_core_stat_t){.core = i, .load_percent = 0, .ops_per_s = 0};
        }
        last_total_ops = 0;
    } else if (elapsed_us >= BURN_MIN_SAMPLE_US) {
        last_sample_us = now;
        uint32_t total = 0;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t const ops = cores[i].ops;
            uint32_t const busy_us = cores[i].busy_us;
            uint32_t const ops_delta = ops - last_ops[i];  // wrap safe
            uint32_t const busy_delta = busy_us - last_busy_us[i];
            last_ops[i] = ops;
            last_busy_us[i] = busy_us;

            burn_core_stat_t stat = {
                .core = i,
                .ops_per_s = (uint32_t)((uint64_t)ops_delta * 1000000 / (uint64_t)elapsed_us),
                .load_percent = (uint32_t)((uint64_t)busy_delta * 100 / (uint64_t)elapsed_us),
            };
            total += stat.ops_per_s;
            last_stats[i] = stat;
        }
        last_total_ops = total;
    }

    if (out_stats != NULL) {
        memcpy(out_stats, last_stats, count * sizeof(burn_core_stat_t));
    }
    return last_total_ops;
}
