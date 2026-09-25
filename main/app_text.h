// main/app_text.h —— Text helpers for dynamic UI content.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Format an SSID for the built-in Montserrat fonts, which cover printable
// ASCII only. Supported set: 0x20..0x7E are copied; every other UTF-8 code
// point (or invalid byte) becomes one '?'. The result is always
// NUL-terminated and truncated to fit cap.
void app_text_ssid(const uint8_t *ssid, size_t len, char *out, size_t cap);
