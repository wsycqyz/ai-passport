// main/wifi_link.h —— Wi-Fi station connection manager with credential storage.
//
// Threading: ESP-IDF event handlers and the timeout timer only forward
// wifi_link_msg_t values through the post callback (non-blocking). Every other
// function, including wifi_link_handle(), must run in the single controller
// task, so the state machine needs no locking.
//
// Each attempt restarts the station (esp_wifi_stop/start) and waits for
// WIFI_EVENT_STA_START before connecting, so stale events from an earlier
// attempt are ignored. Credentials are stored in NVS namespace "sonic_wifi"
// only when the caller asks (after a successful connection); the driver itself
// runs with WIFI_STORAGE_RAM.
#pragma once

#include "esp_err.h"
#include "sonic_wifi.h"
#include "wifi_policy.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_LINK_MSG_STA_START = 0,
    WIFI_LINK_MSG_STA_CONNECTED,
    WIFI_LINK_MSG_STA_DISCONNECTED,
    WIFI_LINK_MSG_GOT_IP,
    WIFI_LINK_MSG_TIMEOUT,
} wifi_link_msg_type_t;

typedef struct {
    wifi_link_msg_type_t type;
    uint16_t reason;      // disconnect reason (wifi_err_reason_t)
    uint32_t token;       // timeout generation
} wifi_link_msg_t;

// Called from the event loop or esp_timer task; must not block.
typedef void (*wifi_link_post_t)(const wifi_link_msg_t *msg);

typedef enum {
    WIFI_LINK_IDLE = 0,
    WIFI_LINK_STARTING,
    WIFI_LINK_JOINING,
    WIFI_LINK_GETTING_IP,
    WIFI_LINK_CONNECTED,
} wifi_link_stage_t;

typedef enum {
    WIFI_LINK_NONE = 0,       // message ignored (stale or irrelevant)
    WIFI_LINK_PROGRESS,       // stage or attempt changed
    WIFI_LINK_UP,             // got an IP address
    WIFI_LINK_FAILED,         // gave up; see wifi_link_failure()
    WIFI_LINK_LOST,           // an established connection dropped
    WIFI_LINK_INFO_CHANGED,   // IP information renewed while connected
} wifi_link_result_t;

typedef struct {
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
    int8_t rssi;
    uint8_t channel;
} wifi_link_info_t;

// Prepare NVS, netif, event loop and the station driver. Safe to call again
// after a failure; returns the first error.
esp_err_t wifi_link_init(wifi_link_post_t post);

bool wifi_link_load(sonic_wifi_credentials_t *out);
esp_err_t wifi_link_save(const sonic_wifi_credentials_t *creds);

// Start connecting (async). Returns WIFI_LINK_PROGRESS, or WIFI_LINK_FAILED
// when the attempt cannot even start (see wifi_link_failure()).
wifi_link_result_t wifi_link_begin(const sonic_wifi_credentials_t *creds);
wifi_link_result_t wifi_link_handle(const wifi_link_msg_t *msg);

// Disconnect and power the radio down; late events are ignored afterwards.
void wifi_link_stop(void);

wifi_link_stage_t wifi_link_stage(void);
uint8_t wifi_link_attempt(void);
uint8_t wifi_link_attempt_limit(void);
uint32_t wifi_link_stage_elapsed_ms(void);
wifi_fail_t wifi_link_failure(void);
uint16_t wifi_link_reason(void);

// IP details of the current connection; false when not connected.
bool wifi_link_info(wifi_link_info_t *out);

// Re-read RSSI/channel of the connected AP into the cached info.
void wifi_link_refresh_signal(void);
