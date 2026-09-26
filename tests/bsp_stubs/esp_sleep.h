#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_GPIO_WAKEUP_GPIO_LOW = 0, ESP_GPIO_WAKEUP_GPIO_HIGH = 1 } esp_deepsleep_gpio_wake_up_mode_t;
typedef enum { ESP_SLEEP_WAKEUP_GPIO = 7 } esp_sleep_source_t;
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t, esp_deepsleep_gpio_wake_up_mode_t);
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t);
