/*
 * buttons.c — GPIO falling-edge ISR + per-button debounce.
 *
 * Each button is wired to GND with the internal pull-up enabled, so a press is
 * a high->low transition (GPIO_INTR_NEGEDGE). The ISR drops bounces by ignoring
 * edges that arrive within DEBOUNCE_US of the last accepted press, then posts a
 * button_event_t to the app queue. No deferred work runs in the ISR — the app
 * task does the heavy lifting (fetch/render).
 */
#include "buttons.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "buttons";

/* USER = the EE05 board's side button 1 (net BOTTON1, XIAO D1, 10K external
 * pull-up). BOOT = the button on the XIAO module itself. The EE05's other two
 * side buttons (GPIO3, GPIO8) are unused. */
#define BUTTON_USER_GPIO  GPIO_NUM_2
#define BUTTON_BOOT_GPIO  GPIO_NUM_0
#define DEBOUNCE_US       (200 * 1000)   /* 200 ms */

static QueueHandle_t     s_queue;
static volatile int64_t  s_last_us[BUTTON_COUNT];

static void IRAM_ATTR button_isr(void *arg)
{
    button_id_t id = (button_id_t)(uintptr_t)arg;

    /* esp_timer_get_time() is ISR-safe (IRAM); use it to debounce. */
    int64_t now = esp_timer_get_time();
    if (now - s_last_us[id] < DEBOUNCE_US) {
        return;
    }
    s_last_us[id] = now;

    button_event_t ev = { .id = id };
    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(s_queue, &ev, &hp_task_woken);
    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
}

void buttons_init(QueueHandle_t out_queue)
{
    s_queue = out_queue;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_USER_GPIO) | (1ULL << BUTTON_BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* Tolerate the ISR service already being installed by another component. */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_USER_GPIO, button_isr, (void *)BUTTON_USER));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_BOOT_GPIO, button_isr, (void *)BUTTON_BOOT));

    ESP_LOGI(TAG, "ready: USER=GPIO%d, BOOT=GPIO%d",
             BUTTON_USER_GPIO, BUTTON_BOOT_GPIO);
}

bool buttons_both_pressed(void)
{
    /* Active-low: a held button reads 0. */
    return gpio_get_level(BUTTON_USER_GPIO) == 0 && gpio_get_level(BUTTON_BOOT_GPIO) == 0;
}
