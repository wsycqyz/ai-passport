// main/app_text.c
#include "app_text.h"

static size_t utf8_sequence_length(uint8_t lead)
{
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 1;   // ASCII, stray continuation, or invalid lead byte
}

void app_text_ssid(const uint8_t *ssid, size_t len, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t o = 0;
    size_t i = 0;
    while (ssid && i < len && o + 1 < cap) {
        const uint8_t c = ssid[i];
        if (c >= 0x20 && c <= 0x7E) {
            out[o++] = (char)c;
            i++;
            continue;
        }
        size_t n = utf8_sequence_length(c);
        size_t valid = 1;
        while (valid < n && i + valid < len && (ssid[i + valid] & 0xC0) == 0x80) valid++;
        // A truncated sequence only consumes the bytes that belong to it.
        i += valid == n ? n : 1;
        out[o++] = '?';
    }
    out[o] = '\0';
}
