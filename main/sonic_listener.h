// main/sonic_listener.h —— Microphone worker feeding the SonicLink receiver.
//
// sonic_listener_start() wakes the ES8311 (16 kHz mono), then a dedicated task
// reads 16 ms PCM chunks and runs the receiver. The task never touches LVGL; it
// publishes a status snapshot and posts events. sonic_listener_stop() is a
// bounded handshake (the worker exits between chunks) followed by
// bsp_audio_sleep(), so the codec and microphone bias are off when idle.
// start/stop/take must be called from one controller task.
#pragma once

#include "esp_err.h"
#include "sonic_wifi.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SONIC_LISTENER_EVT_RECEIVING = 0,   // header accepted; body incoming
    SONIC_LISTENER_EVT_DAMAGED,         // a frame failed; still listening
    SONIC_LISTENER_EVT_CREDENTIALS,     // valid credentials ready for take()
    SONIC_LISTENER_EVT_UNSUPPORTED,     // frame decoded but not Wi-Fi credentials
    SONIC_LISTENER_EVT_AUDIO_ERROR,     // microphone read failed; worker idle
} sonic_listener_event_t;

// Called from the worker task; must not block.
typedef void (*sonic_listener_post_t)(sonic_listener_event_t event);

typedef struct {
    uint8_t level;        // input level 0..100 (-60..0 dBFS), peak-hold decay
    bool clipping;        // samples near full scale in the last ~0.5 s
    bool receiving;       // a frame body is being received
    uint16_t progress;    // body progress in permille
} sonic_listener_status_t;

esp_err_t sonic_listener_start(sonic_listener_post_t post);

// Returns ESP_ERR_TIMEOUT if the worker did not exit in time; call again.
esp_err_t sonic_listener_stop(void);

void sonic_listener_status(sonic_listener_status_t *out);

// Move the received credentials to out (and wipe the internal copy).
bool sonic_listener_take(sonic_wifi_credentials_t *out);
