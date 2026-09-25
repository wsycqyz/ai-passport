// components/sonic_link/src/sonic_rx.c
// Streaming SonicLink receiver.
//
// Pipeline per 64-sample hop (4 ms at 16 kHz):
//   1. Hann-window the latest 256 samples and run 32 integer Goertzel filters
//      (one per tone). Q14 coefficients and 64-bit products keep the state
//      exact enough without an FPU; |state| <= 128 * 32767 / sin(w) < 2^24.
//   2. Preamble correlator: every frame adds the energy fraction of each
//      preamble tone to the accumulator of every candidate start it could
//      belong to, so a candidate's score completes 155 frames later without
//      storing frame history. Fractions use the energy of both banks, which
//      makes the score drop when windows straddle adjacent symbols.
//   3. After the score crosses SYNC_THRESHOLD, candidates keep being scored
//      while the plateau grows; the plateau centre becomes the symbol timing
//      as soon as the lock deadline for that centre arrives.
//   4. Data symbols: energies of the symbol's own bank are summed over the core
//      frames (window fully inside the symbol, skipping the first 8 ms of room
//      echo); the strongest digit wins and (best - second) / best becomes its
//      confidence, later used to pick Reed-Solomon erasures.
#include "sonic_link.h"

#include <math.h>
#include <string.h>

#define SONIC_PI        3.14159265358979323846
#define PRE_RING        256
#define CORE_FIRST      2
#define CORE_LAST       5
#define CORE_COUNT      (CORE_LAST - CORE_FIRST + 1)
#define PRE_TERMS       (SONIC_PREAMBLE_SYMBOLS * CORE_COUNT)
#define PRE_SPAN        ((SONIC_PREAMBLE_SYMBOLS - 1) * SONIC_FRAMES_PER_SYMBOL + CORE_LAST)
#define DATA_OFFSET     (SONIC_PREAMBLE_SYMBOLS * SONIC_FRAMES_PER_SYMBOL)
// Mean preamble-tone energy fraction needed to start a lock. Tuned with the
// host sweep: 0.22 decodes down to about -10 dB wideband SNR while tone-rich
// music-like audio produced no header-stage false alarms (a false sync only
// costs a silent 0.7 s header attempt).
#define SYNC_THRESHOLD  0.22f
// Candidates may be examined up to SYNC_LOOKAHEAD past the chosen one: the
// lock must happen before that candidate's first data core frame.
#define SYNC_LOOKAHEAD  6
#define SYNC_MAX        24
#define SYNC_KEEP       0.90f
// Bytes whose weaker nibble has confidence above this are never erased.
#define ERASE_CONF_MAX  160
#define ERASE_ATTEMPTS  16

_Static_assert((PRE_RING & (PRE_RING - 1)) == 0, "ring must be a power of two");
_Static_assert(PRE_RING > PRE_SPAN, "ring must cover a whole preamble");
_Static_assert(SYNC_MAX <= sizeof(((sonic_rx_t *)0)->sync_scores) / sizeof(float), "sync history");
// Locking when the newest candidate is SYNC_LOOKAHEAD past the chosen one
// happens one frame before the chosen candidate's first data core frame.
_Static_assert(SYNC_LOOKAHEAD + PRE_SPAN < DATA_OFFSET + CORE_FIRST, "late lock");

static int16_t s_hann[SONIC_WINDOW];
static int32_t s_coeff[SONIC_TONES];
static bool s_tables_ready;

static void tables_init(void)
{
    if (s_tables_ready) return;
    for (int n = 0; n < SONIC_WINDOW; n++) {
        const double w = 0.5 - 0.5 * cos(2.0 * SONIC_PI * n / SONIC_WINDOW);
        s_hann[n] = (int16_t)floor(w * 32767.0 + 0.5);
    }
    for (int bank = 0; bank < SONIC_BANKS; bank++) {
        for (int digit = 0; digit < SONIC_DIGITS; digit++) {
            const int bin = sonic_tone_bin(bank, digit);
            const double c = 2.0 * cos(2.0 * SONIC_PI * bin / SONIC_WINDOW);
            s_coeff[bank * SONIC_DIGITS + digit] = (int32_t)floor(c * 16384.0 + 0.5);
        }
    }
    sonic_rs_init();
    s_tables_ready = true;
}

static void clear_frame_state(sonic_rx_t *rx)
{
    rx->sync_count = 0;
    memset(rx->sym_acc, 0, sizeof(rx->sym_acc));
    rx->sym_index = 0;
    rx->have_hi = false;
    rx->hi_digit = 0;
    rx->hi_conf = 0;
    rx->byte_count = 0;
    rx->body_total = 0;
    rx->body_len = 0;
    rx->body_parity = 0;
    // Received bytes can contain credentials; wipe them between frames.
    memset(rx->bytes, 0, sizeof(rx->bytes));
    memset(rx->conf, 0, sizeof(rx->conf));
}

