// tests/test_vocab_input.c - input filter rules R1-R8 (spec 3.1, 3.3; AC-28 - AC-31).
//
// Owner: lead developer. Events follow the BSP timing: PRESS, RELEASE, then CLICK about
// 180 ms after the release; LONG fires once at 500 ms of hold.
#include "vocab_config.h"
#include "vocab_input.h"
#include "vocab_test.h"

static vocab_input_t in;

static vocab_input_result_t ev(vocab_key_t key, vocab_raw_kind_t kind, vocab_ms_t t, bool locked,
                               bool screen_off)
{
    const vocab_raw_event_t raw = { key, kind, t };
    return vocab_input_feed(&in, &raw, locked, screen_off);
}

// A complete tap of `ms` duration starting at t; returns the CLICK result.
static vocab_input_result_t tap(vocab_key_t key, vocab_ms_t t, uint32_t ms, bool locked)
{
    ev(key, VOCAB_RAW_PRESS, t, locked, false);
    ev(key, VOCAB_RAW_RELEASE, t + ms, locked, false);
    return ev(key, VOCAB_RAW_CLICK, t + ms + 180, locked, false);
}

static void test_r1_normalize(void)
{
    vocab_input_init(&in, 0);
    vocab_input_result_t r = ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 1000, false, false);
    CHECK(r.activity && !r.intent, "PRESS is activity only");
    r = ev(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 1080, false, false);
    CHECK(!r.intent && !r.activity, "RELEASE never acts (AC-28)");
    ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 1150, false, false);
    ev(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 1230, false, false);
    r = ev(VOCAB_KEY_UP, VOCAB_RAW_DOUBLE, 1410, false, false);
    CHECK(r.intent && r.event.kind == VOCAB_EV_CLICK && r.event.key == VOCAB_KEY_UP,
          "DOUBLE counts as one CLICK (AC-28)");
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 2000, false, false);
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_LONG, 2500, false, false);
    CHECK(r.intent && r.event.kind == VOCAB_EV_LONG && r.event.key == VOCAB_KEY_OK, "LONG intent");
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 2900, false, false);
    CHECK(!r.intent, "release after LONG does nothing");
}

static void test_r2_wake(void)
{
    // Short press while the screen is off: the wake press and its CLICK are swallowed.
    vocab_input_init(&in, 0);
    vocab_input_result_t r = ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 1000, false, true);
    CHECK(r.activity && r.wake && !r.intent, "first PRESS only wakes");
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 1080, false, false);
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_CLICK, 1260, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "delayed CLICK of the wake press swallowed (AC-30)");
    // A new press 450 ms after the release acts.
    r = tap(VOCAB_KEY_OK, 1530, 80, false);
    CHECK(r.intent, "a new press 450 ms after the wake release acts (AC-30)");

    // 1 s hold while off: its LONG is swallowed too.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 5000, false, true);
    r = ev(VOCAB_KEY_DOWN, VOCAB_RAW_LONG, 5500, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "LONG of the wake press swallowed (AC-30)");
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, 6000, false, false);
    r = tap(VOCAB_KEY_UP, 6100, 80, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "other keys swallowed during the tail");
    r = tap(VOCAB_KEY_UP, 6500, 80, false);
    CHECK(r.intent, "tail over: keys act");

    // Lost RELEASE: the guard disarms after WAKE_GUARD_MAX_MS.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 100, false, true);
    r = tap(VOCAB_KEY_OK, 100 + VOCAB_WAKE_GUARD_MAX_MS, 80, false);
    CHECK(r.intent, "wake guard safety disarm");

    // A later wake re-arms the guard even if the previous one was never consumed.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 1000, false, true);
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 1080, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 40000, false, true);
    r = ev(VOCAB_KEY_DOWN, VOCAB_RAW_LONG, 40500, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "second wake swallows its LONG");
}

static void test_r3_r4_epoch(void)
{
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 1000, false, false);
    vocab_input_new_epoch(&in, 1050);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, 1100, false, false);
    vocab_input_result_t r = ev(VOCAB_KEY_DOWN, VOCAB_RAW_CLICK, 1280, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_EPOCH, "press begun before the epoch never acts (AC-29)");
    r = ev(VOCAB_KEY_UP, VOCAB_RAW_CLICK, 1300, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_EPOCH, "intent without a recorded PRESS dropped");

    r = tap(VOCAB_KEY_UP, 1050 + 249, 80, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_COOLDOWN, "UP within 250 ms of the epoch dropped (AC-29)");
    r = tap(VOCAB_KEY_DOWN, 1050 + 250, 80, false);
    CHECK(r.intent, "DOWN at 250 ms accepted");
    vocab_input_new_epoch(&in, 3000);
    r = tap(VOCAB_KEY_OK, 3010, 80, false);
    CHECK(r.intent, "OK is not subject to the cooldown");
}

