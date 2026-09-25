// components/sonic_link/include/sonic_link.h
// SonicLink: a small data-over-sound link (PC speaker -> device microphone).
//
// Physical layer (v1, must match tools/sonic_link.py and the protocol document
// docs/assets/sonic-link-protocol.md):
//   - receiver audio: 16 kHz, 16-bit mono PCM;
//   - 16-ary continuous-phase FSK, 4 bits per 40 ms symbol;
//   - 32 tones in two interleaved banks. Symbol i (counted from the first
//     preamble symbol) uses bank i % 2, tone bin = 24 + 2 * bank + 4 * digit of
//     a 256-point analysis at 16 kHz (62.5 Hz per bin, 1500..5375 Hz). Adjacent
//     symbols therefore never share candidate tones, so the room echo of the
//     previous symbol does not compete with the current one;
//   - 16-symbol Costas-array preamble for detection and timing;
//   - header: RS(9,3) codeword {version, body_len, body_parity};
//   - body: RS(body_len + body_parity, body_len) codeword whose data ends with a
//     CRC-16/CCITT-FALSE of the payload. Bytes are sent high nibble first.
//
// Pure C99 without dynamic allocation or ESP-IDF dependencies, so it can be
// unit-tested on a host and reused on other MCUs. The receiver uses integer
// Goertzel filters per sample (the ESP32-C3 has no FPU) and a little float
// work per 4 ms analysis frame.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sonic_rs.h"

#define SONIC_SAMPLE_RATE_HZ    16000
#define SONIC_WINDOW            256
#define SONIC_HOP               64
#define SONIC_SYMBOL_SAMPLES    640
#define SONIC_FRAMES_PER_SYMBOL (SONIC_SYMBOL_SAMPLES / SONIC_HOP)
#define SONIC_DIGITS            16
#define SONIC_BANKS             2
#define SONIC_TONES             (SONIC_DIGITS * SONIC_BANKS)
#define SONIC_BIN_BASE          24
#define SONIC_PREAMBLE_SYMBOLS  16

#define SONIC_VERSION           1
#define SONIC_HEADER_DATA       3
#define SONIC_HEADER_PARITY     6
#define SONIC_HEADER_BYTES      (SONIC_HEADER_DATA + SONIC_HEADER_PARITY)
#define SONIC_CRC_BYTES         2
#define SONIC_BODY_MIN          (1 + SONIC_CRC_BYTES)
#define SONIC_PARITY_MIN        4
#define SONIC_PARITY_MAX        SONIC_RS_MAX_PARITY
#define SONIC_CODEWORD_MAX      SONIC_RS_MAX_CODEWORD
#define SONIC_PAYLOAD_MAX       (SONIC_CODEWORD_MAX - SONIC_PARITY_MIN - SONIC_CRC_BYTES)

// Analysis bin of a tone; frequency in Hz = bin * 62.5.
static inline int sonic_tone_bin(int bank, int digit)
{
    return SONIC_BIN_BASE + 2 * bank + 4 * digit;
}

// Costas-array preamble digits (Welch construction, p = 17, g = 3).
extern const uint8_t SONIC_PREAMBLE[SONIC_PREAMBLE_SYMBOLS];

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0).
uint16_t sonic_crc16(const uint8_t *data, size_t len);

// ---------------------------------------------------------------------------
// Frame builder (transmit side). Used by host tests and available to firmware
// that wants to emit SonicLink audio itself.
// ---------------------------------------------------------------------------

// Parity policy of the reference encoder: max(16, 2 * ceil(0.15 * body_len)).
uint8_t sonic_default_parity(size_t payload_len);

// Number of symbols (preamble included) for a payload, or 0 if invalid.
size_t sonic_frame_symbol_count(size_t payload_len, uint8_t parity);

// Write the digit sequence (0..15 per symbol, preamble included) for payload.
// parity must be even and within [SONIC_PARITY_MIN, SONIC_PARITY_MAX].
// Returns the number of digits written, or 0 on invalid input/capacity.
size_t sonic_frame_build(const uint8_t *payload, size_t payload_len, uint8_t parity,
                         uint8_t *digits, size_t capacity);

