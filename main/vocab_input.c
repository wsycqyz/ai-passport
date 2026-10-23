// main/vocab_input.c - pure input filter (see vocab_input.h for the rules R1-R9).
#include "vocab_input.h"

#include <string.h>

#include "vocab_config.h"

// Unsigned elapsed time from `from` to `to` (wrap-safe for intervals < 24 days).
static uint32_t elapsed(vocab_ms_t from, vocab_ms_t to)
{
    return (uint32_t)(to - from);
}

static bool earlier(vocab_ms_t a, vocab_ms_t b)
{
    return (int32_t)(a - b) < 0;
}

void vocab_input_init(vocab_input_t *in, vocab_ms_t now)
{
    memset(in, 0, sizeof(*in));
    in->epoch_ms = now;
}

void vocab_input_new_epoch(vocab_input_t *in, vocab_ms_t now)
{
    in->epoch_ms = now;
}

static void on_press(vocab_input_t *in, unsigned k, vocab_ms_t t, bool screen_off,
                     vocab_input_result_t *res)
{
    res->activity = true;
    bool other_held = false;
    for (unsigned j = 0; j < VOCAB_KEY_COUNT; j++) {
        vocab_key_state_t *other = &in->key[j];
        if (j == k || !other->held) continue;
        if (elapsed(other->press_ms, t) >= VOCAB_WAKE_GUARD_MAX_MS) {
            other->held = false;  // a lost RELEASE must not block input forever (R6)
        } else {
            other_held = true;
        }
    }
    vocab_key_state_t *ks = &in->key[k];
    const bool rejoin = ks->has_press && ks->released && ks->long_fired &&
                        elapsed(ks->release_ms, t) < VOCAB_LONG_REJOIN_MS;
    // The driver may merge this press with the previous one into a DOUBLE.
    const bool continues = ks->has_press && ks->released && !ks->long_fired && ks->seq_open &&
                           elapsed(ks->release_ms, t) <= VOCAB_DOUBLE_WINDOW_MS;
    ks->press_ms = t;
    ks->has_press = true;
    ks->held = true;
    ks->released = false;
    ks->long_fired = false;
    ks->cross = other_held;
    ks->rejoin = rejoin;
    if (continues) {
        ks->seq_cross = ks->seq_cross || other_held;
    } else {
        ks->seq_ms = t;
        ks->seq_max_ms = 0;
        ks->seq_cross = other_held;
        ks->seq_rejoin = rejoin;
    }
    ks->seq_open = true;
    if (screen_off) {
        // (Re)arm for this press: an older guard may still be armed if no intent arrived
        // after its tail.
        res->wake = true;
        in->wake_guard = true;
        in->wake_released = false;
        in->wake_key = (vocab_key_t)k;
        in->wake_ms = t;
    } else if (continues && in->wake_guard && in->wake_released && (unsigned)in->wake_key == k) {
        // Second tap of a waking double tap: the tail restarts at this press's RELEASE.
        in->wake_released = false;
    }
}

static void on_release(vocab_input_t *in, unsigned k, vocab_ms_t t)
{
    vocab_key_state_t *ks = &in->key[k];
    if (ks->held) {
        const uint32_t len = elapsed(ks->press_ms, t);
        if (len > ks->seq_max_ms) ks->seq_max_ms = len;
    }
    ks->held = false;
    ks->released = true;
    ks->release_ms = t;
    if (in->wake_guard && !in->wake_released && (unsigned)in->wake_key == k) {
        in->wake_released = true;
        in->wake_release_ms = t;
    }
}

// R2: true while the waking press (including its delayed CLICK/LONG) must be swallowed.
static bool wake_guard_blocks(vocab_input_t *in, vocab_ms_t t)
{
    if (!in->wake_guard) return false;
    if ((in->wake_released && elapsed(in->wake_release_ms, t) >= VOCAB_WAKE_TAIL_MS) ||
        elapsed(in->wake_ms, t) >= VOCAB_WAKE_GUARD_MAX_MS) {
        in->wake_guard = false;
        return false;
    }
    return true;
}

