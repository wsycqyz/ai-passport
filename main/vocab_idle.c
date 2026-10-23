// main/vocab_idle.c - pure idle / power timeline (see vocab_idle.h).
#include "vocab_idle.h"

#include "vocab_config.h"

void vocab_idle_init(vocab_idle_t *idle, vocab_ms_t now)
{
    idle->state = VOCAB_POWER_ACTIVE;
    idle->last_activity = now;
    idle->sleep_at = now + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS;
}

void vocab_idle_activity(vocab_idle_t *idle, vocab_ms_t now)
{
    vocab_idle_init(idle, now);
}

vocab_power_t vocab_idle_update(vocab_idle_t *idle, vocab_ms_t now)
{
    const vocab_ms_t dim_at = idle->last_activity + VOCAB_IDLE_DIM_MS;
    const vocab_ms_t off_at = idle->last_activity + VOCAB_IDLE_OFF_MS;
    if (idle->state == VOCAB_POWER_ACTIVE && vocab_ms_reached(now, dim_at)) {
        idle->state = VOCAB_POWER_DIM;
    }
    if (idle->state == VOCAB_POWER_DIM && vocab_ms_reached(now, off_at)) {
        idle->state = VOCAB_POWER_OFF;
    }
    if (idle->state == VOCAB_POWER_OFF && vocab_ms_reached(now, idle->sleep_at)) {
        idle->state = VOCAB_POWER_SLEEP;
    }
    return idle->state;
}

void vocab_idle_sleep_refused(vocab_idle_t *idle, vocab_ms_t now)
{
    idle->state = VOCAB_POWER_OFF;
    idle->sleep_at = now + VOCAB_SLEEP_RETRY_MS;
}

bool vocab_idle_next_deadline(const vocab_idle_t *idle, vocab_ms_t *deadline)
{
    vocab_ms_t at;
    switch (idle->state) {
    case VOCAB_POWER_ACTIVE: at = idle->last_activity + VOCAB_IDLE_DIM_MS; break;
    case VOCAB_POWER_DIM: at = idle->last_activity + VOCAB_IDLE_OFF_MS; break;
    case VOCAB_POWER_OFF: at = idle->sleep_at; break;
    case VOCAB_POWER_SLEEP:
    default:
        return false;
    }
    if (deadline) *deadline = at;
    return true;
}
