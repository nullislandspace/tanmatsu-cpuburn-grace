// cpuburn: load both cores of the ESP32-P4 and watch what that does to the
// power system and to the temperatures. F6 starts and stops the load, F4/F5
// set how much of every cycle is busy, F3 toggles an optional external VFD,
// F1/ESC goes back to the launcher.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "burn.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_clk.h"
#include "esp_log.h"
#include "esp_system.h"
#include "gl_input.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "power_info.h"
#include "portmacro.h"
#include "vfd.h"

// Constants
static char const TAG[] = "main";

#define TEXT_FONT  pax_font_sky_mono
#define TEXT_SIZE  16
#define LINE_PITCH (TEXT_SIZE + 2)
#define MARGIN_X   12
#define HEADER_H   34
#define FOOTER_H   30
#define CONTENT_Y  (HEADER_H + 10)
// Sky Mono glyphs are 7x9 pixels at their nominal size of 9, so the advance of
// every glyph is this wide. The columns are laid out with that number.
#define CHAR_W            (TEXT_SIZE * 7.0f / 9.0f)
// Longest line of the right column, in characters.
#define RIGHT_COLUMN_CHARS 33
// Longest line of the left column, in characters.
#define LEFT_COLUMN_CHARS  26

#define COL_BACKGROUND 0xFFFFFFFF
#define COL_FOREGROUND 0xFF000000
#define COL_BAR        0xFF202020
#define COL_BAR_TEXT   0xFFFFFFFF
#define COL_WARN       0xFFB00000
#define COL_DIVIDER    0xFFC8C8C8

// Global variables
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB;
static bsp_display_endianness_t   display_data_endian  = BSP_DISPLAY_ENDIAN_LITTLE;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;
static esp_chip_info_t            chip                 = {0};

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// Lines that did not fit on screen, for the current frame. Silently dropping a
// reading is worse than admitting it, so these get reported in the footer.
static unsigned dropped_line_count = 0;
static unsigned clipped_line_count = 0;

// Draw one line of a column, returning the index of the next line. Lines that
// would run into the footer are dropped, lines that would run off the right
// edge are truncated with a trailing '~'.
static int draw_line(float x, int line, pax_col_t color, char const* text) {
    float const bottom  = (float)pax_buf_get_height(&fb) - FOOTER_H;
    float const limit_x = (float)pax_buf_get_width(&fb) - MARGIN_X;
    float const y       = CONTENT_Y + LINE_PITCH * line;

    if (y + TEXT_SIZE > bottom) {
        dropped_line_count++;
        return line + 1;
    }

    size_t const len = strlen(text);
    if (x + (float)len * CHAR_W > limit_x) {
        clipped_line_count++;
        char clipped[128];
        size_t const keep = (size_t)((limit_x - x) / CHAR_W);
        if (keep < 2) {
            clipped[0] = '\0';
        } else {
            snprintf(clipped, sizeof(clipped), "%.*s~", (int)(keep - 1), text);
        }
        pax_draw_text(&fb, color, TEXT_FONT, TEXT_SIZE, x, y, clipped);
    } else {
        pax_draw_text(&fb, color, TEXT_FONT, TEXT_SIZE, x, y, text);
    }

    return line + 1;
}

