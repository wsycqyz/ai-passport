// tests/test_vocab_idle.c - idle and power timeline (spec 9; AC-47, AC-48).
//
// Owner: lead developer. The engine-level behavior (backlight, save, wake-only first key,
// deep sleep and reboot) is covered by tests/test_vocab_sim.c.
#include "vocab_config.h"
#include "vocab_idle.h"
#include "vocab_test.h"

int main(void)
{
    vocab_idle_t idle;
    vocab_ms_t next = 0;
    const vocab_ms_t t0 = 1000;

    vocab_idle_init(&idle, t0);
    CHECK(vocab_idle_next_deadline(&idle, &next) && next == t0 + VOCAB_IDLE_DIM_MS, "first deadline: dim");
    CHECK(vocab_idle_update(&idle, t0 + VOCAB_IDLE_DIM_MS - 1) == VOCAB_POWER_ACTIVE, "active before 20 s");
    CHECK(vocab_idle_update(&idle, t0 + VOCAB_IDLE_DIM_MS) == VOCAB_POWER_DIM, "dim at 20 s (AC-47)");
    CHECK(vocab_idle_next_deadline(&idle, &next) && next == t0 + VOCAB_IDLE_OFF_MS, "next: off");
    CHECK(vocab_idle_update(&idle, t0 + VOCAB_IDLE_OFF_MS) == VOCAB_POWER_OFF, "off at 30 s (AC-47)");
    CHECK(vocab_idle_next_deadline(&idle, &next) &&
              next == t0 + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS, "next: deep sleep");
    CHECK(vocab_idle_update(&idle, t0 + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS - 1) ==
              VOCAB_POWER_OFF, "still off just before 150 s");
    CHECK(vocab_idle_update(&idle, t0 + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS) ==
              VOCAB_POWER_SLEEP, "deep sleep 120 s after off (AC-47)");
    CHECK(!vocab_idle_next_deadline(&idle, &next), "nothing scheduled while sleeping");

    // A late update jumps through every stage at once.
    vocab_idle_init(&idle, 0);
    CHECK(vocab_idle_update(&idle, 500000) == VOCAB_POWER_SLEEP, "one late update reaches SLEEP");

    // Key held at the deep-sleep moment: stay off, retry after 5 s (AC-48).
    vocab_idle_sleep_refused(&idle, 600000);
    CHECK(idle.state == VOCAB_POWER_OFF && vocab_idle_next_deadline(&idle, &next) &&
              next == 600000 + VOCAB_SLEEP_RETRY_MS, "retry scheduled");
    CHECK(vocab_idle_update(&idle, 600000 + VOCAB_SLEEP_RETRY_MS - 1) == VOCAB_POWER_OFF, "waiting to retry");
    CHECK(vocab_idle_update(&idle, 600000 + VOCAB_SLEEP_RETRY_MS) == VOCAB_POWER_SLEEP, "retried");

    // Any PRESS restarts the whole timeline (AC-47).
    vocab_idle_activity(&idle, 700000);
    CHECK(idle.state == VOCAB_POWER_ACTIVE && vocab_idle_update(&idle, 700000 + VOCAB_IDLE_DIM_MS - 1) ==
              VOCAB_POWER_ACTIVE, "activity restarts the timeline");
    vocab_idle_update(&idle, 700000 + VOCAB_IDLE_DIM_MS);
    vocab_idle_activity(&idle, 700000 + VOCAB_IDLE_DIM_MS + 5);
    CHECK(idle.state == VOCAB_POWER_ACTIVE, "a key while dim restores full brightness");

    // Wrap-around of the millisecond clock.
    vocab_idle_init(&idle, 0xFFFFFF00u);
    CHECK(vocab_idle_update(&idle, 0xFFFFFF00u + VOCAB_IDLE_DIM_MS) == VOCAB_POWER_DIM, "wrap-safe");
    return vocab_test_report("vocab idle tests");
}
