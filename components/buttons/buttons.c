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

#define DEBOUNCE_US       (200 * 1000)   /* 200 ms */

static QueueHandle_t     s_queue;
static int               s_gpio[BUTTON_COUNT];
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

void buttons_init(QueueHandle_t out_queue, const int gpios[BUTTON_COUNT])
{
    s_queue = out_queue;

    uint64_t mask = 0;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        s_gpio[i] = gpios ? gpios[i] : -1;
        if (s_gpio[i] >= 0) mask |= 1ULL << s_gpio[i];
    }
    if (!mask) {
        ESP_LOGW(TAG, "no buttons configured");
        return;
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
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

    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (s_gpio[i] < 0) continue;
        ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)s_gpio[i], button_isr,
                                             (void *)(uintptr_t)i));
    }

    ESP_LOGI(TAG, "ready: KEY0=GPIO%d KEY1=GPIO%d KEY2=GPIO%d BOOT=GPIO%d",
             s_gpio[BUTTON_KEY0], s_gpio[BUTTON_KEY1],
             s_gpio[BUTTON_KEY2], s_gpio[BUTTON_BOOT]);
}

bool buttons_is_pressed(button_id_t id)
{
    if (id < 0 || id >= BUTTON_COUNT || s_gpio[id] < 0) {
        return false;
    }
    /* Active-low: a held button reads 0. */
    return gpio_get_level((gpio_num_t)s_gpio[id]) == 0;
}
