// main/app_flow.h —— Page and connection state machine of the heatmap app.
//
// Pure logic (no ESP-IDF/LVGL) so every transition is host-tested. The
// controller task feeds events and executes the returned APP_ACTION_* bits:
//
//   BOOT --saved--> HEATMAP (connects in the background)
//     \--empty--> SETUP
//   HEATMAP --OK--> SETUP --OK--> LISTENING --credentials--> CONNECTING
//   CONNECTING --connected--> HEATMAP (the new credentials are saved)
//              \--failed--> FAILED --OK--> SETUP
//   LISTENING --OK/timeout/audio error--> SETUP;  CONNECTING --OK--> SETUP
//   SETUP, LISTENING, CONNECTING, FAILED --BACK (UP or DOWN)--> HEATMAP
//
// On HEATMAP the link is OFF (nothing saved), CONNECTING, UP, or WAITING for
// a retry after a failed attempt; a dropped link reconnects at once. The
// Wi-Fi setup pages keep the radio off except while trying new credentials.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_HEATMAP,
    APP_STATE_SETUP,
    APP_STATE_LISTENING,
    APP_STATE_CONNECTING,
    APP_STATE_FAILED,
} app_state_t;

typedef enum {
    APP_LINK_OFF = 0,
    APP_LINK_CONNECTING,
    APP_LINK_UP,
    APP_LINK_WAITING,
} app_link_t;

typedef enum {
    APP_SOURCE_SAVED = 0,     // credentials stored in flash
    APP_SOURCE_SOUND,         // credentials just received; save only after success
    APP_SOURCE_RECONNECT,     // retry the credentials in use
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
    APP_EVENT_OK,             // OK key
    APP_EVENT_BACK,           // UP or DOWN key on a Wi-Fi setup page
    APP_EVENT_CREDENTIALS,
    APP_EVENT_LISTEN_TIMEOUT,
    APP_EVENT_LISTEN_FAILED,
    APP_EVENT_WIFI_CONNECTED,
    APP_EVENT_WIFI_FAILED,
    APP_EVENT_WIFI_LOST,
    APP_EVENT_RETRY,          // the background retry delay elapsed
} app_event_t;

// Actions, executed by the controller in this order: stop listener, stop
// Wi-Fi, cancel download, save, start listener, schedule retry, render (or
// refresh the online state), connect.
#define APP_ACTION_LISTEN_STOP   (1u << 0)
#define APP_ACTION_WIFI_STOP     (1u << 1)
#define APP_ACTION_FETCH_CANCEL  (1u << 2)
#define APP_ACTION_SAVE          (1u << 3)
#define APP_ACTION_LISTEN_START  (1u << 4)
#define APP_ACTION_RETRY_LATER   (1u << 5)
#define APP_ACTION_RENDER        (1u << 6)
#define APP_ACTION_LINK          (1u << 7)   // online state changed on the main page
#define APP_ACTION_CONNECT       (1u << 8)

typedef struct {
    app_state_t state;
    app_link_t link;
    app_source_t source;
    app_note_t note;
    bool have_saved;          // credentials in flash; the controller updates it
} app_flow_t;

void app_flow_init(app_flow_t *flow);

// Apply an event; returns the actions to execute (0 when ignored).
uint32_t app_flow_handle(app_flow_t *flow, app_event_t event);

// User-facing note for the setup page, or NULL.
const char *app_flow_note_text(app_note_t note);

// Exponential back-off: base_ms after the first failure, doubling, at most cap_ms.
uint32_t app_retry_delay_ms(uint32_t base_ms, uint32_t cap_ms, uint8_t failures);

// Whether the rolling year should be downloaded: at least once per power-on
// (history restored from flash is still refreshed), then every period_us.
bool app_refresh_due(bool downloaded_since_boot, int64_t now_us, int64_t last_ok_us, int64_t period_us);
