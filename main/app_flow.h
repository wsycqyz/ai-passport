// main/app_flow.h —— Application state machine for "Wi-Fi by sound".
//
// Pure logic (no ESP-IDF/LVGL) so every transition is host-tested. The
// controller task feeds events and executes the returned APP_ACTION_* bits:
//
//   BOOT --saved--> CONNECTING --ok--> CONNECTED --button--> SETUP
//     |                |  \--fail--> FAILED --button--> SETUP
//     |                \--button--> SETUP
//     \--empty--> SETUP --button--> LISTENING --credentials--> CONNECTING
//                                     |--button/timeout/audio error--> SETUP
//   CONNECTED --connection lost--> CONNECTING (same credentials)
//
// Every page has exactly one on-screen button, activated with the OK key.
#pragma once

#include <stdint.h>

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_CONNECTING,
    APP_STATE_SETUP,
    APP_STATE_LISTENING,
    APP_STATE_CONNECTED,
    APP_STATE_FAILED,
} app_state_t;

typedef enum {
    APP_SOURCE_SAVED = 0,     // credentials stored in flash
    APP_SOURCE_SOUND,         // credentials just received; save only after success
    APP_SOURCE_RECONNECT,     // link dropped; retry the credentials in use
} app_source_t;

typedef enum {
    APP_NOTE_NONE = 0,
    APP_NOTE_NO_SAVED,
    APP_NOTE_CONNECT_STOPPED,
    APP_NOTE_LISTEN_CANCELLED,
    APP_NOTE_LISTEN_TIMEOUT,
    APP_NOTE_AUDIO_ERROR,
} app_note_t;

typedef enum {
    APP_EVENT_BOOT_SAVED = 0,
    APP_EVENT_BOOT_EMPTY,
    APP_EVENT_BUTTON,
    APP_EVENT_CREDENTIALS,
    APP_EVENT_LISTEN_TIMEOUT,
    APP_EVENT_LISTEN_FAILED,
    APP_EVENT_WIFI_CONNECTED,
    APP_EVENT_WIFI_FAILED,
    APP_EVENT_WIFI_LOST,
} app_event_t;

// Actions, executed by the controller in this order: stop listener, stop
// Wi-Fi, save, start listener, render, connect.
#define APP_ACTION_LISTEN_STOP  (1u << 0)
#define APP_ACTION_WIFI_STOP    (1u << 1)
#define APP_ACTION_SAVE         (1u << 2)
#define APP_ACTION_LISTEN_START (1u << 3)
#define APP_ACTION_RENDER       (1u << 4)
#define APP_ACTION_CONNECT      (1u << 5)

typedef struct {
    app_state_t state;
    app_source_t source;
    app_note_t note;
} app_flow_t;

void app_flow_init(app_flow_t *flow);

// Apply an event; returns the actions to execute (0 when ignored).
uint32_t app_flow_handle(app_flow_t *flow, app_event_t event);

// User-facing note for the setup page, or NULL.
const char *app_flow_note_text(app_note_t note);