static void test_r5_lock(void)
{
    vocab_input_init(&in, 0);
    vocab_input_result_t r = tap(VOCAB_KEY_UP, 1000, 80, true);
    CHECK(!r.intent && r.drop == VOCAB_DROP_LOCKED, "lock drops clicks");
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 2000, true, false);
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_LONG, 2500, true, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_LOCKED, "lock drops OK LONG");
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 2600, true, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 3000, true, false);
    r = ev(VOCAB_KEY_DOWN, VOCAB_RAW_LONG, 3500, true, false);
    CHECK(r.intent && r.event.key == VOCAB_KEY_DOWN, "DOWN LONG passes a lock (AC-33)");
}

static void test_r6_r7_r8(void)
{
    // R6: OK pressed while UP is held (bounce sweep during a long UP hold).
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 1000, false, false);
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 1300, false, false);
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 1310, false, false);
    vocab_input_result_t r = ev(VOCAB_KEY_OK, VOCAB_RAW_CLICK, 1490, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_CROSS_KEY, "OK begun while UP held dropped (AC-31)");
    ev(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 1600, false, false);

    // Stale held flag: a lost RELEASE stops blocking after WAKE_GUARD_MAX_MS.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 1000, false, false);
    r = tap(VOCAB_KEY_OK, 1000 + VOCAB_WAKE_GUARD_MAX_MS, 80, false);
    CHECK(r.intent, "stale held flag ignored");

    // R7: a 29 ms click is dropped, a 30 ms click accepted (amendment A3).
    vocab_input_init(&in, 0);
    r = tap(VOCAB_KEY_OK, 1000, VOCAB_MIN_PRESS_MS - 1, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_SHORT, "short click dropped (AC-31)");
    r = tap(VOCAB_KEY_OK, 2000, VOCAB_MIN_PRESS_MS, false);
    CHECK(r.intent, "30 ms click accepted");

    // R8: re-press within 120 ms after a LONG's release is a bounce.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 1000, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_LONG, 1500, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, 1800, false, false);
    r = tap(VOCAB_KEY_DOWN, 1800 + VOCAB_LONG_REJOIN_MS - 1, 60, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_REJOIN, "rejoin within 120 ms dropped (AC-31)");
    r = tap(VOCAB_KEY_DOWN, 3000, 80, false);
    CHECK(r.intent, "later press accepted");
}

static void test_drain(void)
{
    vocab_input_init(&in, 0);
    const vocab_raw_event_t press = { VOCAB_KEY_UP, VOCAB_RAW_PRESS, 1000 };
    const vocab_raw_event_t release = { VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 1070 };
    const vocab_raw_event_t click = { VOCAB_KEY_UP, VOCAB_RAW_CLICK, 1250 };
    vocab_input_result_t r = vocab_input_drain(&in, &press);
    CHECK(r.activity && !r.intent, "drained PRESS is activity");
    r = vocab_input_drain(&in, &release);
    CHECK(!in.key[VOCAB_KEY_UP].held, "drained RELEASE clears the held flag");
    r = vocab_input_drain(&in, &click);
    CHECK(!r.intent, "drained CLICK never acts");
    r = tap(VOCAB_KEY_OK, 2000, 80, false);
    CHECK(r.intent, "no phantom cross-key block after a drained RELEASE");
}

// A double tap as the button driver reports it: PRESS, RELEASE after `ms1`, PRESS after
// `gap`, RELEASE after `ms2`, DOUBLE 185 ms later. The first PRESS may wake the screen.
static vocab_input_result_t double_tap(vocab_key_t key, vocab_ms_t t, uint32_t ms1, uint32_t gap,
                                       uint32_t ms2, bool first_wakes)
{
    ev(key, VOCAB_RAW_PRESS, t, false, first_wakes);
    ev(key, VOCAB_RAW_RELEASE, t + ms1, false, false);
    ev(key, VOCAB_RAW_PRESS, t + ms1 + gap, false, false);
    ev(key, VOCAB_RAW_RELEASE, t + ms1 + gap + ms2, false, false);
    return ev(key, VOCAB_RAW_DOUBLE, t + ms1 + gap + ms2 + 185, false, false);
}

