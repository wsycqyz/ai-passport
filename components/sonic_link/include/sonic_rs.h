// components/sonic_link/include/sonic_rs.h
// Reed-Solomon codec over GF(2^8) used by the SonicLink data-over-sound protocol.
//
// Code parameters (fixed; the PC encoder in tools/sonic_link.py must match):
//   field polynomial 0x11D, generator element alpha = 2, first consecutive root
//   alpha^0 (fcr = 0), systematic codewords "message bytes || parity bytes".
// The first codeword byte is the highest-degree coefficient.
//
// Pure C99, no dynamic allocation, no ESP-IDF dependency. Functions are
// reentrant after sonic_rs_init(); decoding keeps its work arrays on the stack
// (about 0.6 KB for the maximum parity).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SONIC_RS_MAX_CODEWORD 255
#define SONIC_RS_MAX_PARITY   64

// Build the shared GF(256) tables. Idempotent; call once from a single task
// before concurrent use (sonic_rx_init() calls it).
void sonic_rs_init(void);

// Compute nsym parity bytes for msg[0..msg_len). Requires
// 1 <= nsym <= SONIC_RS_MAX_PARITY and msg_len + nsym <= 255.
// Returns 0 on success, -1 for invalid arguments.
int sonic_rs_encode(const uint8_t *msg, size_t msg_len, uint8_t nsym, uint8_t *parity);

// Correct codeword[0..n) in place (n includes the nsym parity bytes).
// erasures lists known-unreliable byte positions (indices into codeword,
// unique, may be NULL when erasure_count is 0). Decoding succeeds when
// 2 * errors + erasures <= nsym.
// Returns the number of corrected bytes (0 when already valid) or -1 when the
// codeword is uncorrectable. On failure the buffer content is unspecified.
int sonic_rs_decode(uint8_t *codeword, size_t n, uint8_t nsym,
                    const uint8_t *erasures, size_t erasure_count);
