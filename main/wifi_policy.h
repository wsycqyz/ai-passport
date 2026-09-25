// main/wifi_policy.h —— Station failure classification and retry limits.
//
// Pure logic so the mapping is host-tested. Reason codes are the ESP-IDF 5.5.3
// wifi_err_reason_t values reported in WIFI_EVENT_STA_DISCONNECTED
// (esp_wifi_types_generic.h); they are repeated here as plain numbers so this
// file does not depend on ESP-IDF headers.
#pragma once

#include <stdint.h>

typedef enum {
    WIFI_FAIL_NONE = 0,
    WIFI_FAIL_NOT_FOUND,
    WIFI_FAIL_WRONG_PASSWORD,
    WIFI_FAIL_SECURITY,
    WIFI_FAIL_AP_FULL,
    WIFI_FAIL_REJECTED,
    WIFI_FAIL_NO_IP,          // associated, but DHCP gave no address in time
    WIFI_FAIL_TIMEOUT,        // no association result in time
    WIFI_FAIL_BAD_PASSWORD,   // driver refused the configured password format
    WIFI_FAIL_DRIVER,         // radio could not start/connect
    WIFI_FAIL_UNKNOWN,
} wifi_fail_t;

wifi_fail_t wifi_policy_classify(uint16_t reason);

// Total connection attempts allowed once this failure has been seen.
uint8_t wifi_policy_attempts(wifi_fail_t fail);

// Short page title and one-sentence advice; UNKNOWN is shown as "Error".
const char *wifi_policy_title(wifi_fail_t fail);
const char *wifi_policy_hint(wifi_fail_t fail);
