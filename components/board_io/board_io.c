/*
 * board_io.c — battery sensing on the EE04. See board_io.h.
 *
 * Speaks the ESP-IDF adc_oneshot + adc_cali APIs directly; no sensor library.
 * The GPIO -> (unit, channel) mapping is asked of the driver rather than
 * hardcoded, so main/user_config.h stays the only place that names a pin.
 */
#include "board_io.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_io";

#define BATT_DIVIDER   3.0f     /* the board divides the cell voltage by 3 */
#define BATT_FULL_V    4.12f
#define BATT_EMPTY_V   3.00f

/* Below this the input is open, not flat: a Li-ion that reaches 2.5V has had
 * its protection circuit cut it off long before, so anything under this is a
 * missing battery (USB-only operation) rather than a dead one. */
#define BATT_PRESENT_V 2.50f

/* One reading is ~10mV of noise on a 12-bit ADC behind a divider; averaging a
 * handful costs microseconds and stops the battery chip flickering between
 * percentages on every tick. */
#define BATT_SAMPLES   8

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_unit_t                s_unit;
static adc_channel_t             s_chan;
static bool                      s_ready;
static bool                      s_present;

void board_io_init(int adc_gpio, int enable_gpio)
{
    if (enable_gpio >= 0) {
        gpio_config_t en = {
            .pin_bit_mask = 1ULL << enable_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&en) == ESP_OK) {
            gpio_set_level((gpio_num_t)enable_gpio, 1);
            /* The divider needs a moment to charge through its own resistance
             * before the first conversion, or the boot reading reads low. */
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGW(TAG, "battery enable GPIO%d config failed", enable_gpio);
        }
    }

    if (adc_gpio < 0 || adc_oneshot_io_to_channel(adc_gpio, &s_unit, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d is not an ADC pin — battery reporting disabled", adc_gpio);
        return;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = s_unit };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc unit init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    if (adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "adc channel config failed");
        return;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = s_unit,
        .chan     = s_chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable (uncalibrated voltage only)");
        s_cali = NULL;
    }
    s_ready = true;

    float v = board_io_battery_voltage();
    ESP_LOGI(TAG, "battery ADC on GPIO%d (unit %d ch %d): %.2fV%s",
             adc_gpio, (int)s_unit, (int)s_chan, v,
             s_present ? "" : " — no cell fitted, USB power");
}

float board_io_battery_voltage(void)
{
    if (!s_ready) return 0.0f;

    long acc = 0;
    int  got = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) continue;
        acc += raw;
        got++;
    }
    if (!got) return 0.0f;
    int raw = (int)(acc / got);

    float v;
    if (s_cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return 0.0f;
        v = 0.001f * (float)mv * BATT_DIVIDER;
    } else {
        /* No calibration: approximate against the 12dB full-scale (~3.1V). */
        v = ((float)raw / 4095.0f) * 3.1f * BATT_DIVIDER;
    }

    s_present = v >= BATT_PRESENT_V;
    return v;
}

int board_io_battery_percent(void)
{
    float v = board_io_battery_voltage();
    if (v < BATT_PRESENT_V) return 0;
    if (v <= BATT_EMPTY_V)  return 0;
    if (v >= BATT_FULL_V)   return 100;
    return (int)((v - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f + 0.5f);
}

bool board_io_battery_present(void)
{
    return s_present;
}
