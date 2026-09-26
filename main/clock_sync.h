// main/clock_sync.h —— Sets the clock from public NTP servers once Wi-Fi is up.
//
// The first call starts the ESP-IDF SNTP client (pool.ntp.org, then
// time.cloudflare.com); later calls restart it so a reconnect asks again at
// once instead of waiting for the hourly resync. The system time runs on the
// RTC timer, so it survives the idle deep sleep and resets, but not a power
// cut. Local time is derived by the caller (UTC+8 in this app).
//
// Controller task only. on_sync runs in the lwIP task after every successful
// sync and must not block.
#pragma once

#include "esp_err.h"

#include <stdint.h>

typedef void (*clock_sync_cb_t)(int64_t utc_seconds);

esp_err_t clock_sync_start(clock_sync_cb_t on_sync);