static bool valid(const vocab_raw_event_t *raw)
{
    return raw && (unsigned)raw->key < VOCAB_KEY_COUNT && (unsigned)raw->kind <= VOCAB_RAW_RELEASE;
}

static vocab_input_result_t empty_result(void)
{
    vocab_input_result_t res;
    memset(&res, 0, sizeof(res));
    res.event.kind = VOCAB_EV_NONE;
    return res;
}

vocab_input_result_t vocab_input_feed(vocab_input_t *in, const vocab_raw_event_t *raw,
                                      bool locked, bool screen_off)
{
    vocab_input_result_t res = empty_result();
    if (!valid(raw)) {
        res.drop = VOCAB_DROP_INVALID;
        return res;
    }
    const unsigned k = (unsigned)raw->key;
    const vocab_ms_t t = raw->t_ms;
    vocab_key_state_t *ks = &in->key[k];

    switch (raw->kind) {
    case VOCAB_RAW_PRESS:
        on_press(in, k, t, screen_off, &res);
        return res;
    case VOCAB_RAW_RELEASE:
        on_release(in, k, t);
        return res;
    case VOCAB_RAW_LONG:
        ks->long_fired = true;
        break;
    case VOCAB_RAW_CLICK:   // R1: a DOUBLE is one click
    case VOCAB_RAW_DOUBLE:
    default:
        break;
    }
    ks->seq_open = false;  // the driver reported this sequence: the next PRESS starts anew
    const bool is_long = raw->kind == VOCAB_RAW_LONG;
    // A DOUBLE is judged by its whole press sequence (see vocab_input.h).
    const bool is_double = raw->kind == VOCAB_RAW_DOUBLE;
    const vocab_ms_t start = is_double ? ks->seq_ms : ks->press_ms;
    const bool cross = is_double ? ks->seq_cross : ks->cross;
    const bool rejoin = is_double ? ks->seq_rejoin : ks->rejoin;
    const uint32_t press_len = is_double ? ks->seq_max_ms : elapsed(ks->press_ms, ks->release_ms);

    if (screen_off || wake_guard_blocks(in, t)) {                                  // R2
        res.drop = VOCAB_DROP_WAKE;
    } else if (!ks->has_press || earlier(start, in->epoch_ms)) {                  // R3
        res.drop = VOCAB_DROP_EPOCH;
    } else if (!is_long && raw->key != VOCAB_KEY_OK &&
               elapsed(in->epoch_ms, start) < VOCAB_CARD_COOLDOWN_MS) {           // R4
        res.drop = VOCAB_DROP_COOLDOWN;
    } else if (locked && !(is_long && raw->key == VOCAB_KEY_DOWN)) {             // R5
        res.drop = VOCAB_DROP_LOCKED;
    } else if (cross) {                                                           // R6
        res.drop = VOCAB_DROP_CROSS_KEY;
    } else if (!is_long && ks->released && press_len < VOCAB_MIN_PRESS_MS) {      // R7
        res.drop = VOCAB_DROP_SHORT;
    } else if (rejoin) {                                                          // R8
        res.drop = VOCAB_DROP_REJOIN;
    } else {
        res.intent = true;
        res.event = is_long ? vocab_event_long(raw->key) : vocab_event_click(raw->key);
    }
    return res;
}

vocab_input_result_t vocab_input_drain(vocab_input_t *in, const vocab_raw_event_t *raw)
{
    vocab_input_result_t res = empty_result();
    if (!valid(raw)) {
        res.drop = VOCAB_DROP_INVALID;
        return res;
    }
    const unsigned k = (unsigned)raw->key;
    switch (raw->kind) {
    case VOCAB_RAW_PRESS:
        on_press(in, k, raw->t_ms, false, &res);
        break;
    case VOCAB_RAW_RELEASE:
        on_release(in, k, raw->t_ms);
        break;
    case VOCAB_RAW_LONG:
        in->key[k].long_fired = true;
        in->key[k].seq_open = false;
        res.drop = VOCAB_DROP_EPOCH;
        break;
    case VOCAB_RAW_CLICK:
    case VOCAB_RAW_DOUBLE:
    default:
        in->key[k].seq_open = false;
        res.drop = VOCAB_DROP_EPOCH;
        break;
    }
    return res;
}
