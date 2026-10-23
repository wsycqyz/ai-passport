// tests/test_vocab_qa_fuzz.c - QA robustness: key mashing / malformed event fuzz with
// system invariants, and an English-safety sweep of every bank entry through every stage
// format and outcome, muted and unmuted.
//
// - AC-32: >= 100,000 raw events (child-like mashing bursts, overlapping keys, holds,
//   doubles, and BSP-impossible sequences: CLICK without PRESS, duplicate RELEASE, LONG
//   after RELEASE, invalid key/kind values) across every state, with audio on, muted,
//   unavailable and clips missing, low battery, failing NVS writes, busy audio, deep
//   sleeps and power cycles. After every step the harness checks the view (AC-27), the
//   queue bound, mastery range, unseen records untouched, tick monotonic, backlight vs.
//   power state, volume/battery icons, HOME lines, that no audio is requested while
//   muted/off, that every deadline lies in the future (no busy loop) and that every
//   reboot restores the last saved state; liveness: HOME is always reachable.
// - AC-27: every entry is forced through Stage 1, Stage 2 (word and cloze-sentence front),
//   Stage 3 (or its Stage 2 fallback), with every ending (correct, wrong + hold, forgot +
//   TEST_LATER, too easy + check pass/fail, second try, reveal), at 80 % and muted; the
//   scanner verifies every English string against the current card's bank data and that
//   no wrong-chunk spelling ever appears.
//
// Owner: QA engineer.
#include "test_vocab_qa_harness.h"

static vocab_rng_t F;

static uint32_t fr(uint32_t n)
{
    return vocab_rng_below(&F, n);
}

// ---------------------------------------------------------------------------------------
// AC-32: mashing fuzz
// ---------------------------------------------------------------------------------------

typedef struct {
    const char *name;
    bool audio_ok;
    bool clips;
    uint8_t volume_index;
    unsigned events;
} fuzz_cfg_t;

static void mash_burst(void)
{
    // A child hammering keys: 3..12 taps, 15..90 ms apart, presses of 10..200 ms, keys mixed.
    const unsigned n = 3 + fr(10);
    vocab_ms_t t = D.now + 1;
    for (unsigned i = 0; i < n; i++) {
        const vocab_key_t k = (vocab_key_t)fr(3);
        const uint32_t len = 10 + fr(190);
        qa_push(k, VOCAB_RAW_PRESS, t);
        qa_push(k, VOCAB_RAW_RELEASE, t + len);
        if (fr(4) == 0) {
            qa_push(k, VOCAB_RAW_DOUBLE, t + len + 180);
        } else {
            qa_push(k, VOCAB_RAW_CLICK, t + len + 180);
        }
        t += 15 + fr(75);
    }
    qa_run_until(t + 400);
}

static void raw_garbage(void)
{
    // BSP-impossible sequences (contact bounce, lost events, corrupted values).
    vocab_ms_t t = D.now + 1;
    const unsigned n = 2 + fr(8);
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pick = fr(20);
        const vocab_key_t k = pick == 0 ? (vocab_key_t)(3 + fr(5)) : (vocab_key_t)fr(3);
        const vocab_raw_kind_t kind = pick == 1 ? (vocab_raw_kind_t)(5 + fr(5)) : (vocab_raw_kind_t)fr(5);
        qa_push(k, kind, t);
        t += fr(60);
    }
    qa_run_until(t + 1);
}

static void sensible_answer(void)
{
    qa_settle();
    if (D.view.screen != VOCAB_SCREEN_CARD) return;
    qa_wait(300);
    if (qa_s()->stage_format == VOCAB_FORMAT_S2) {
        qa_wait(VOCAB_S2_FRONT_MIN_MS);
        qa_tap(VOCAB_KEY_OK, 80);
        qa_wait(300);
        qa_tap(fr(3) ? VOCAB_KEY_UP : VOCAB_KEY_DOWN, 80);
    } else {
        const vocab_key_t right = qa_correct_key();
        qa_tap(fr(3) ? right : qa_other_key(right), 80);
    }
}

