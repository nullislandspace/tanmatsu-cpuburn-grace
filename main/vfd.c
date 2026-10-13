// =====================================================================
//  cpuburn  --  optional external VFD status display (see vfd.h)
// ---------------------------------------------------------------------
//  Ported from ../tanmatsu-synthminer-grace/main/vfd.c, which is itself
//  lifted from ../tanmatsu-synthracer-grace/main/vfd.c. The probe, the bus
//  sharing and the shutdown are unchanged; what changed is what the tube
//  shows. SynthMiner publishes a translated item name, which needs a text
//  buffer, a fold through the i18n table and scrolling for names wider than
//  the tube. This app has one fixed pattern and two states, so all of that
//  is gone and only the static write is left.
//
//  NE-HCS12SS59T-R1 register map (8-bit registers):
//    0       system control (bit 0 = enable, bit 1 = test, bit 2 = LED)
//    1       display offset into the text buffer
//    2       scroll length (maximum offset while auto-scrolling)
//    3       scroll mode (bits 3-0: 0 off / 1 left / 2 right; bit 4 loop)
//    4-5     scroll speed, ms per step (little-endian 16 bit)
//    6       filament current (default 110)
//    10-255  ASCII text buffer
// =====================================================================

#include "vfd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/catt.h"
#include "bsp/i2c.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static char const TAG[] = "vfd";

#define VFD_ADDRESS     0x13    // 7-bit address (default 0x10 + jumpers 1 and 2)
#define VFD_SPEED_HZ    100000
#define VFD_CHARS       12      // visible characters
#define VFD_TIMEOUT_MS  50      // per-transfer timeout
#define VFD_PROBE_MS    20      // per-bus probe timeout

#define VFD_REG_CONTROL      0
#define VFD_REG_OFFSET       1
#define VFD_REG_SCROLL_MODE  3
#define VFD_REG_BRIGHTNESS   6
#define VFD_REG_TEXT         10

#define VFD_CTRL_ENABLE      (1 << 0)

// Filament current. DON'T GO MUCH HIGHER: it drastically shortens the
// tube's life (same value as the launcher, the vfdclock app and the device
// default). The pattern below already lights every character as hard as the
// font allows; the current is not the place to push this tube any further.
#define VFD_BRIGHTNESS       110

// The busiest glyph the module's font has. The tube is a dot matrix and its
// characters come from a five-by-seven ASCII font, so the count is the lit
// dots of that grid: '8' has 23 of its 35 dots lit, against 21 for '&', 20
// for '#', '@' and 'B', and 18 for '0'. Twelve of them is the most this
// display can be asked to light through its text buffer.
#define VFD_PATTERN          "888888888888"

#define VFD_POLL_MS          50  // task wake-up interval
#define VFD_TASK_STACK_BYTES (6 * 1024)  // ESP-IDF stack sizes are in bytes
// Same priority as the burn tasks (burn.c), on purpose: the tube is the only
// thing here with no deadline at all, so it should never take a slice from
// the load generator -- it runs in the yield window every burn cycle leaves.
#define VFD_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)
#define VFD_TASK_CORE        1

typedef struct {
    char const*             name;
    i2c_master_bus_handle_t handle;
    bool                    shared;  // internal bus: shared with the coprocessor, needs claim/release
} vfd_bus_t;

static vfd_bus_t s_buses[2] = {
    {.name = "CATT I2C", .shared = false},
    {.name = "Internal I2C", .shared = true},
};

static vfd_bus_t*              s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static volatile bool     s_want         = false;  // what F3 asked for; the tube starts off
static volatile bool     s_shown        = false;  // what the tube is actually showing
static volatile bool     s_present      = false;
static volatile bool     s_exit_request = false;
static volatile bool     s_task_done    = true;  // true until the task runs, and once it has finished

static void bus_lock(vfd_bus_t const* bus) {
    if (bus->shared) bsp_i2c_primary_bus_claim();
}

static void bus_unlock(vfd_bus_t const* bus) {
    if (bus->shared) bsp_i2c_primary_bus_release();
}

static esp_err_t vfd_write(uint8_t const* data, size_t len) {
    bus_lock(s_bus);
    esp_err_t const res = i2c_master_transmit(s_dev, data, len, VFD_TIMEOUT_MS);
    bus_unlock(s_bus);
    return res;
}

static esp_err_t vfd_write_reg(uint8_t reg, uint8_t value) {
    uint8_t const data[2] = {reg, value};
    return vfd_write(data, sizeof(data));
}

// Write `text` at the start of the text buffer, space-padded to `len`
// characters. Padding is what erases the previous, longer word.
static esp_err_t vfd_write_text(char const* text, size_t len) {
    uint8_t buf[1 + VFD_CHARS];
    size_t  n = strlen(text);
    if (n > len) n = len;

    buf[0] = VFD_REG_TEXT;
    memcpy(&buf[1], text, n);
    memset(&buf[1 + n], ' ', len - n);
    return vfd_write(buf, 1 + len);
}

