// components/sonic_link/src/sonic_wifi.c
#include "sonic_wifi.h"

#include <string.h>

static bool printable(const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) return false;
    }
    return true;
}

static bool hex_digits(const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = s[i];
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

bool sonic_wifi_password_valid(const uint8_t *password, size_t len)
{
    if (len == 0) return true;
    if (!password || len > SONIC_WIFI_PASSWORD_MAX) return false;
    if (len == SONIC_WIFI_PASSWORD_MAX) return hex_digits(password, len);
    if (len == 5 || (len >= 8 && len <= 63)) return printable(password, len);
    return false;
}

static bool ssid_valid(const uint8_t *ssid, size_t len)
{
    if (len == 0 || len > SONIC_WIFI_SSID_MAX) return false;
    return memchr(ssid, 0, len) == NULL;
}

sonic_wifi_status_t sonic_wifi_parse(const uint8_t *payload, size_t len,
                                     sonic_wifi_credentials_t *out)
{
    if (!out) return SONIC_WIFI_ERR_FORMAT;
    sonic_wifi_wipe(out);
    if (!payload || len < 3) return SONIC_WIFI_ERR_FORMAT;
    if (payload[0] != SONIC_PAYLOAD_WIFI) return SONIC_WIFI_ERR_TYPE;

    const size_t ssid_len = payload[1];
    if (ssid_len == 0 || ssid_len > SONIC_WIFI_SSID_MAX) return SONIC_WIFI_ERR_SSID;
    if (len < 3 + ssid_len) return SONIC_WIFI_ERR_FORMAT;
    const size_t pass_len = payload[2 + ssid_len];
    if (len != 3 + ssid_len + pass_len) return SONIC_WIFI_ERR_FORMAT;

    const uint8_t *ssid = payload + 2;
    const uint8_t *password = payload + 3 + ssid_len;
    if (!ssid_valid(ssid, ssid_len)) return SONIC_WIFI_ERR_SSID;
    if (!sonic_wifi_password_valid(password, pass_len)) return SONIC_WIFI_ERR_PASSWORD;

    memcpy(out->ssid, ssid, ssid_len);
    out->ssid_len = (uint8_t)ssid_len;
    memcpy(out->password, password, pass_len);
    out->password_len = (uint8_t)pass_len;
    return SONIC_WIFI_OK;
}

size_t sonic_wifi_encode(const sonic_wifi_credentials_t *creds, uint8_t *out, size_t capacity)
{
    if (!creds || !out || !ssid_valid(creds->ssid, creds->ssid_len) ||
        !sonic_wifi_password_valid(creds->password, creds->password_len)) {
        return 0;
    }
    const size_t len = 3u + creds->ssid_len + creds->password_len;
    if (capacity < len) return 0;
    out[0] = SONIC_PAYLOAD_WIFI;
    out[1] = creds->ssid_len;
    memcpy(out + 2, creds->ssid, creds->ssid_len);
    out[2 + creds->ssid_len] = creds->password_len;
    memcpy(out + 3 + creds->ssid_len, creds->password, creds->password_len);
    return len;
}

void sonic_wifi_wipe(sonic_wifi_credentials_t *creds)
{
    if (!creds) return;
    // volatile stores so the compiler cannot drop the wipe of a dying object.
    volatile uint8_t *p = (volatile uint8_t *)creds;
    for (size_t i = 0; i < sizeof(*creds); i++) p[i] = 0;
}