static unsigned fuzz(uint32_t seed, const fuzz_cfg_t *cfg)
{
    qa_power_on_fresh(seed);
    vocab_rng_seed(&F, seed ^ 0xA5A5u);
    D.audio_ok = cfg->audio_ok;
    D.clips = cfg->clips;
    // A mid-progress device so every stage appears.
    static vocab_record_t recs[VOCAB_RECORDS_MAX];
    qa_fresh_records(recs);
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        const uint8_t m = (uint8_t)fr(6);
        if (m == 0 && fr(2)) continue;  // some words stay new
        recs[i].mastery = m;
        recs[i].flags = VOCAB_REC_SEEN | (m == 5 ? VOCAB_REC_EVER_MASTERED : 0);
        recs[i].due = (vocab_tick_t)(fr(8));
    }
    qa_seed_storage(recs, 4, 80, cfg->volume_index);
    qa_power_cycle(500);
    const unsigned start = D.raw;
    unsigned actions = 0;
    while (D.raw - start < cfg->events && vocab_test_failures < 20) {
        const uint32_t pick = fr(1000);
        const vocab_key_t key = (vocab_key_t)fr(3);
        if (pick < 300) {
            mash_burst();
        } else if (pick < 450) {
            qa_tap(key, 10 + fr(400));
        } else if (pick < 520) {
            qa_hold(key, 500 + fr(2500));
        } else if (pick < 560) {
            if (fr(2)) {
                qa_double(key);
            } else {
                // A child's double tap: 70..130 ms taps, the second press within 180 ms of
                // the first release; the BSP reports one DOUBLE ~185 ms after the second release.
                const vocab_ms_t t = D.now + 1;
                const uint32_t a = 70 + fr(60), gap = 40 + fr(140), b = 70 + fr(60);
                qa_push(key, VOCAB_RAW_PRESS, t);
                qa_push(key, VOCAB_RAW_RELEASE, t + a);
                qa_push(key, VOCAB_RAW_PRESS, t + a + gap);
                qa_push(key, VOCAB_RAW_RELEASE, t + a + gap + b);
                qa_push(key, VOCAB_RAW_DOUBLE, t + a + gap + b + 185);
                qa_run_until(t + a + gap + b + 186);
            }
        } else if (pick < 600) {
            raw_garbage();
        } else if (pick < 640) {
            // Cross-key phantom during a long hold.
            const vocab_key_t k2 = (vocab_key_t)((key + 1u + fr(2)) % 3u);
            const vocab_ms_t t = D.now + 1;
            qa_push(key, VOCAB_RAW_PRESS, t);
            qa_push(k2, VOCAB_RAW_PRESS, t + 200 + fr(250));
            qa_push(k2, VOCAB_RAW_RELEASE, t + 460);
            qa_push(k2, VOCAB_RAW_CLICK, t + 640);
            qa_push(key, VOCAB_RAW_LONG, t + 500);
            qa_push(key, VOCAB_RAW_RELEASE, t + 700 + fr(800));
            qa_run_until(t + 1600);
        } else if (pick < 820) {
            qa_wait(fr(2500));
        } else if (pick < 930) {
            sensible_answer();
        } else if (pick < 945) {
            D.battery = (int)fr(103) - 1;
            qa_wait(VOCAB_BATTERY_POLL_MS / 2);
        } else if (pick < 955) {
            // Storage trouble: one failing write or a stuck audio worker for a while.
            D.check_restore = false;
            if (fr(2)) {
                D.fail_set_at = D.kv_sets + 1u + fr(3);
            } else {
                D.audio_stuck = !D.audio_stuck;
            }
        } else if (pick < 975) {
            qa_wait(VOCAB_IDLE_OFF_MS + fr(3000));                                  // screen off
        } else if (pick < 990) {
            qa_wait(VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + fr(2000));  // deep sleep
        } else {
            qa_power_cycle(1000 + fr(5000));                                        // battery pulled
        }
        if (++actions % 400 == 0) {
            D.fail_set_at = 0;
            D.audio_stuck = false;
            qa_recover_to_home();
            // Make storage trustworthy again before re-enabling the round-trip check.
            qa_wait(VOCAB_IDLE_OFF_MS + 200);
            qa_tap(VOCAB_KEY_UP, 80);
            qa_wait(500);
            D.check_restore = D.saved_valid;
        }
    }
    D.fail_set_at = 0;
    D.audio_stuck = false;
    qa_recover_to_home();
    const unsigned need = (1u << VOCAB_SCREEN_HOME) | (1u << VOCAB_SCREEN_CARD) | (1u << VOCAB_SCREEN_PAUSE) |
                          (1u << VOCAB_SCREEN_SUMMARY) | (1u << VOCAB_SCREEN_BANNER);
    CHECK((D.screens_seen & need) == need, "fuzz '%s' reached every screen (0x%x)", cfg->name, D.screens_seen);
    const unsigned fmts = (1u << VOCAB_FORMAT_S1) | (1u << VOCAB_FORMAT_S2) | (1u << VOCAB_FORMAT_S3);
    CHECK((D.formats_seen & fmts) == fmts, "fuzz '%s' ran every stage (0x%x)", cfg->name, D.formats_seen);
    printf("QA fuzz '%s' seed %u: %u raw events, %u steps, %u boots (restores checked %u), %u saves "
           "(%u ok, %u deferred, %u failed), %u deep sleeps, %u refused, %u clips, %u cues, screens 0x%x\n",
           cfg->name, (unsigned)seed, D.raw - start, D.steps, D.boots, D.restores_checked, D.saves, D.saves_ok,
           D.saves_deferred, D.saves_failed, D.sleeps, D.refused, D.words, D.cues, D.screens_seen);
    return D.raw - start;
}