// Put the tube into one of the two states this app knows.
static esp_err_t vfd_apply(bool enabled) {
    // Stop scrolling and rewind first, so a display left scrolling by another
    // app shows the pattern from its first character rather than mid-marquee.
    esp_err_t res = vfd_write_reg(VFD_REG_SCROLL_MODE, 0);
    if (res == ESP_OK) res = vfd_write_reg(VFD_REG_OFFSET, 0);

    if (enabled) {
        if (res == ESP_OK) res = vfd_write_reg(VFD_REG_BRIGHTNESS, VFD_BRIGHTNESS);
        if (res == ESP_OK) res = vfd_write_text(VFD_PATTERN, VFD_CHARS);
        if (res == ESP_OK) res = vfd_write_reg(VFD_REG_CONTROL, VFD_CTRL_ENABLE);
    } else {
        if (res == ESP_OK) res = vfd_write_text("", VFD_CHARS);
        if (res == ESP_OK) res = vfd_write_reg(VFD_REG_CONTROL, 0);
    }
    return res;
}

// Look for the VFD on the CATT bus, then the internal bus, and attach to
// the first bus it answers on.
static bool vfd_probe(void) {
    // CATT bus: bsp_device_initialize() leaves it down if an add-on held the lines, so retry once
    if (bsp_catt_i2c_bus_get_handle(&s_buses[0].handle) != ESP_OK) {
        s_buses[0].handle = NULL;
        if (bsp_catt_set_i2c_enabled(true) != ESP_OK ||
            bsp_catt_i2c_bus_get_handle(&s_buses[0].handle) != ESP_OK) {
            s_buses[0].handle = NULL;
        }
    }
    if (bsp_i2c_primary_bus_get_handle(&s_buses[1].handle) != ESP_OK) {
        s_buses[1].handle = NULL;
    }

    for (size_t i = 0; i < sizeof(s_buses) / sizeof(s_buses[0]); i++) {
        vfd_bus_t* const bus = &s_buses[i];
        if (bus->handle == NULL) continue;
        bus_lock(bus);
        esp_err_t const res = i2c_master_probe(bus->handle, VFD_ADDRESS, VFD_PROBE_MS);
        bus_unlock(bus);
        if (res == ESP_OK) {
            s_bus = bus;
            break;
        }
    }
    if (s_bus == NULL) {
        ESP_LOGI(TAG, "no VFD at 0x%02X", VFD_ADDRESS);
        return false;
    }

    i2c_device_config_t const dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = VFD_ADDRESS,
        .scl_speed_hz    = VFD_SPEED_HZ,
    };
    esp_err_t const res = i2c_master_bus_add_device(s_bus->handle, &dev_cfg, &s_dev);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "failed to add VFD device: %s", esp_err_to_name(res));
        s_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "VFD at 0x%02X on %s", VFD_ADDRESS, s_bus->name);
    return true;
}

static void vfd_task(void* arg) {
    (void)arg;

    if (vfd_probe()) {
        s_present = true;

        // Another app may have left the display on or scrolling; this one
        // starts dark, whatever the previous one did.
        vfd_apply(false);

        bool write_ok = true;

        while (!s_exit_request) {
            // THE TUBE FOLLOWS F3, one write per change: a tube that already
            // shows what was asked for is left alone, and a write that did not
            // make it is retried on the next poll because `s_shown` never took
            // it.
            if (s_want != s_shown) {
                esp_err_t const res = vfd_apply(s_want);
                if (res == ESP_OK) {
                    s_shown = s_want;
                }

                if ((res == ESP_OK) != write_ok) {
                    write_ok = (res == ESP_OK);
                    if (write_ok) ESP_LOGI(TAG, "VFD writes recovered");
                    else          ESP_LOGW(TAG, "VFD write failed: %s", esp_err_to_name(res));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(VFD_POLL_MS));
        }

        // Leave the tube dark: blank the text, stop scrolling, switch off.
        vfd_apply(false);
        s_shown = false;
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    s_task_done = true;
    vTaskDelete(NULL);
}

void vfd_init(void) {
    s_want         = false;
    s_shown        = false;
    s_present      = false;
    s_exit_request = false;
    s_task_done    = false;
    BaseType_t const ok = xTaskCreatePinnedToCore(vfd_task, "vfd", VFD_TASK_STACK_BYTES, NULL,
                                                  VFD_TASK_PRIORITY, NULL, VFD_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create VFD task");
        s_task_done = true;
    }
}

void vfd_set_enabled(bool enabled) {
    s_want = enabled;
}

bool vfd_is_enabled(void) {
    return s_want;
}

bool vfd_is_present(void) {
    return s_present;
}

void vfd_shutdown(void) {
    s_exit_request = true;
    // The task finishes its current write (at most a few bus timeouts),
    // then blanks and powers the display down. Bound the wait so a hung bus
    // can never keep the app from exiting.
    for (int i = 0; i < 100 && !s_task_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