static void restart_search(sonic_rx_t *rx)
{
    clear_frame_state(rx);
    rx->state = SONIC_RX_SEARCHING;
}

void sonic_rx_init(sonic_rx_t *rx)
{
    tables_init();
    memset(rx, 0, sizeof(*rx));
    rx->max_payload = SONIC_PAYLOAD_MAX;
    rx->state = SONIC_RX_SEARCHING;
}

void sonic_rx_reset(sonic_rx_t *rx)
{
    const uint16_t max_payload = rx->max_payload;
    const uint32_t frames_ok = rx->frames_ok;
    const uint32_t frames_failed = rx->frames_failed;
    sonic_rx_init(rx);
    rx->max_payload = max_payload ? max_payload : SONIC_PAYLOAD_MAX;
    rx->frames_ok = frames_ok;
    rx->frames_failed = frames_failed;
}

void sonic_rx_set_max_payload(sonic_rx_t *rx, uint16_t max_payload)
{
    if (max_payload == 0 || max_payload > SONIC_PAYLOAD_MAX) max_payload = SONIC_PAYLOAD_MAX;
    rx->max_payload = max_payload;
}

static void compute_frame(const sonic_rx_t *rx, float energy[SONIC_TONES])
{
    int16_t xw[SONIC_WINDOW];
    // ring_pos is the oldest sample once the ring is full.
    for (int n = 0; n < SONIC_WINDOW; n++) {
        const int32_t s = rx->ring[(rx->ring_pos + n) & (SONIC_WINDOW - 1)];
        xw[n] = (int16_t)((s * s_hann[n]) >> 15);
    }
    for (int t = 0; t < SONIC_TONES; t++) {
        const int32_t c = s_coeff[t];
        int32_t s1 = 0;
        int32_t s2 = 0;
        for (int n = 0; n < SONIC_WINDOW; n++) {
            const int32_t s0 = xw[n] + (int32_t)(((int64_t)c * s1) >> 14) - s2;
            s2 = s1;
            s1 = s0;
        }
        const int64_t cross = (((int64_t)c * s1) >> 14) * (int64_t)s2;
        const int64_t power = (int64_t)s1 * s1 + (int64_t)s2 * s2 - cross;
        energy[t] = power > 0 ? (float)power : 0.0f;
    }
}

static void fail(sonic_rx_t *rx, sonic_rx_error_t error, uint32_t *events)
{
    rx->last_error = error;
    rx->frames_failed++;
    *events |= SONIC_RX_EV_ERROR;
    restart_search(rx);
}

// Centre of the contiguous run of candidates scoring within SYNC_KEEP of the
// best one. The score is flat while every core window lies inside its symbol,
// so the plateau centre is a better timing estimate than the raw maximum.
static int plateau_centre(const sonic_rx_t *rx, float *best_score)
{
    int best = 0;
    for (int k = 1; k < rx->sync_count; k++) {
        if (rx->sync_scores[k] > rx->sync_scores[best]) best = k;
    }
    const float keep = rx->sync_scores[best] * SYNC_KEEP;
    int left = best;
    int right = best;
    while (left > 0 && rx->sync_scores[left - 1] >= keep) left--;
    while (right + 1 < rx->sync_count && rx->sync_scores[right + 1] >= keep) right++;
    *best_score = rx->sync_scores[best];
    return (left + right + 1) / 2;
}

static void lock_timing(sonic_rx_t *rx, int centre, float best_score, uint32_t *events)
{
    clear_frame_state(rx);
    rx->last_sync_score = best_score;
    rx->data_start = rx->sync_first + (uint32_t)centre + DATA_OFFSET;
    rx->state = SONIC_RX_HEADER;
    *events |= SONIC_RX_EV_SYNC;
}

static void track_sync(sonic_rx_t *rx, float score, uint32_t *events)
{
    rx->sync_scores[rx->sync_count++] = score;
    float best_score = 0.0f;
    int centre = plateau_centre(rx, &best_score);
    const int latest = rx->sync_count - 1;
    if (latest - centre < SYNC_LOOKAHEAD && rx->sync_count < SYNC_MAX) return;
    if (latest - centre > SYNC_LOOKAHEAD) centre = latest - SYNC_LOOKAHEAD;
    lock_timing(rx, centre, best_score, events);
}

