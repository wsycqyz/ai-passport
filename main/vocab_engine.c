// main/vocab_engine.c - pure top-level composition (see vocab_engine.h).
#include "vocab_engine.h"

#include <string.h>

#include "vocab_config.h"

static void push_value(vocab_outbox_t *out, vocab_cmd_kind_t kind, uint32_t value)
{
    vocab_outbox_push(out, kind, VOCAB_CUE_NONE, VOCAB_TIMER_SESSION, VOCAB_UID_NONE, value);
}

static bool screen_off(const vocab_engine_t *eng)
{
    return eng->power == VOCAB_POWER_OFF || eng->power == VOCAB_POWER_SLEEP;
}

// Applies NEW_EPOCH commands appended since `from` to the input filter. They stay in the
// outbox so the app also drains its key queue.
static void apply_epochs(vocab_engine_t *eng, const vocab_outbox_t *out, uint8_t from,
                         vocab_ms_t now)
{
    for (uint8_t i = from; i < out->count; i++) {
        if (out->cmd[i].kind == VOCAB_CMD_NEW_EPOCH) vocab_input_new_epoch(&eng->input, now);
    }
}

void vocab_engine_init(vocab_engine_t *eng, const vocab_engine_config_t *cfg, vocab_ms_t now,
                       vocab_view_t *view, vocab_outbox_t *out)
{
    memset(eng, 0, sizeof(*eng));
    vocab_rng_seed(&eng->rng, cfg->rng_seed);
    vocab_session_config_t scfg = cfg->session;
    scfg.rng = &eng->rng;
    vocab_input_init(&eng->input, now);  // keys pressed during boot never act (R3)
    vocab_idle_init(&eng->idle, now);
    eng->power = VOCAB_POWER_ACTIVE;
    eng->battery_due = now + VOCAB_BATTERY_POLL_MS;
    const uint8_t mark = out->count;
    vocab_session_init(&eng->session, &scfg, now, view, out);
    apply_epochs(eng, out, mark, now);
    push_value(out, VOCAB_CMD_BACKLIGHT, VOCAB_BACKLIGHT_ON);
    push_value(out, VOCAB_CMD_READ_BATTERY, 0);
}

static void wake(vocab_engine_t *eng, vocab_ms_t now, vocab_outbox_t *out)
{
    // Same screen and state; new epoch; fresh battery reading (spec 9).
    eng->power = VOCAB_POWER_ACTIVE;
    vocab_input_new_epoch(&eng->input, now);
    push_value(out, VOCAB_CMD_BACKLIGHT, VOCAB_BACKLIGHT_ON);
    push_value(out, VOCAB_CMD_READ_BATTERY, 0);
    vocab_out_new_epoch(out);
    eng->battery_due = now + VOCAB_BATTERY_POLL_MS;
}

void vocab_engine_key(vocab_engine_t *eng, const vocab_raw_event_t *raw, vocab_ms_t now,
                      vocab_view_t *view, vocab_outbox_t *out)
{
    const bool off = screen_off(eng);
    const vocab_input_result_t res =
        vocab_input_feed(&eng->input, raw, vocab_session_locked(&eng->session), off);
    if (res.activity) {
        vocab_idle_activity(&eng->idle, now);
        if (off) {
            wake(eng, now, out);
            return;
        }
        if (eng->power == VOCAB_POWER_DIM) push_value(out, VOCAB_CMD_BACKLIGHT, VOCAB_BACKLIGHT_ON);
        eng->power = VOCAB_POWER_ACTIVE;
    }
    if (!res.intent || off) return;
    const uint8_t mark = out->count;
    vocab_session_handle(&eng->session, &res.event, now, view, out);
    apply_epochs(eng, out, mark, now);
}

void vocab_engine_key_drained(vocab_engine_t *eng, const vocab_raw_event_t *raw, vocab_ms_t now)
{
    const vocab_input_result_t res = vocab_input_drain(&eng->input, raw);
    if (res.activity && !screen_off(eng)) {
        vocab_idle_activity(&eng->idle, now);
        eng->power = VOCAB_POWER_ACTIVE;
    }
}

void vocab_engine_poll(vocab_engine_t *eng, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    const uint8_t mark = out->count;
    vocab_session_poll(&eng->session, now, view, out);
    apply_epochs(eng, out, mark, now);

    const vocab_power_t before = eng->power;
    const vocab_power_t after = vocab_idle_update(&eng->idle, now);
    if (after != before) {
        eng->power = after;
        switch (after) {
        case VOCAB_POWER_DIM:
            push_value(out, VOCAB_CMD_BACKLIGHT, VOCAB_BACKLIGHT_DIM);
            break;
        case VOCAB_POWER_OFF:
            // Stop audio, wait for idle and save (the app's save gate), then dark.
            vocab_out_stop_audio(out);
            push_value(out, VOCAB_CMD_SAVE, VOCAB_SAVE_POINT);
            push_value(out, VOCAB_CMD_BACKLIGHT, 0);
            break;
        case VOCAB_POWER_SLEEP:
            push_value(out, VOCAB_CMD_SAVE, VOCAB_SAVE_POINT);
            push_value(out, VOCAB_CMD_DEEP_SLEEP, 0);
            break;
        case VOCAB_POWER_ACTIVE:
        default:
            break;
        }
    }
    if (!screen_off(eng) && vocab_ms_reached(now, eng->battery_due)) {
        eng->battery_due = now + VOCAB_BATTERY_POLL_MS;
        push_value(out, VOCAB_CMD_READ_BATTERY, 0);
    }
}

bool vocab_engine_next_deadline(const vocab_engine_t *eng, vocab_ms_t *deadline)
{
    bool found = false;
    vocab_ms_t best = 0;
    vocab_ms_t t;
    if (vocab_session_next_deadline(&eng->session, &t)) {
        best = t;
        found = true;
    }
    if (vocab_idle_next_deadline(&eng->idle, &t) && (!found || (int32_t)(t - best) < 0)) {
        best = t;
        found = true;
    }
    if (!screen_off(eng) && (!found || (int32_t)(eng->battery_due - best) < 0)) {
        best = eng->battery_due;
        found = true;
    }
    if (found && deadline) *deadline = best;
    return found;
}

void vocab_engine_battery(vocab_engine_t *eng, int pct, vocab_view_t *view)
{
    vocab_session_set_battery(&eng->session, pct, view);
}

void vocab_engine_storage_warning(vocab_engine_t *eng, bool warning, vocab_view_t *view)
{
    vocab_session_set_storage_warning(&eng->session, warning, view);
}

void vocab_engine_sleep_refused(vocab_engine_t *eng, vocab_ms_t now)
{
    vocab_idle_sleep_refused(&eng->idle, now);
    eng->power = VOCAB_POWER_OFF;
}

vocab_power_t vocab_engine_power(const vocab_engine_t *eng)
{
    return eng->power;
}

vocab_session_persist_t vocab_engine_persist(const vocab_engine_t *eng)
{
    return vocab_session_persist(&eng->session);
}
