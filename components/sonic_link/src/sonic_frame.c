// components/sonic_link/src/sonic_frame.c
// SonicLink framing shared by transmitters and tests: preamble digits, CRC,
// parity policy, and conversion of a payload into the symbol digit sequence.
#include "sonic_link.h"

#include <string.h>

const uint8_t SONIC_PREAMBLE[SONIC_PREAMBLE_SYMBOLS] = {
    2, 8, 9, 12, 4, 14, 10, 15, 13, 7, 6, 3, 11, 1, 5, 0,
};

uint16_t sonic_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint8_t sonic_default_parity(size_t payload_len)
{
    const size_t body = payload_len + SONIC_CRC_BYTES;
    size_t parity = 2 * ((body * 15 + 99) / 100);   // 2 * ceil(0.15 * body)
    if (parity < 16) parity = 16;
    if (parity > SONIC_PARITY_MAX) parity = SONIC_PARITY_MAX;
    while (parity > SONIC_PARITY_MIN && body + parity > SONIC_CODEWORD_MAX) parity -= 2;
    return (uint8_t)parity;
}

size_t sonic_frame_symbol_count(size_t payload_len, uint8_t parity)
{
    const size_t body = payload_len + SONIC_CRC_BYTES;
    if (payload_len == 0 || (parity & 1u) || parity < SONIC_PARITY_MIN ||
        parity > SONIC_PARITY_MAX || body + parity > SONIC_CODEWORD_MAX) {
        return 0;
    }
    return SONIC_PREAMBLE_SYMBOLS + 2 * (SONIC_HEADER_BYTES + body + parity);
}

static size_t put_bytes(uint8_t *digits, size_t at, const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        digits[at++] = (uint8_t)(bytes[i] >> 4);
        digits[at++] = (uint8_t)(bytes[i] & 0x0F);
    }
    return at;
}

size_t sonic_frame_build(const uint8_t *payload, size_t payload_len, uint8_t parity,
                         uint8_t *digits, size_t capacity)
{
    const size_t count = sonic_frame_symbol_count(payload_len, parity);
    if (count == 0 || !payload || !digits || capacity < count) return 0;

    const size_t body_len = payload_len + SONIC_CRC_BYTES;
    uint8_t header[SONIC_HEADER_BYTES] = { SONIC_VERSION, (uint8_t)body_len, parity };
    uint8_t body[SONIC_CODEWORD_MAX];
    memcpy(body, payload, payload_len);
    const uint16_t crc = sonic_crc16(payload, payload_len);
    body[payload_len] = (uint8_t)(crc >> 8);
    body[payload_len + 1] = (uint8_t)(crc & 0xFF);
    if (sonic_rs_encode(header, SONIC_HEADER_DATA, SONIC_HEADER_PARITY,
                        header + SONIC_HEADER_DATA) != 0 ||
        sonic_rs_encode(body, body_len, parity, body + body_len) != 0) {
        memset(body, 0, sizeof(body));
        return 0;
    }

    memcpy(digits, SONIC_PREAMBLE, SONIC_PREAMBLE_SYMBOLS);
    size_t at = SONIC_PREAMBLE_SYMBOLS;
    at = put_bytes(digits, at, header, sizeof(header));
    at = put_bytes(digits, at, body, body_len + parity);
    // The payload may contain credentials; do not leave a stack copy behind.
    memset(body, 0, sizeof(body));
    return at;
}