static void accept_header(sonic_rx_t *rx, uint32_t *events)
{
    uint8_t header[SONIC_HEADER_BYTES];
    memcpy(header, rx->bytes, sizeof(header));
    const int corrected = sonic_rs_decode(header, SONIC_HEADER_BYTES, SONIC_HEADER_PARITY, NULL, 0);
    const unsigned body_len = header[1];
    const unsigned parity = header[2];
    if (corrected < 0 || header[0] != SONIC_VERSION || body_len < SONIC_BODY_MIN ||
        parity < SONIC_PARITY_MIN || parity > SONIC_PARITY_MAX || (parity & 1u) ||
        body_len + parity > SONIC_CODEWORD_MAX) {
        fail(rx, SONIC_RX_ERR_HEADER, events);
        return;
    }
    if (body_len - SONIC_CRC_BYTES > rx->max_payload) {
        fail(rx, SONIC_RX_ERR_TOO_LONG, events);
        return;
    }
    rx->body_len = (uint8_t)body_len;
    rx->body_parity = (uint8_t)parity;
    rx->body_total = (uint16_t)(body_len + parity);
    rx->state = SONIC_RX_BODY;
    *events |= SONIC_RX_EV_HEADER;
}

typedef enum { BODY_RS_FAIL = 0, BODY_CRC_FAIL, BODY_OK } body_result_t;

static body_result_t try_body(const sonic_rx_t *rx, uint8_t *work,
                              const uint8_t *erasures, size_t erasure_count)
{
    memcpy(work, rx->bytes + SONIC_HEADER_BYTES, rx->body_total);
    if (sonic_rs_decode(work, rx->body_total, rx->body_parity, erasures, erasure_count) < 0) {
        return BODY_RS_FAIL;
    }
    const size_t payload_len = (size_t)rx->body_len - SONIC_CRC_BYTES;
    const uint16_t crc = (uint16_t)(((uint16_t)work[payload_len] << 8) | work[payload_len + 1]);
    return crc == sonic_crc16(work, payload_len) ? BODY_OK : BODY_CRC_FAIL;
}

static void finish_body(sonic_rx_t *rx, uint32_t *events)
{
    uint8_t work[SONIC_CODEWORD_MAX];
    body_result_t result = try_body(rx, work, NULL, 0);
    bool crc_mismatch = result == BODY_CRC_FAIL;

    if (result != BODY_OK) {
        // Retry with the least confident bytes marked as erasures. Keep two
        // parity bytes unused so the decoder can still reject inconsistencies.
        uint8_t order[SONIC_CODEWORD_MAX];
        size_t candidates = 0;
        const uint8_t *conf = rx->conf + SONIC_HEADER_BYTES;
        for (size_t i = 0; i < rx->body_total; i++) {
            if (conf[i] <= ERASE_CONF_MAX) order[candidates++] = (uint8_t)i;
        }
        for (size_t i = 1; i < candidates; i++) {
            const uint8_t item = order[i];
            size_t j = i;
            while (j > 0 && conf[order[j - 1]] > conf[item]) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = item;
        }
        size_t limit = rx->body_parity - 2u;
        if (limit > candidates) limit = candidates;
        const size_t step = limit > ERASE_ATTEMPTS ? (limit + ERASE_ATTEMPTS - 1) / ERASE_ATTEMPTS : 1;
        for (size_t k = step; k <= limit && result != BODY_OK; k += step) {
            result = try_body(rx, work, order, k);
            if (result == BODY_CRC_FAIL) crc_mismatch = true;
        }
    }

    if (result == BODY_OK) {
        rx->payload_len = (uint16_t)(rx->body_len - SONIC_CRC_BYTES);
        memcpy(rx->payload, work, rx->payload_len);
        rx->last_error = SONIC_RX_ERR_NONE;
        rx->frames_ok++;
        *events |= SONIC_RX_EV_FRAME;
        restart_search(rx);
    } else {
        fail(rx, crc_mismatch ? SONIC_RX_ERR_CRC : SONIC_RX_ERR_BODY, events);
    }
    memset(work, 0, sizeof(work));
}

static void finish_symbol(sonic_rx_t *rx, uint32_t *events)
{
    int best = 0;
    float first = rx->sym_acc[0];
    float second = 0.0f;
    for (int d = 1; d < SONIC_DIGITS; d++) {
        const float v = rx->sym_acc[d];
        if (v > first) {
            second = first;
            first = v;
            best = d;
        } else if (v > second) {
            second = v;
        }
    }
    uint8_t conf = 0;
    if (first > 0.0f) conf = (uint8_t)(255.0f * (first - second) / first + 0.5f);
    memset(rx->sym_acc, 0, sizeof(rx->sym_acc));
    rx->sym_index++;

    if (!rx->have_hi) {
        rx->hi_digit = (uint8_t)best;
        rx->hi_conf = conf;
        rx->have_hi = true;
        return;
    }
    rx->have_hi = false;
    const uint16_t at = rx->byte_count++;
    rx->bytes[at] = (uint8_t)((rx->hi_digit << 4) | best);
    rx->conf[at] = conf < rx->hi_conf ? conf : rx->hi_conf;

    if (rx->state == SONIC_RX_HEADER && rx->byte_count == SONIC_HEADER_BYTES) {
        accept_header(rx, events);
    } else if (rx->state == SONIC_RX_BODY &&
               rx->byte_count == SONIC_HEADER_BYTES + rx->body_total) {
        finish_body(rx, events);
    }
}