// ---------------------------------------------------------------------------
// Streaming receiver.
// ---------------------------------------------------------------------------

typedef enum {
    SONIC_RX_SEARCHING = 0,   // looking for a preamble
    SONIC_RX_SYNCING,         // preamble candidate found, refining timing
    SONIC_RX_HEADER,          // receiving the 9-byte header
    SONIC_RX_BODY,            // receiving the body; progress is meaningful
} sonic_rx_state_t;

// Event bits returned by sonic_rx_process().
#define SONIC_RX_EV_SYNC   (1u << 0)   // preamble locked, header reception started
#define SONIC_RX_EV_HEADER (1u << 1)   // header accepted, body reception started
#define SONIC_RX_EV_FRAME  (1u << 2)   // payload decoded and CRC-checked
#define SONIC_RX_EV_ERROR  (1u << 3)   // header/body rejected; searching again

typedef enum {
    SONIC_RX_ERR_NONE = 0,
    SONIC_RX_ERR_HEADER,      // header uncorrectable or not a supported frame
    SONIC_RX_ERR_TOO_LONG,    // header valid but longer than the configured limit
    SONIC_RX_ERR_BODY,        // body uncorrectable
    SONIC_RX_ERR_CRC,         // body corrected but payload CRC mismatch
} sonic_rx_error_t;

// Receiver state. Treat as opaque; it is public only so callers can place it
// statically (about 3 KB) instead of allocating it.
typedef struct {
    int16_t ring[SONIC_WINDOW];
    uint16_t ring_pos;
    uint16_t hop_fill;
    uint32_t frame;
    float pre_acc[256];
    float sync_scores[24];
    uint8_t sync_count;
    uint32_t sync_first;
    uint32_t data_start;
    float last_sync_score;
    sonic_rx_state_t state;
    float sym_acc[SONIC_DIGITS];
    uint16_t sym_index;
    uint8_t hi_digit;
    uint8_t hi_conf;
    bool have_hi;
    uint16_t byte_count;
    uint16_t body_total;
    uint8_t body_len;
    uint8_t body_parity;
    uint16_t max_payload;
    uint8_t bytes[SONIC_HEADER_BYTES + SONIC_CODEWORD_MAX];
    uint8_t conf[SONIC_HEADER_BYTES + SONIC_CODEWORD_MAX];
    uint8_t payload[SONIC_CODEWORD_MAX];
    uint16_t payload_len;
    sonic_rx_error_t last_error;
    uint32_t frames_ok;
    uint32_t frames_failed;
} sonic_rx_t;

// Initialise tables and state. Call from one task before first use.
void sonic_rx_init(sonic_rx_t *rx);

// Forget any partial frame and the analysis history (keeps configuration).
void sonic_rx_reset(sonic_rx_t *rx);

// Reject frames whose payload exceeds max_payload bytes right after the header
// (default SONIC_PAYLOAD_MAX). Limits time spent on a corrupted header.
void sonic_rx_set_max_payload(sonic_rx_t *rx, uint16_t max_payload);

// Feed consecutive 16 kHz mono samples. Returns the OR of SONIC_RX_EV_* bits
// raised while processing them. Deterministic and non-blocking; CPU cost is
// roughly 32 Goertzel filters over 256 samples every 64 input samples.
uint32_t sonic_rx_process(sonic_rx_t *rx, const int16_t *pcm, size_t count);

sonic_rx_state_t sonic_rx_state(const sonic_rx_t *rx);

// Body reception progress in permille (0 unless state is SONIC_RX_BODY).
uint16_t sonic_rx_progress(const sonic_rx_t *rx);

sonic_rx_error_t sonic_rx_last_error(const sonic_rx_t *rx);

// Last decoded payload (valid after SONIC_RX_EV_FRAME until the next frame or
// sonic_rx_clear_payload()). Returns NULL when none is stored.
const uint8_t *sonic_rx_payload(const sonic_rx_t *rx, size_t *len);

// Wipe the stored payload (it may contain secrets such as passwords).
void sonic_rx_clear_payload(sonic_rx_t *rx);