static void render(void) {
    power_info_t power;
    power_info_collect(&power);

    burn_core_stat_t stats[portNUM_PROCESSORS];
    uint32_t   core_count = burn_core_count();
    uint32_t   total_ops = burn_sample(stats, core_count);
    uint32_t   total_load = 0;
    for (uint32_t i = 0; i < core_count; i++) {
        total_load += stats[i].load_percent;
    }
    if (core_count > 0) {
        total_load /= core_count;
    }

    char text[128];

    dropped_line_count = 0;
    clipped_line_count = 0;

    int const screen_w = pax_buf_get_width(&fb);
    int const screen_h = pax_buf_get_height(&fb);
    // The panel is 480x800 and the BSP rotates it, so what is left after
    // pax_buf_get_width/height is 800x480. Two columns fit that, one does not.
    float const left_x    = MARGIN_X;
    float const divider_x = left_x + (float)LEFT_COLUMN_CHARS * CHAR_W + 6.0f;
    float       right_x   = (float)(screen_w - MARGIN_X) - (float)RIGHT_COLUMN_CHARS * CHAR_W;
    if (right_x < divider_x + 12.0f) {
        right_x = divider_x + 12.0f;
    }
    float const footer_y = (float)(screen_h - FOOTER_H + 7);

    pax_background(&fb, COL_BACKGROUND);
    pax_simple_rect(&fb, COL_BAR, 0, 0, screen_w, HEADER_H);
    pax_simple_rect(&fb, COL_BAR, 0, screen_h - FOOTER_H, screen_w, FOOTER_H);
    pax_simple_rect(&fb, COL_DIVIDER, (int)divider_x, CONTENT_Y - 6, 1, screen_h - FOOTER_H - CONTENT_Y + 2);
    pax_draw_text(&fb, COL_BAR_TEXT, TEXT_FONT, TEXT_SIZE, MARGIN_X, 9, "CPU load diagnostic");
    pax_draw_text(&fb, COL_BAR_TEXT, TEXT_FONT, TEXT_SIZE, MARGIN_X, footer_y,
                  "F1/ESC Back   F3 VFD   F4/F5 Load -5/+5   F6 Start/Stop");

    int line = 0;

    snprintf(text, sizeof(text), "Load: %s", burn_is_enabled() ? "RUNNING" : "IDLE");
    line = draw_line(left_x, line, burn_is_enabled() ? COL_WARN : COL_FOREGROUND, text);
    snprintf(text, sizeof(text), "Target: %" PRIu32 " %%", burn_get_load_percent());
    line = draw_line(left_x, line, COL_FOREGROUND, text);
    for (uint32_t i = 0; i < core_count; i++) {
        snprintf(text, sizeof(text), "Core %" PRIu32 ": %5.1f Mops %3" PRIu32 " %%", i,
                 (float)stats[i].ops_per_s / 1000000.0f, stats[i].load_percent);
        line = draw_line(left_x, line, COL_FOREGROUND, text);
    }
    snprintf(text, sizeof(text), "Total:  %5.1f Mops %3" PRIu32 " %%", (float)total_ops / 1000000.0f, total_load);
    line = draw_line(left_x, line, COL_FOREGROUND, text);

    line++;

    char const* model = "Unknown";
    switch (chip.model) {
        case CHIP_ESP32P4:
            model = "ESP32-P4";
            break;
        case CHIP_ESP32S3:
            model = "ESP32-S3";
            break;
        default:
            break;
    }
    snprintf(text, sizeof(text), "Chip: %s r%" PRIu16 ", %" PRIu8 " cores", model, chip.revision, chip.cores);
    line = draw_line(left_x, line, COL_FOREGROUND, text);
    snprintf(text, sizeof(text), "Clock: %d MHz", esp_clk_cpu_freq());
    line = draw_line(left_x, line, COL_FOREGROUND, text);
    snprintf(text, sizeof(text), "Free heap: %" PRIu32 " kB", esp_get_free_heap_size() / 1024);
    line = draw_line(left_x, line, COL_FOREGROUND, text);

    line++;

    // The optional tube (vfd.h). What F3 was asked for, and whether there is
    // a display at all to ask -- a missing one should not look like a dead key.
    char const* vfd_state = vfd_is_present() ? (vfd_is_enabled() ? "on, all segments" : "off") : "no display found";
    snprintf(text, sizeof(text), "VFD: %s", vfd_state);
    line = draw_line(left_x, line, vfd_is_enabled() ? COL_WARN : COL_FOREGROUND, text);

    // Right column: the readings of the launcher's Power information dialog.
    line = 0;

    if (power.battery_err != ESP_OK) {
        snprintf(text, sizeof(text), "Battery: unavailable (%s)", esp_err_to_name(power.battery_err));
        line = draw_line(right_x, line, COL_WARN, text);
    } else if (power.battery.battery_available) {
        snprintf(text, sizeof(text), "Battery type: %s", power.battery.type ? power.battery.type : "Unknown");
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Charging disabled: %s", power.battery.charging_disabled ? "Yes" : "No");
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Battery charging: %s", power.battery.battery_charging ? "Yes" : "No");
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Max charging current: %" PRIu16 " mA", power.battery.maximum_charging_current);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Charging current: %" PRIu16 " mA", power.battery.current_charging_current);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Battery voltage: %" PRIu16 " mV", power.battery.voltage);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Charging target voltage: %" PRIu16 " mV", power.battery.charging_target_voltage);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
        snprintf(text, sizeof(text), "Remaining charge: %.2f %%", power.battery.remaining_percentage);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
    } else {
        snprintf(text, sizeof(text), "No battery detected");
        line = draw_line(right_x, line, COL_FOREGROUND, text);
    }

    line++;

    snprintf(text, sizeof(text), "Power supply detected: %s", power.battery.power_supply_available ? "Yes" : "No");
    line = draw_line(right_x, line, COL_FOREGROUND, text);
    if (power.has_system_voltage) {
        snprintf(text, sizeof(text), "System voltage: %" PRIu16 " mV", power.system_voltage);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
    }
    if (power.has_input_voltage) {
        snprintf(text, sizeof(text), "Input voltage: %" PRIu16 " mV", power.input_voltage);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
    }

    line++;

    if (power.has_soc_temperature) {
        snprintf(text, sizeof(text), "SoC temperature: ~%.2f *C", power.soc_temperature);
        line = draw_line(right_x, line, power.soc_temperature > 70.0f ? COL_WARN : COL_FOREGROUND, text);
    } else {
        snprintf(text, sizeof(text), "SoC temperature: unavailable (%s)", esp_err_to_name(power.soc_err));
        line = draw_line(right_x, line, COL_WARN, text);
    }

    if (power.has_charger_ts) {
        snprintf(text, sizeof(text), "Charger TS: %.2f %% of VREGN", power.charger_ts / 100.0f);
        line = draw_line(right_x, line, COL_FOREGROUND, text);
    } else {
        snprintf(text, sizeof(text), "Charger TS: unavailable");
        line = draw_line(right_x, line, COL_WARN, text);
    }
    snprintf(text, sizeof(text), "Charger temp: no NTC fitted");
    line = draw_line(right_x, line, COL_FOREGROUND, text);

    if (power.has_pmic_faults) {
        char faults[96] = "";
        if (power.pmic_faults.chrg_thermal) {
            strlcat(faults, "thermal ", sizeof(faults));
        }
        if (power.pmic_faults.chrg_input) {
            strlcat(faults, "input ", sizeof(faults));
        }
        if (power.pmic_faults.chrg_safety) {
            strlcat(faults, "safety ", sizeof(faults));
        }
        if (power.pmic_faults.ntc_cold || power.pmic_faults.ntc_hot) {
            strlcat(faults, "ntc ", sizeof(faults));
        }
        if (power.pmic_faults.boost) {
            strlcat(faults, "boost ", sizeof(faults));
        }
        if (power.pmic_faults.batt_ovp) {
            strlcat(faults, "battery ", sizeof(faults));
        }
        if (faults[0] == '\0') {
            snprintf(text, sizeof(text), "Charger faults: none");
            line = draw_line(right_x, line, COL_FOREGROUND, text);
        } else {
            // Too long for one line together with the label.
            line = draw_line(right_x, line, COL_WARN, "Charger faults:");
            line = draw_line(right_x, line, COL_WARN, faults);
        }
    }

    if (dropped_line_count != 0 || clipped_line_count != 0) {
        snprintf(text, sizeof(text), "%u dropped %u clipped", dropped_line_count, clipped_line_count);
        pax_draw_text(&fb, COL_WARN, TEXT_FONT, TEXT_SIZE, (float)screen_w - MARGIN_X - (float)strlen(text) * CHAR_W,
                      footer_y, text);
    }

    blit();
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage partition
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }

    // Convert BSP color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:
            format = PAX_BUF_16_565RGB;
            break;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

    // Initialize graphics stack
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    format = PAX_BUF_2_PAL;
#endif
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    fb.palette      = palette;
    fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
