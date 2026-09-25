// components/sonic_link/include/sonic_wifi.h
// Wi-Fi station credentials carried as a SonicLink payload.
//
// Payload type 0x01 layout (all lengths in bytes):
//   [0]            0x01
//   [1]            ssid_len (1..32)
//   [2..]          SSID bytes (no NUL bytes; UTF-8 allowed)
//   [2+ssid_len]   pass_len (0..64)
//   [...]          password bytes
// Password rules follow ESP-IDF station configuration: empty for an open
// network, 8..63 printable ASCII characters (WPA/WPA2/WPA3 passphrase),
// exactly 64 hexadecimal digits (raw PSK), or 5/13 printable characters (WEP).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SONIC_PAYLOAD_WIFI       0x01
#define SONIC_WIFI_SSID_MAX      32
#define SONIC_WIFI_PASSWORD_MAX  64
#define SONIC_WIFI_PAYLOAD_MAX   (3 + SONIC_WIFI_SSID_MAX + SONIC_WIFI_PASSWORD_MAX)

typedef struct {
    uint8_t ssid[SONIC_WIFI_SSID_MAX];
    uint8_t ssid_len;
    uint8_t password[SONIC_WIFI_PASSWORD_MAX];
    uint8_t password_len;
} sonic_wifi_credentials_t;

typedef enum {
    SONIC_WIFI_OK = 0,
    SONIC_WIFI_ERR_TYPE,       // payload is not a Wi-Fi credential record
    SONIC_WIFI_ERR_FORMAT,     // truncated or inconsistent lengths
    SONIC_WIFI_ERR_SSID,       // empty, too long, or contains NUL bytes
    SONIC_WIFI_ERR_PASSWORD,   // length/characters unusable by a station
} sonic_wifi_status_t;

bool sonic_wifi_password_valid(const uint8_t *password, size_t len);

// Parse and validate a payload. out is always cleared first.
sonic_wifi_status_t sonic_wifi_parse(const uint8_t *payload, size_t len,
                                     sonic_wifi_credentials_t *out);

// Serialise credentials; returns the payload length or 0 if invalid/too small.
size_t sonic_wifi_encode(const sonic_wifi_credentials_t *creds, uint8_t *out, size_t capacity);

// Overwrite credentials (use before releasing buffers that held a password).
void sonic_wifi_wipe(sonic_wifi_credentials_t *creds);