// ---------------------------------------------------------------------------------------
// AC-27: every entry through every format and ending
// ---------------------------------------------------------------------------------------

typedef enum { V_GOOD = 0, V_BAD, V_TOO_EASY_PASS, V_TOO_EASY_FAIL, V_SECOND, V_COUNT } variant_t;

static vocab_index_t TARGET;
static unsigned sweep_cards, sweep_target_cards, sweep_fmt[4], sweep_cloze_fronts;

static void play_current(variant_t v)
{
    const vocab_session_t *s = qa_s();
    const uint8_t cur = s->cur;
    const vocab_card_t card = s->cards[cur];
    const bool target = card.index == TARGET;
    variant_t mine = target ? v : V_GOOD;
    if (target && card.purpose != VOCAB_PURPOSE_LEARN) mine = v == V_TOO_EASY_FAIL || v == V_BAD ? V_BAD : V_GOOD;
    sweep_cards++;
    sweep_target_cards += target;
    sweep_fmt[s->stage_format]++;
    if (s->stage_format == VOCAB_FORMAT_S2 && D.view.card.sentence.visible) sweep_cloze_fronts++;
    const bool learn = card.purpose == VOCAB_PURPOSE_LEARN;
    qa_wait(300);
    switch (s->stage_format) {
    case VOCAB_FORMAT_S1:
        if (learn && (mine == V_TOO_EASY_PASS || mine == V_TOO_EASY_FAIL)) {
            qa_hold(VOCAB_KEY_UP, 700);
            break;
        }
        qa_tap(VOCAB_KEY_OK, 80);  // replay / pulse
        qa_wait(300);
        if (mine == V_GOOD || mine == V_SECOND) {
            qa_tap(qa_correct_key(), 80);
        } else {
            const vocab_key_t right = qa_correct_key();
            qa_tap(qa_other_key(right), 80);
            qa_wait(VOCAB_ERROR_HOLD_MS + 300);
            qa_tap(qa_other_key(right), 80);  // nudge
            qa_wait(300);
            qa_tap(VOCAB_KEY_OK, 80);
            qa_wait(300);
            qa_tap(right, 80);
        }
        break;
    case VOCAB_FORMAT_S2:
        if (learn && (mine == V_TOO_EASY_PASS || mine == V_TOO_EASY_FAIL)) {
            qa_hold(VOCAB_KEY_UP, 700);
            break;
        }
        qa_wait(VOCAB_S2_FRONT_MIN_MS);
        qa_tap(VOCAB_KEY_OK, 80);
        qa_wait(300);
        qa_tap(VOCAB_KEY_OK, 80);  // replay on the back
        qa_wait(300);
        qa_tap(mine == V_BAD ? VOCAB_KEY_DOWN : VOCAB_KEY_UP, 80);
        break;
    case VOCAB_FORMAT_S3: {
        const vocab_key_t right = qa_correct_key();
        const vocab_key_t wrong = qa_other_key(right);
        qa_tap(VOCAB_KEY_OK, 80);
        qa_wait(300);
        if (mine == V_GOOD || mine == V_TOO_EASY_PASS) {
            qa_tap(right, 80);
            break;
        }
        qa_tap(wrong, 80);
        if (learn) {
            qa_wait(VOCAB_S3_RETRY_LOCK_MS + 300);
            qa_tap(mine == V_SECOND ? right : wrong, 80);
            if (mine == V_SECOND) break;
        }
        qa_wait(VOCAB_ERROR_HOLD_MS + 300);
        qa_tap(wrong, 80);
        qa_wait(300);
        qa_tap(right, 80);
        break;
    }
    case VOCAB_FORMAT_NONE:
    default:
        CHECK(false, "card without a format");
        break;
    }
    qa_settle();
}

