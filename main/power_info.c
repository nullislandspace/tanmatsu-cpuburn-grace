#include "power_info.h"
#include "bsp/tanmatsu.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>

static char const TAG[] = "power_info";

static temperature_sensor_handle_t  temp_handle  = NULL;
static tanmatsu_coprocessor_handle_t coprocessor = NULL;

esp_err_t power_info_init(void) {
    // The internal sensor is only accurate within one of its fixed ranges;
    // -10..80 *C is the range with the smallest error and still covers a die
    // that has been loaded for a while.
    temperature_sensor_config_t const config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_RETURN_ON_ERROR(temperature_sensor_install(&config, &temp_handle), TAG,
                        "Failed to install the temperature sensor");
    ESP_RETURN_ON_ERROR(temperature_sensor_enable(temp_handle), TAG, "Failed to enable the temperature sensor");

    esp_err_t res = bsp_tanmatsu_coprocessor_get_handle(&coprocessor);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "No coprocessor handle, charger readings unavailable: %s", esp_err_to_name(res));
        coprocessor = NULL;
    }

    return ESP_OK;
}

esp_err_t power_info_collect(power_info_t* out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    out->battery_err = bsp_power_get_battery_information(&out->battery);
    out->has_system_voltage = bsp_power_get_system_voltage(&out->system_voltage) == ESP_OK;
    out->has_input_voltage = bsp_power_get_input_voltage(&out->input_voltage) == ESP_OK;

    if (temp_handle != NULL) {
        out->soc_err = temperature_sensor_get_celsius(temp_handle, &out->soc_temperature);
        out->has_soc_temperature = out->soc_err == ESP_OK;
    }

    if (coprocessor != NULL) {
        out->has_charger_ts = tanmatsu_coprocessor_get_pmic_ts(coprocessor, &out->charger_ts) == ESP_OK;
        out->has_pmic_faults = tanmatsu_coprocessor_get_pmic_faults(coprocessor, &out->pmic_faults) == ESP_OK;
    }

    return ESP_OK;
}
