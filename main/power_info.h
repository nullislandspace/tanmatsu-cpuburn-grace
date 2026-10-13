// Battery, power supply and temperature readings for the diagnostic screen.
//
// The battery/supply values are the ones the launcher shows in its "Power
// information" dialog. On top of that this collects the ESP32-P4 SoC
// temperature and what the BQ25895 charger IC reports.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bsp/power.h"
#include "esp_err.h"
#include "tanmatsu_coprocessor.h"

typedef struct {
    // Battery + power supply, as in the launcher's Power information dialog
    esp_err_t                     battery_err;
    bsp_power_battery_information_t battery;
    bool     has_system_voltage;
    uint16_t system_voltage;  // mV
    bool     has_input_voltage;
    uint16_t input_voltage;  // mV

    // ESP32-P4 internal temperature sensor
    bool      has_soc_temperature;
    float     soc_temperature;  // *C
    esp_err_t soc_err;

    // BQ25895 charger IC. The TS pin has no thermistor on the Tanmatsu (it is
    // a fixed 10k/10k divider to REGN), so the charger temperature cannot be
    // read; the TS percentage and the fault flags are what the IC has to say
    // about its own temperature.
    bool                    has_charger_ts;
    uint16_t                charger_ts;  // TS pin, percent of VREGN x 100 (divide by 100)
    bool                    has_pmic_faults;
    tanmatsu_coprocessor_pmic_faults_t pmic_faults;
} power_info_t;

// Installs the SoC temperature sensor and looks up the coprocessor handle.
esp_err_t power_info_init(void);

// Reads everything again into out.
esp_err_t power_info_collect(power_info_t* out);