static void test_double_tap_sequences(void)
{
    // A waking double tap (100 ms taps, 150 ms gap; DOUBLE 535 ms after the first PRESS)
    // only wakes (R2, AC-30): the second tap restarts the wake tail.
    vocab_input_init(&in, 0);
    vocab_input_result_t r = double_tap(VOCAB_KEY_DOWN, 5000, 100, 150, 100, true);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "waking 100/150/100 ms double tap swallowed (AC-30)");
    r = tap(VOCAB_KEY_DOWN, 5000 + 350 + VOCAB_WAKE_TAIL_MS, 80, false);
    CHECK(r.intent, "a new tap after the restarted tail acts");
    // Same with OK and the slowest double tap the driver still merges (gap 180 ms).
    vocab_input_init(&in, 0);
    r = double_tap(VOCAB_KEY_OK, 9000, 170, VOCAB_BTN_SHORT_PRESS_MS, 170, true);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "slow waking double tap swallowed");
    // The wake key tapped once more after the window is a new press, governed by the tail.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 1000, false, true);
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 1100, false, false);
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_CLICK, 1285, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_WAKE, "waking single tap swallowed");
    r = tap(VOCAB_KEY_OK, 1100 + VOCAB_DOUBLE_WINDOW_MS + 20, 100, false);
    CHECK(r.intent, "a separate later tap acts (its CLICK comes after the tail)");

    // A double tap straddling an epoch is dropped (R3, AC-29): first press 30 ms before the
    // card change, second press 175 ms after the first release.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 2000, false, false);
    vocab_input_new_epoch(&in, 2030);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, 2110, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 2285, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, 2395, false, false);
    r = ev(VOCAB_KEY_DOWN, VOCAB_RAW_DOUBLE, 2580, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_EPOCH, "double tap begun before the epoch dropped (AC-29)");
    // OK has no cooldown but the epoch rule still applies to its first press.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 3000, false, false);
    vocab_input_new_epoch(&in, 3010);
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 3090, false, false);
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 3200, false, false);
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 3280, false, false);
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_DOUBLE, 3465, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_EPOCH, "OK double tap straddling an epoch dropped");
    // The cooldown (R4) is measured from the first press of the sequence.
    vocab_input_init(&in, 0);
    vocab_input_new_epoch(&in, 4000);
    r = double_tap(VOCAB_KEY_UP, 4000 + VOCAB_CARD_COOLDOWN_MS - 50, 80, 150, 80, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_COOLDOWN, "reflex double tap inside the cooldown dropped");

    // Normal taps on a settled card still act.
    vocab_input_init(&in, 0);
    vocab_input_new_epoch(&in, 6000);
    r = double_tap(VOCAB_KEY_UP, 6400, 100, 150, 100, false);
    CHECK(r.intent && r.event.kind == VOCAB_EV_CLICK && r.event.key == VOCAB_KEY_UP,
          "a double tap on a settled card is one click");
    r = tap(VOCAB_KEY_DOWN, 7500, 100, false);
    CHECK(r.intent, "a single tap on a settled card acts");
    r = double_tap(VOCAB_KEY_OK, 8500, 60, 60, 60, false);
    CHECK(r.intent, "a fast double tap acts");
    // A CLICK closes the sequence: a press after it starts a new one even inside the window.
    vocab_input_init(&in, 0);
    vocab_input_new_epoch(&in, 1000);
    r = tap(VOCAB_KEY_OK, 1400, 80, false);
    CHECK(r.intent, "first tap acts");
    vocab_input_new_epoch(&in, 1661);                       // the click changed the card
    r = double_tap(VOCAB_KEY_OK, 1480 + VOCAB_BTN_SHORT_PRESS_MS + 20, 80, 100, 80, false);
    CHECK(r.intent, "a double tap after a delivered CLICK starts a new sequence");

    // R6/R7/R8 for DOUBLE use the whole sequence.
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 1000, false, false);          // UP held
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 1100, false, false);          // phantom OK
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 1115, false, false);
    ev(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 1150, false, false);
    ev(VOCAB_KEY_OK, VOCAB_RAW_PRESS, 1200, false, false);          // second OK, nothing held
    ev(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, 1260, false, false);
    r = ev(VOCAB_KEY_OK, VOCAB_RAW_DOUBLE, 1445, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_CROSS_KEY, "a DOUBLE begun as a cross-key phantom is dropped");
    vocab_input_init(&in, 0);
    r = double_tap(VOCAB_KEY_OK, 1000, 90, 100, VOCAB_MIN_PRESS_MS - 10, false);
    CHECK(r.intent, "a DOUBLE with one real press acts (short second press)");
    vocab_input_init(&in, 0);
    r = double_tap(VOCAB_KEY_OK, 1000, 15, 100, 20, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_SHORT, "a DOUBLE of two glitches is dropped");
    vocab_input_init(&in, 0);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, 1000, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_LONG, 1500, false, false);
    ev(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, 1800, false, false);
    r = double_tap(VOCAB_KEY_DOWN, 1850, 30, 60, 40, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_REJOIN, "release bounces after a LONG never form a DOUBLE");
    // A drained first press still counts as the sequence start.
    vocab_input_init(&in, 0);
    const vocab_raw_event_t p1 = { VOCAB_KEY_UP, VOCAB_RAW_PRESS, 5000 };
    vocab_input_drain(&in, &p1);
    vocab_input_new_epoch(&in, 5010);
    ev(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 5100, false, false);
    ev(VOCAB_KEY_UP, VOCAB_RAW_PRESS, 5250, false, false);
    ev(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, 5350, false, false);
    r = ev(VOCAB_KEY_UP, VOCAB_RAW_DOUBLE, 5535, false, false);
    CHECK(!r.intent && r.drop == VOCAB_DROP_EPOCH, "double tap whose first press was drained is dropped");
}

int main(void)
{
    test_r1_normalize();
    test_r2_wake();
    test_r3_r4_epoch();
    test_r5_lock();
    test_r6_r7_r8();
    test_drain();
    test_double_tap_sequences();
    return vocab_test_report("vocab input filter tests");
}