static void sweep_one(vocab_index_t target, uint8_t mastery, variant_t v, uint8_t volume_index, bool audio_ok)
{
    qa_power_on_fresh(500u + target * 7u + mastery * 3u + (unsigned)v);
    D.audio_ok = audio_ok;
    static vocab_record_t recs[VOCAB_RECORDS_MAX];
    qa_fresh_records(recs);
    // Everything else seen and not yet due (the session fills up with early reviews of
    // mixed mastery, so all formats appear around the target).
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        recs[i].flags = VOCAB_REC_SEEN;
        recs[i].mastery = (uint8_t)(1 + (i * 7u + mastery) % 5u);
        recs[i].due = (vocab_tick_t)(21 + (i * 5u) % 9u);
        if (recs[i].mastery == 5) recs[i].flags |= VOCAB_REC_EVER_MASTERED;
    }
    recs[target].mastery = mastery;
    recs[target].due = 18;
    recs[target].flags = VOCAB_REC_SEEN;
    qa_seed_storage(recs, 20, 120, volume_index);
    qa_power_cycle(500);
    TARGET = target;
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    for (int guard = 0; guard < 20 && D.view.screen != VOCAB_SCREEN_SUMMARY && vocab_test_failures < 20; guard++) {
        qa_settle();
        if (D.view.screen == VOCAB_SCREEN_CARD) {
            play_current(v);
        } else if (D.view.screen != VOCAB_SCREEN_SUMMARY) {
            qa_wait(100);
        }
    }
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "sweep session for '%s' ended", D.bank->entries[target].text);
}

static void english_sweep(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned sessions = 0;
    for (vocab_index_t i = 0; i < bank->count && vocab_test_failures < 20; i++) {
        for (uint8_t m = 0; m <= 3; m++) {
            for (int v = 0; v < V_COUNT; v++) {
                if (m < 3 && v == V_SECOND) continue;   // Stage 3 only
                if (m >= 3 && (v == V_TOO_EASY_PASS || v == V_TOO_EASY_FAIL)) continue;
                sweep_one(i, m, (variant_t)v, 0, true);  // 80 %
                sweep_one(i, m, (variant_t)v, 2, true);  // muted
                sessions += 2;
            }
        }
        sweep_one(i, 2, V_GOOD, 0, false);  // audio unavailable
        sessions++;
    }
    CHECK(sweep_fmt[VOCAB_FORMAT_S1] > 0 && sweep_fmt[VOCAB_FORMAT_S2] > 0 && sweep_fmt[VOCAB_FORMAT_S3] > 0 &&
              sweep_cloze_fronts > 0, "every format appeared");
    printf("QA English-safety sweep: %u sessions, %u cards (%u of the target word): S1 %u, S2 %u (cloze fronts %u), "
           "S3 %u; every view scanned (AC-27)\n", sessions, sweep_cards, sweep_target_cards,
           sweep_fmt[VOCAB_FORMAT_S1], sweep_fmt[VOCAB_FORMAT_S2], sweep_cloze_fronts, sweep_fmt[VOCAB_FORMAT_S3]);
}

int main(void)
{
    static const fuzz_cfg_t cfgs[] = {
        { "audio 80%", true, true, 0, 40000 },
        { "muted", true, true, 2, 25000 },
        { "clips missing", true, false, 1, 20000 },
        { "audio unavailable", false, false, 0, 20000 },
    };
    unsigned total = 0;
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) total += fuzz(900u + (uint32_t)i, &cfgs[i]);
    CHECK(total >= 100000u, "AC-32: at least 100,000 raw events (got %u)", total);
    english_sweep();
    qa_report_known_defects();
    return vocab_test_report("vocab QA fuzz and English-safety tests");
}