static void receive_frame(sonic_rx_t *rx, const float energy[SONIC_TONES], uint32_t *events)
{
    const int32_t rel = (int32_t)(rx->frame - rx->data_start);
    if (rel < 0) return;
    const uint32_t symbol = (uint32_t)rel / SONIC_FRAMES_PER_SYMBOL;
    const uint32_t phase = (uint32_t)rel % SONIC_FRAMES_PER_SYMBOL;
    if (symbol != rx->sym_index || phase < CORE_FIRST || phase > CORE_LAST) return;
    // Data symbol k is overall symbol 16 + k, so its bank is k % 2.
    const float *bank = &energy[(symbol & 1u) * SONIC_DIGITS];
    for (int d = 0; d < SONIC_DIGITS; d++) rx->sym_acc[d] += bank[d];
    if (phase == CORE_LAST) finish_symbol(rx, events);
}

static void on_frame(sonic_rx_t *rx, const float energy[SONIC_TONES], uint32_t *events)
{
    const uint32_t f = rx->frame;
    float total = 1.0f;
    for (int t = 0; t < SONIC_TONES; t++) total += energy[t];
    const float inv_total = 1.0f / total;
    for (int i = 0; i < SONIC_PREAMBLE_SYMBOLS; i++) {
        const float frac = energy[(i & 1) * SONIC_DIGITS + SONIC_PREAMBLE[i]] * inv_total;
        for (int m = CORE_FIRST; m <= CORE_LAST; m++) {
            const uint32_t candidate = f - (uint32_t)(i * SONIC_FRAMES_PER_SYMBOL + m);
            rx->pre_acc[candidate & (PRE_RING - 1)] += frac;
        }
    }
    // The candidate whose final contribution was just added is now complete.
    const uint32_t done = f - PRE_SPAN;
    const uint32_t slot = done & (PRE_RING - 1);
    const float score = rx->pre_acc[slot] / (float)PRE_TERMS;
    rx->pre_acc[slot] = 0.0f;
    const bool score_valid = f >= PRE_SPAN;

    switch (rx->state) {
    case SONIC_RX_SEARCHING:
        if (score_valid && score >= SYNC_THRESHOLD) {
            rx->state = SONIC_RX_SYNCING;
            rx->sync_first = done;
            rx->sync_scores[0] = score;
            rx->sync_count = 1;
        }
        break;
    case SONIC_RX_SYNCING:
        track_sync(rx, score, events);
        break;
    case SONIC_RX_HEADER:
    case SONIC_RX_BODY:
        receive_frame(rx, energy, events);
        break;
    }
}

uint32_t sonic_rx_process(sonic_rx_t *rx, const int16_t *pcm, size_t count)
{
    uint32_t events = 0;
    float energy[SONIC_TONES];
    for (size_t i = 0; i < count; i++) {
        rx->ring[rx->ring_pos] = pcm[i];
        rx->ring_pos = (uint16_t)((rx->ring_pos + 1) & (SONIC_WINDOW - 1));
        if (++rx->hop_fill < SONIC_HOP) continue;
        rx->hop_fill = 0;
        compute_frame(rx, energy);
        on_frame(rx, energy, &events);
        rx->frame++;
    }
    return events;
}

sonic_rx_state_t sonic_rx_state(const sonic_rx_t *rx)
{
    return rx->state;
}

uint16_t sonic_rx_progress(const sonic_rx_t *rx)
{
    if (rx->state != SONIC_RX_BODY || rx->body_total == 0) return 0;
    const uint32_t header_symbols = 2u * SONIC_HEADER_BYTES;
    const uint32_t done = rx->sym_index > header_symbols ? rx->sym_index - header_symbols : 0;
    const uint32_t total = 2u * rx->body_total;
    return (uint16_t)(done >= total ? 1000u : done * 1000u / total);
}

sonic_rx_error_t sonic_rx_last_error(const sonic_rx_t *rx)
{
    return rx->last_error;
}

const uint8_t *sonic_rx_payload(const sonic_rx_t *rx, size_t *len)
{
    if (len) *len = rx->payload_len;
    return rx->payload_len ? rx->payload : NULL;
}

void sonic_rx_clear_payload(sonic_rx_t *rx)
{
    memset(rx->payload, 0, sizeof(rx->payload));
    rx->payload_len = 0;
}