#endif
    pax_buf_set_orientation(&fb, orientation);

    // Get input event queue from graceloader (merges native + USB keyboard)
    ESP_ERROR_CHECK(gl_input_get_queue(&input_event_queue));

    // What the app reports
    esp_chip_info(&chip);
    ESP_ERROR_CHECK(power_info_init());

    // The optional external display. Probed before the burn tasks exist, so
    // the bus is free while it looks for the tube.
    vfd_init();

    // What the app measures
    burn_start();

    // The burn tasks run at priority 1; this task has to be above them to keep
    // redrawing while both cores are loaded.
    vTaskPrioritySet(NULL, 2);

    render();
    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    if (!event.args_navigation.state) {
                        break;
                    }
                    switch (event.args_navigation.key) {
                        case BSP_INPUT_NAVIGATION_KEY_ESC:
                        case BSP_INPUT_NAVIGATION_KEY_F1:
                        case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                            burn_set_enabled(false);
                            vfd_shutdown();  // not a tube left burning on a battery
                            bsp_device_restart_to_launcher();
                            return;
                        case BSP_INPUT_NAVIGATION_KEY_F3:
                            vfd_set_enabled(!vfd_is_enabled());
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_F4:
                            burn_adjust_load_percent(-5);
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_F5:
                            burn_adjust_load_percent(5);
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_F6:
                            burn_set_enabled(!burn_is_enabled());
                            break;
                        default:
                            break;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        render();
    }
}
