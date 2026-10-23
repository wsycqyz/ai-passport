// tests/test_vocab_qa_flows.c - QA system scenarios with exact timing, through raw keys.
//
// Black-box flows over the whole engine (tests/test_vocab_qa_harness.h), each checking the
// user-visible behavior and the records against the spec:
// - HOME / SUMMARY keys and the 800 ms SUMMARY lock; save before the cheer (AC-4, AC-43);
// - the Stage 1 error hold at its millisecond boundaries, queue flush at both ends, nudge,
//   replay (AC-15) and presses carried over a card change or inside the cooldown (AC-29);
// - Stage 2 front minimum and flip audio (AC-17), Stage 3 retry / reveal (AC-24);
// - spot check pass and fail with the full-streak rollback (AC-19, AC-20);
// - "too easy" on a Stage 1 question and a Stage 2 front, pass and fail, and every place
//   where UP LONG must be ignored (AC-21, AC-22);
// - the pause card: resume keeps the card (also in the error-hold wait part), HOME keeps
//   the outcomes, saves and does not advance the tick (AC-5, AC-10);
// - mute, a missing clip and failed audio init: WORD_PULSE, "look again", no cues, dim
//   mute icon and toast (AC-34); DOWN LONG in every state (AC-33); volume persisted only at
//   the next save point (AC-43);
// - DOUBLE = one click, cross-key phantom, 29/30 ms presses, LONG rejoin (AC-28, AC-31).
//
// Owner: QA engineer.
#include "test_vocab_qa_harness.h"

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

static vocab_record_t R[VOCAB_RECORDS_MAX];

// Everything mastered and not due at `tick`, except the listed uids (seen, mastery m, due).
static void seed_due(vocab_tick_t tick, const vocab_uid_t *uids, unsigned n, uint8_t m, uint32_t xp, uint8_t vol)
{
    qa_power_on_fresh(77);
    qa_fresh_records(R);
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        R[i].flags = VOCAB_REC_SEEN | VOCAB_REC_EVER_MASTERED;
        R[i].mastery = VOCAB_MASTERY_MAX;
        R[i].due = (vocab_tick_t)(tick + 20u);
    }
    for (unsigned k = 0; k < n; k++) qa_set_record(R, uids[k], m, (vocab_tick_t)(tick - (n - k)), VOCAB_REC_SEEN);
    qa_seed_storage(R, tick, xp, vol);
    qa_power_cycle(1000);
}

static void wait_for_screen(vocab_screen_t screen, uint32_t max_ms)
{
    for (uint32_t t = 0; t < max_ms && D.view.screen != screen; t += 10) qa_wait(10);
}

static bool on_card(uint8_t cur)
{
    return D.view.screen == VOCAB_SCREEN_CARD && qa_s()->cur == cur;
}

static const vocab_record_t *rec_of(vocab_index_t i)
{
    return &D.records[i];
}

// Answers the current card correctly (S2: flip + I know); `settle_after` waits for its end
// (including any following banner).
static void answer(bool settle_after)
{
    qa_settle();
    if (D.view.screen != VOCAB_SCREEN_CARD) return;
    if (qa_s()->stage_format == VOCAB_FORMAT_S2) {
        qa_wait(VOCAB_S2_FRONT_MIN_MS + 50);
        qa_tap(VOCAB_KEY_OK, 80);
        qa_wait(300);
        qa_tap(VOCAB_KEY_UP, 80);
    } else {
        qa_wait(300);
        qa_tap(qa_correct_key(), 80);
    }
    if (settle_after) qa_settle();
}

static void answer_ok(void)
{
    answer(true);
}

static void finish_session(void)
{
    for (int i = 0; i < 20 && D.view.screen != VOCAB_SCREEN_SUMMARY; i++) answer_ok();
    wait_for_screen(VOCAB_SCREEN_SUMMARY, 3000);
}

// ---------------------------------------------------------------------------------------
// AC-4, AC-43: HOME, SUMMARY and its lock
// ---------------------------------------------------------------------------------------

static void test_home_and_summary(void)
{
    qa_power_on_fresh(1);
    CHECK(D.view.screen == VOCAB_SCREEN_HOME && D.backlight == VOCAB_BACKLIGHT_ON, "boot: HOME, backlight on");
    qa_mark();
    qa_wait(300);
    qa_tap(VOCAB_KEY_UP, 80);
    qa_tap(VOCAB_KEY_DOWN, 80);
    qa_hold(VOCAB_KEY_OK, 700);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_HOME && qa_count(VOCAB_CMD_SAVE) == 0 &&
              qa_count(VOCAB_CMD_PLAY_WORD) == 0, "HOME ignores UP/DOWN clicks and OK/UP LONG");
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_CARD && strcmp(D.view.status.progress.text, "1/5") == 0,
          "HOME + OK starts a session (AC-4)");

    qa_mark();
    finish_session();
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "SUMMARY reached");
    const vocab_ms_t t_sum = D.screen_since;
    const int stop = qa_find(VOCAB_CMD_STOP_AUDIO, 0);
    int save = -1;
    for (int i = 0; i < (int)D.log_len; i++) {
        if (D.log[i].kind == VOCAB_CMD_SAVE && D.log[i].t == t_sum) save = i;
    }
    const int cheer = qa_find(VOCAB_CMD_PLAY_CUE, save < 0 ? 0 : save);
    CHECK(save >= 0 && stop >= 0 && cheer > save && D.log[cheer].cue == VOCAB_CUE_CHEER,
          "SUMMARY: stop audio, save, then the cheer (AC-43)");
    CHECK(vocab_engine_persist(&D.eng).tick == 1 && D.saved_persist.tick == 1, "tick + 1, saved (AC-10)");
    // A click inside the 800 ms lock is ignored; a press begun in the lock never acts later.
    qa_tap_click_at(VOCAB_KEY_OK, 100, t_sum + VOCAB_SUMMARY_LOCK_MS - 1);
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "OK at 799 ms of SUMMARY ignored (AC-4)");
    qa_tap_click_at(VOCAB_KEY_UP, 120, t_sum + VOCAB_SUMMARY_LOCK_MS + 100);
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "a press begun in the lock does not act after it (R3)");
    CHECK(strcmp(D.view.hint.text, VOCAB_STR_HINT_SUMMARY) == 0, "SUMMARY hint after the lock");
    qa_wait(300);
    qa_tap(VOCAB_KEY_UP, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_HOME, "SUMMARY + UP -> HOME (AC-4)");

    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    finish_session();
    qa_wait(VOCAB_SUMMARY_LOCK_MS + 300);
    qa_tap(VOCAB_KEY_DOWN, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_HOME, "SUMMARY + DOWN -> HOME (AC-4)");
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    finish_session();
    qa_wait(VOCAB_SUMMARY_LOCK_MS + 300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_CARD && vocab_engine_persist(&D.eng).tick == 3,
          "SUMMARY + OK starts the next session (AC-4)");
}

// ---------------------------------------------------------------------------------------
// AC-15, AC-29: the Stage 1 error hold at its boundaries
// ---------------------------------------------------------------------------------------

static void test_error_hold_timing(void)
{
    qa_power_on_fresh(2);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_s()->stage_format == VOCAB_FORMAT_S1, "first card in Stage 1");
    const vocab_index_t word = qa_card()->index;
    qa_wait(400);
    const vocab_key_t right = qa_correct_key();
    const vocab_key_t wrong = qa_other_key(right);
    // Wrong press, and the correct key tapped right behind it (press before the hold starts).
    const vocab_ms_t t = D.now + 1;
    qa_push(wrong, VOCAB_RAW_PRESS, t);
    qa_push(wrong, VOCAB_RAW_RELEASE, t + 80);
    qa_push(wrong, VOCAB_RAW_CLICK, t + 260);
    qa_push(right, VOCAB_RAW_PRESS, t + 100);
    qa_push(right, VOCAB_RAW_RELEASE, t + 180);
    qa_push(right, VOCAB_RAW_CLICK, t + 360);
    qa_run_until(t + 260);
    const vocab_ms_t tw = D.now;
    CHECK(qa_locked() && D.view.badge == VOCAB_BADGE_REMEMBER && !D.view.panel[qa_panel_of(wrong)].visible &&
              D.view.panel[qa_panel_of(right)].visible &&
              D.view.panel[qa_panel_of(right)].style == VOCAB_PANEL_CORRECT &&
              strcmp(D.view.card.prompt.text, VOCAB_STR_HOLD_MEANING) == 0 && D.view.card.word.visible &&
              D.view.hint.text[0] == '\0', "same update: wrong panel hidden, correct panel green (spec 5.3)");
    CHECK(rec_of(word)->mastery == 0 && rec_of(word)->due == 1 && rec_of(word)->lapses == 0, "O2 (first encounter)");
    qa_run_until(t + 400);
    CHECK(on_card(0) && qa_locked(), "the queued correct key is flushed at the hold start");
    // Correct key at 1499 ms: ignored.
    qa_tap_click_at(right, 119, tw + VOCAB_ERROR_HOLD_MS - 1);
    CHECK(on_card(0) && qa_locked(), "correct key at 1499 ms ignored (AC-15)");
    // A press begun inside the lock with its CLICK after the lock: dropped (flush at the end).
    qa_push(right, VOCAB_RAW_PRESS, tw + 1450);
    qa_push(right, VOCAB_RAW_RELEASE, tw + 1530);
    qa_push(right, VOCAB_RAW_CLICK, tw + 1710);
    qa_run_until(tw + VOCAB_ERROR_HOLD_MS);
    CHECK(!qa_locked() && D.unlock_since == tw + VOCAB_ERROR_HOLD_MS, "the lock ends at exactly 1500 ms");
    CHECK(strcmp(D.view.hint.text, right == VOCAB_KEY_UP ? VOCAB_STR_HINT_PRESS_UP : VOCAB_STR_HINT_PRESS_DOWN) == 0 &&
              D.view.panel[qa_panel_of(right)].nudge_repeat, "wait part: press-to-continue hint, nudging panel");
    qa_run_until(tw + 1710);
    CHECK(on_card(0), "press begun in the lock does not complete the card (AC-15 flush)");
    // Correct key within 250 ms of the hold-end epoch: cooldown.
    qa_push(right, VOCAB_RAW_PRESS, tw + 1500 + 249);
    qa_push(right, VOCAB_RAW_RELEASE, tw + 1500 + 330);
    qa_push(right, VOCAB_RAW_CLICK, tw + 1500 + 510);
    qa_run_until(tw + 1500 + 510);
    CHECK(on_card(0), "UP/DOWN within 250 ms of the epoch ignored (AC-29)");
    // The other key only nudges; OK replays.
    const uint8_t nudges = D.view.anim.nudge_seq[qa_panel_of(right)];
    qa_wait(300);
    qa_tap(wrong, 80);
    CHECK(on_card(0) && D.view.anim.nudge_seq[qa_panel_of(right)] != nudges, "the other key only nudges (AC-15)");
    qa_mark();
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(on_card(0) && qa_find(VOCAB_CMD_PLAY_WORD, 0) >= 0, "OK replays the word in the wait part");
    qa_wait(300);
    qa_tap(right, 80);
    CHECK(on_card(1), "only the correct key completes the card (AC-15)");
    CHECK(rec_of(word)->mastery == 0 && rec_of(word)->due == 1, "outcome fixed at the first press");

    // AC-29: a press begun on the previous card never acts on the new card.
    qa_wait(400);
    const vocab_key_t right2 = qa_correct_key();
    qa_tap(right2, 80);  // correct: 600 ms locked feedback, then card 3 (index 2)
    const vocab_ms_t tc = D.now;
    qa_push(VOCAB_KEY_UP, VOCAB_RAW_PRESS, tc + 500);
    qa_push(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, tc + 580);
    qa_push(VOCAB_KEY_UP, VOCAB_RAW_CLICK, tc + 760);
    qa_run_until(tc + 800);
    CHECK(on_card(2) && D.card_since == tc + VOCAB_CORRECT_FEEDBACK_MS && !qa_s()->outcome_seen,
          "a press begun before the card change never acts on the new card (AC-29)");
    qa_wait(400);
    qa_tap(qa_correct_key(), 80);
    CHECK(on_card(2) && qa_s()->outcome_seen, "a press after the cooldown answers");
}

static void test_cooldown_boundary(void)
{
    for (int edge = 0; edge < 2; edge++) {
        qa_power_on_fresh(3);
        qa_wait(300);
        qa_tap(VOCAB_KEY_OK, 80);
        const vocab_ms_t t0 = D.card_since;
        const vocab_key_t right = qa_correct_key();
        const uint32_t at = edge ? VOCAB_CARD_COOLDOWN_MS : VOCAB_CARD_COOLDOWN_MS - 1u;
        qa_push(right, VOCAB_RAW_PRESS, t0 + at);
        qa_push(right, VOCAB_RAW_RELEASE, t0 + at + 80);
        qa_push(right, VOCAB_RAW_CLICK, t0 + at + 260);
        qa_run_until(t0 + at + 260);
        CHECK(qa_s()->outcome_seen == (edge == 1), "press at %u ms after the new card: %s (AC-29)", at,
              edge ? "accepted" : "ignored");
    }
}

// QA-4 (= reviewer R-1), epoch part: a double tap whose first press began on the previous
// card is reported by the BSP as one DOUBLE after the card change; spec 3.3 R3 says a press
// begun on the previous screen never acts on the new one.
static void test_double_tap_across_card_change(void)
{
    qa_power_on_fresh(12);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    qa_wait(400);
    qa_tap(qa_correct_key(), 80);  // correct: the next card appears 600 ms later
    const vocab_ms_t e = D.now + VOCAB_CORRECT_FEEDBACK_MS;
    const vocab_key_t key = VOCAB_KEY_DOWN;
    qa_push(key, VOCAB_RAW_PRESS, e - 30);   // first press: still on the previous card
    qa_push(key, VOCAB_RAW_RELEASE, e + 80);
    qa_push(key, VOCAB_RAW_PRESS, e + 255);  // second press 175 ms later: a DOUBLE
    qa_push(key, VOCAB_RAW_RELEASE, e + 355);
    qa_push(key, VOCAB_RAW_DOUBLE, e + 540);
    qa_run_until(e + 600);
    CHECK(on_card(1) && D.card_since == e, "the next card appeared at the expected time");
    qa_known_defect("QA-4", !qa_s()->outcome_seen,
                    "a double tap begun on the previous card answers the new card (reviewer R-1; spec 3.3 R3, "
                    "AC-29)");
}

// ---------------------------------------------------------------------------------------
// AC-17: Stage 2 front and flip; AC-24: Stage 3 retry and reveal
// ---------------------------------------------------------------------------------------

static void test_stage2_front(void)
{
    static const vocab_uid_t five[5] = { 202, 401, 601, 701, 801 };  // no cloze data: word fronts
    seed_due(10, five, 5, 1, 40, 0);
    qa_wait(300);
    qa_mark();
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_s()->stage_format == VOCAB_FORMAT_S2 && D.view.badge == VOCAB_BADGE_RECALL &&
              !D.view.panel[0].visible && D.view.card.word.visible, "m=1: Stage 2 word front");
    CHECK(qa_find(VOCAB_CMD_PLAY_WORD, 0) >= 0 && strcmp(D.view.hint.text, VOCAB_STR_HINT_THINK) == 0,
          "front auto-plays, hint 'think' for 1000 ms");
    const vocab_ms_t t0 = D.card_since;
    qa_tap_click_at(VOCAB_KEY_OK, 100, t0 + VOCAB_S2_FRONT_MIN_MS - 1);
    CHECK(on_card(0) && !D.view.panel[0].visible, "OK at 999 ms ignored (AC-17)");
    qa_run_until(t0 + VOCAB_S2_FRONT_MIN_MS);
    CHECK(strcmp(D.view.hint.text, VOCAB_STR_HINT_FLIP) == 0, "hint 'any key flips' at 1000 ms");
    qa_wait(400);
    qa_mark();
    qa_tap(VOCAB_KEY_DOWN, 80);
    CHECK(D.view.panel[0].visible && D.view.panel[1].visible && D.view.card.meaning.visible &&
              D.view.badge == VOCAB_BADGE_SELF_RATE, "DOWN flips to the back (AC-17)");
    const int flip = qa_find(VOCAB_CMD_PLAY_CUE_THEN_WORD, 0);
    CHECK(flip >= 0 && D.log[flip].cue == VOCAB_CUE_FLIP, "flip requests CUE_FLIP then the word (AC-17)");
    const vocab_ms_t tf = D.now;
    qa_tap_click_at(VOCAB_KEY_UP, 80, tf + 1 + 80 + 180);
    CHECK(on_card(0) && !qa_s()->outcome_seen, "UP right after the flip is inside the 250 ms cooldown");
    qa_wait(300);
    const vocab_index_t w = qa_card()->index;
    qa_mark();
    qa_tap(VOCAB_KEY_UP, 80);
    CHECK(rec_of(w)->mastery == 2 && rec_of(w)->due == 12 && qa_locked() && qa_find(VOCAB_CMD_PLAY_CUE, 0) >= 0,
          "I know: O3 (m 2, due T+2), confirm lock, tick cue");
    qa_settle();
    CHECK(on_card(1), "next card after the 400 ms confirm");
}

static void test_stage3_retry_and_reveal(void)
{
    static const vocab_uid_t two[2] = { 203, 301 };  // duck, bus: cloze data
    seed_due(10, two, 2, 3, 40, 0);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    for (int k = 0; k < 2; k++) {
        qa_settle();
        while (D.view.screen == VOCAB_SCREEN_CARD && qa_s()->stage_format != VOCAB_FORMAT_S3) answer_ok();
        if (D.view.screen != VOCAB_SCREEN_CARD) break;
        const uint8_t cur = qa_s()->cur;
        const vocab_index_t w = qa_card()->index;
        const vocab_record_t before = *rec_of(w);
        CHECK(D.view.badge == VOCAB_BADGE_SPELL && D.view.card.skeleton.visible && D.view.card.skeleton.blink &&
                  !D.view.card.ipa.visible && D.view.panel[0].text_en, "Stage 3 card view (spec 7.3)");
        qa_wait(300);
        const vocab_key_t right = qa_correct_key();
        const vocab_key_t wrong = qa_other_key(right);
        qa_tap(wrong, 80);
        const vocab_ms_t tm = D.now;
        CHECK(qa_locked() && !D.view.card.skeleton.filled && D.view.panel[0].visible && D.view.panel[1].visible &&
                  strcmp(D.view.card.prompt.text, VOCAB_STR_S3_RETRY) == 0 && !qa_s()->outcome_seen,
              "miss 1: shake, retry prompt, both options, no outcome (AC-24)");
        qa_tap_click_at(right, 80, tm + VOCAB_S3_RETRY_LOCK_MS - 1);
        CHECK(on_card(cur) && qa_locked(), "retry lock holds 600 ms");
        qa_run_until(tm + VOCAB_S3_RETRY_LOCK_MS);
        CHECK(!qa_locked() && D.unlock_since == tm + VOCAB_S3_RETRY_LOCK_MS, "retry lock ends at 600 ms");
        qa_wait(300);
        if (k == 0) {
            qa_tap(right, 80);
            CHECK(D.view.card.skeleton.filled && rec_of(w)->mastery == before.mastery && rec_of(w)->due == 11,
                  "correct on the second press: O8 (m unchanged, due T+1)");
            qa_run_until(D.now + VOCAB_SUCCESS_HOLD_MS - 1);
            CHECK(on_card(cur), "success hold 1200 ms");
            qa_wait(2);
            CHECK(!on_card(cur), "next card after the success hold");
        } else {
            qa_tap(wrong, 80);
            CHECK(D.view.card.skeleton.filled && D.view.badge == VOCAB_BADGE_REMEMBER &&
                      !D.view.panel[qa_panel_of(wrong)].visible &&
                      strcmp(D.view.card.prompt.text, VOCAB_STR_S3_REVEAL) == 0 && rec_of(w)->mastery == 2 &&
                      rec_of(w)->due == 11 && rec_of(w)->lapses == before.lapses + 1,
                  "second miss: reveal + hold, O9 (AC-24)");
            qa_wait(VOCAB_ERROR_HOLD_MS + 300);
            qa_tap(wrong, 80);
            CHECK(on_card(cur), "reveal wait: the other key only nudges");
            qa_wait(300);
            qa_tap(right, 80);
            CHECK(!on_card(cur), "the correct key ends the reveal");
        }
    }
}

// ---------------------------------------------------------------------------------------
// AC-19, AC-20: spot check
// ---------------------------------------------------------------------------------------

static void test_spot_check(bool pass, bool cloze_target)
{
    // Five m=1 (Stage 1 check) or m=2 cloze words (Stage 3 check); dues T-5 .. T-1.
    static const vocab_uid_t plain[5] = { 202, 401, 601, 701, 801 };
    static const vocab_uid_t cloze[5] = { 101, 201, 203, 301, 402 };
    const vocab_uid_t *uids = cloze_target ? cloze : plain;
    seed_due(30, uids, 5, cloze_target ? 2 : 1, 100, 2 /* muted: word fronts only */);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    vocab_index_t claimed[3];
    vocab_record_t pre[3];
    for (int i = 0; i < 3; i++) {
        qa_settle();
        claimed[i] = qa_card()->index;
        pre[i] = *rec_of(claimed[i]);
        answer(i < 2);
    }
    const vocab_session_t *s = qa_s();
    CHECK(s->card_count == 6 && s->cards[3].purpose == VOCAB_PURPOSE_SPOT_CHECK, "third claim -> spot check next");
    const vocab_index_t target = s->cards[3].index;
    CHECK(target == claimed[0] || target == claimed[1], "target is one of the first two claims (AC-19)");
    wait_for_screen(VOCAB_SCREEN_BANNER, 1000);
    CHECK(D.view.screen == VOCAB_SCREEN_BANNER && strcmp(D.view.card.banner.text, VOCAB_STR_SPOT_BANNER) == 0 &&
              strcmp(D.view.card.prompt.text, VOCAB_STR_SPOT_PROMPT) == 0 && D.view.badge == VOCAB_BADGE_SPOT_CHECK &&
              !D.view.panel[0].visible, "spot-check intro banner");
    const vocab_ms_t tb = D.screen_since;
    qa_settle();
    CHECK(D.card_since == tb + VOCAB_BANNER_MS, "intro lasts 800 ms");
    CHECK(qa_s()->stage_format == (cloze_target ? VOCAB_FORMAT_S3 : VOCAB_FORMAT_S1) &&
              D.view.badge == VOCAB_BADGE_SPOT_CHECK, "spot-check format per spec 4.6 (pre-claim m %s 2)",
          cloze_target ? ">=" : "<");
    qa_wait(300);
    const vocab_key_t right = qa_correct_key();
    qa_tap(pass ? right : qa_other_key(right), 80);
    for (int i = 0; i < 3; i++) {
        const vocab_record_t *r = rec_of(claimed[i]);
        if (pass) {
            CHECK(r->mastery == pre[i].mastery + 1u, "pass: claim %d stands (O10)", i);
        } else if (claimed[i] == target) {
            CHECK(r->mastery == (pre[i].mastery < 1 ? pre[i].mastery : 1u) && r->due == 31 &&
                      r->lapses == pre[i].lapses + 1u, "fail: target demoted (O11)");
        } else {
            CHECK(r->mastery == pre[i].mastery && r->due == (pre[i].due < 31 ? pre[i].due : 31) &&
                      r->lapses == pre[i].lapses, "fail: claim %d restored to its pre-claim state (AC-20)", i);
        }
    }
    CHECK(qa_s()->streak.count == 0, "any spot check resets the streak (AC-20)");
    if (!pass) {
        qa_wait(VOCAB_ERROR_HOLD_MS + 300);
        qa_tap(right, 80);
    }
    qa_settle();
    // Two more claims: no second spot check (the streak restarted).
    answer_ok();
    answer_ok();
    qa_settle();
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY && qa_s()->card_count == 6, "no further spot check this session");
}

// ---------------------------------------------------------------------------------------
// AC-21, AC-22: too easy
// ---------------------------------------------------------------------------------------

static void test_too_easy_stage1(bool pass)
{
    // The first card of a fresh device is a new word in Stage 1; pick a seed whose first
    // word has cloze data (Stage 3 check) for `pass` and one without (Stage 1 check) else.
    for (uint32_t seed = 10; seed < 200; seed++) {
        qa_power_on_fresh(seed);
        qa_wait(300);
        qa_tap(VOCAB_KEY_OK, 80);
        const vocab_entry_t *e = qa_entry();
        if (vocab_entry_has_cloze(e) == pass) break;
    }
    const vocab_index_t w = qa_card()->index;
    const vocab_entry_t *e = qa_entry();
    const char *learn_lure = strcmp(D.view.panel[0].text, e->zh) == 0 ? D.view.panel[1].text : D.view.panel[0].text;
    char lure[VOCAB_VIEW_TEXT_CAP];
    snprintf(lure, sizeof(lure), "%s", learn_lure);
    qa_wait(300);
    qa_hold(VOCAB_KEY_UP, 800);
    CHECK(D.view.screen == VOCAB_SCREEN_BANNER && strcmp(D.view.card.banner.text, VOCAB_STR_TOO_EASY_ACK) == 0 &&
              D.view.badge == VOCAB_BADGE_NEW, "UP LONG: card ends with the acknowledgement, badge kept (AC-21)");
    CHECK(vocab_record_seen(rec_of(w)) && rec_of(w)->mastery == 0 && rec_of(w)->due == 0, "O12: seen, unchanged");
    CHECK(qa_s()->card_count == 6 && qa_s()->cards[3].purpose == VOCAB_PURPOSE_TOO_EASY_CHECK &&
              qa_s()->cards[3].index == w, "check inserted two cards later (AC-21)");
    const vocab_ms_t ta = D.screen_since;
    wait_for_screen(VOCAB_SCREEN_CARD, 2000);
    CHECK(D.card_since - ta == VOCAB_BANNER_MS, "acknowledgement lasts 800 ms");
    answer_ok();
    answer(false);
    wait_for_screen(VOCAB_SCREEN_BANNER, 2000);
    CHECK(strcmp(D.view.card.banner.text, VOCAB_STR_TOO_EASY_BANNER) == 0 &&
              strcmp(D.view.card.prompt.text, VOCAB_STR_TOO_EASY_PROMPT) == 0 && D.view.badge == VOCAB_BADGE_TOO_EASY,
          "too-easy intro");
    qa_settle();
    CHECK(qa_card()->index == w && qa_s()->stage_format == (vocab_entry_has_cloze(e) ? VOCAB_FORMAT_S3
                                                                                      : VOCAB_FORMAT_S1),
          "check: Stage 3 with cloze data, else Stage 1 (AC-21)");
    if (qa_s()->stage_format == VOCAB_FORMAT_S1) {
        const char *check_lure = strcmp(D.view.panel[0].text, e->zh) == 0 ? D.view.panel[1].text : D.view.panel[0].text;
        CHECK(strcmp(check_lure, lure) != 0, "the check uses a fresh lure (spec 5.2 step 4)");
    }
    // UP LONG on the inserted card is ignored (AC-22).
    qa_wait(300);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(on_card(3), "UP LONG ignored on the check card (AC-22)");
    qa_wait(300);
    const vocab_key_t right = qa_correct_key();
    qa_tap(pass ? right : qa_other_key(right), 80);
    if (pass) {
        CHECK(rec_of(w)->mastery == 5 && rec_of(w)->due == 0 + VOCAB_TOO_EASY_INTERVAL &&
                  (rec_of(w)->flags & VOCAB_REC_EVER_MASTERED), "pass: m 5, due T+30 (O13)");
    } else {
        CHECK(rec_of(w)->mastery == 0 && rec_of(w)->due == 1 && rec_of(w)->lapses == 1, "fail: min(m,1), T+1 (O14)");
        qa_wait(VOCAB_ERROR_HOLD_MS + 300);
        qa_tap(right, 80);
    }
    finish_session();
    const uint32_t xp = vocab_engine_persist(&D.eng).xp;
    char text[48];
    snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_XP_FMT, (unsigned)xp);
    CHECK(strcmp(D.view.summary.xp.text, text) == 0, "SUMMARY shows the XP of the session");
    if (pass) CHECK(xp == 5u + 5u + 2u, "XP 5 + 5 first-try (4 new + check) + 2 mastery bonus (got %u)", (unsigned)xp);
}

static void test_too_easy_stage2_and_ignored(void)
{
    static const vocab_uid_t five[5] = { 202, 401, 601, 701, 801 };
    seed_due(10, five, 5, 1, 40, 0);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    // Too easy on the Stage 2 front during the first 1000 ms (allowed: not a lock).
    const vocab_index_t w = qa_card()->index;
    qa_wait(100);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_BANNER && qa_s()->cards[3].purpose == VOCAB_PURPOSE_TOO_EASY_CHECK,
          "UP LONG on the Stage 2 front = too easy (AC-21)");
    qa_settle();
    // Card 2: UP LONG on the back is ignored (AC-22).
    qa_wait(VOCAB_S2_FRONT_MIN_MS + 50);
    qa_tap(VOCAB_KEY_OK, 80);
    qa_wait(300);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(on_card(1) && D.view.panel[0].visible && !qa_s()->outcome_seen, "UP LONG ignored on the Stage 2 back");
    qa_wait(300);
    qa_tap(VOCAB_KEY_UP, 80);
    qa_settle();
    answer_ok();  // card 3
    qa_settle();
    CHECK(qa_card()->index == w && qa_s()->stage_format == VOCAB_FORMAT_S1, "check of a word without cloze: Stage 1");
    qa_wait(300);
    const vocab_key_t right = qa_correct_key();
    qa_tap(qa_other_key(right), 80);
    CHECK(rec_of(w)->mastery == 1 && rec_of(w)->due == 11 && rec_of(w)->lapses == 1, "fail: O14 (m min(1,1), T+1)");
    // UP LONG inside the error-hold lock: ignored.
    qa_hold(VOCAB_KEY_UP, 600);
    CHECK(on_card(3) && qa_locked(), "UP LONG ignored in the lock (AC-22)");
    qa_settle();
    qa_wait(300);
    qa_tap(right, 80);
    finish_session();
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "UP LONG ignored on SUMMARY (AC-22)");
}

static void test_too_easy_ignored_in_stage3(void)
{
    static const vocab_uid_t five[5] = { 101, 201, 203, 301, 402 };
    seed_due(10, five, 5, 3, 40, 0);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_s()->stage_format == VOCAB_FORMAT_S3, "m=3 with cloze: Stage 3");
    qa_wait(300);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(on_card(0) && !qa_s()->outcome_seen && qa_s()->card_count == 5, "UP LONG ignored in Stage 3 (AC-22)");
}

// ---------------------------------------------------------------------------------------
// AC-5: pause
// ---------------------------------------------------------------------------------------

static void test_pause(void)
{
    qa_power_on_fresh(4);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    const vocab_index_t first = qa_card()->index;
    answer_ok();
    // Card 2 waiting: pause, then UP resumes the same card.
    const vocab_index_t second = qa_card()->index;
    char up[VOCAB_VIEW_TEXT_CAP], down[VOCAB_VIEW_TEXT_CAP];
    snprintf(up, sizeof(up), "%s", D.view.panel[0].text);
    snprintf(down, sizeof(down), "%s", D.view.panel[1].text);
    qa_wait(300);
    qa_mark();
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_PAUSE && D.view.badge == VOCAB_BADGE_PAUSE &&
              strcmp(D.view.card.banner.text, VOCAB_STR_PAUSE_BANNER) == 0 &&
              strcmp(D.view.card.prompt.text, VOCAB_STR_PAUSE_PROMPT) == 0 &&
              strcmp(D.view.panel[0].text, VOCAB_STR_PAUSE_CONTINUE) == 0 &&
              strcmp(D.view.panel[1].text, VOCAB_STR_PAUSE_HOME) == 0 && D.view.hint.text[0] == '\0' &&
              qa_find(VOCAB_CMD_STOP_AUDIO, 0) >= 0, "pause card (spec 11.5), audio stopped");
    qa_wait(300);
    qa_tap(VOCAB_KEY_UP, 80);
    CHECK(on_card(1) && strcmp(D.view.panel[0].text, up) == 0 && strcmp(D.view.panel[1].text, down) == 0,
          "UP resumes the same card with the same sides (AC-5)");
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(on_card(1) && strcmp(D.view.panel[0].text, up) == 0, "OK resumes the same card (AC-5)");
    // Wrong answer, pause inside the error-hold lock is ignored; in the wait part it works.
    qa_wait(300);
    const vocab_key_t right = qa_correct_key();
    qa_tap(qa_other_key(right), 80);
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(on_card(1) && qa_locked(), "OK LONG ignored during the hold lock");
    qa_settle();
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_PAUSE, "pause in the wait part");
    qa_wait(300);
    qa_tap(VOCAB_KEY_UP, 80);
    CHECK(on_card(1) && !qa_locked() && D.view.panel[qa_panel_of(right)].visible &&
              !D.view.panel[qa_panel_of(qa_other_key(right))].visible &&
              strcmp(D.view.hint.text, right == VOCAB_KEY_UP ? VOCAB_STR_HINT_PRESS_UP : VOCAB_STR_HINT_PRESS_DOWN) == 0,
          "resume returns to the wait part");
    qa_wait(300);
    qa_tap(qa_other_key(right), 80);
    CHECK(on_card(1), "after resume the other key still only nudges");
    // Pause -> HOME keeps both outcomes, saves, no tick.
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    qa_wait(300);
    qa_mark();
    qa_tap(VOCAB_KEY_DOWN, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_HOME && qa_find(VOCAB_CMD_SAVE, 0) >= 0 &&
              vocab_engine_persist(&D.eng).tick == 0, "DOWN -> HOME, saved, no tick (AC-5, AC-10)");
    CHECK(rec_of(first)->mastery == 1 && rec_of(second)->mastery == 0 && vocab_record_seen(rec_of(second)) &&
              rec_of(second)->due == 1, "applied outcomes kept (AC-5)");
    qa_power_cycle(2000);  // the harness checks the round trip of the saved state
    CHECK(rec_of(first)->mastery == 1 && rec_of(second)->due == 1, "outcomes survive a reboot");
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_HOME, "OK LONG ignored on HOME");
}

// ---------------------------------------------------------------------------------------
// AC-34, AC-33, AC-43: audio substitution and volume
// ---------------------------------------------------------------------------------------

static unsigned pulses(void)
{
    return D.view.anim.pulse_seq;
}

static void test_mute_session(void)
{
    qa_power_on_fresh(5);
    CHECK(D.view.status.volume == VOCAB_VOL_ICON_HIGH, "80 %% icon on a fresh device");
    qa_wait(300);
    qa_mark();
    qa_hold(VOCAB_KEY_DOWN, 700);
    int i = qa_find(VOCAB_CMD_SET_VOLUME, 0);
    CHECK(i >= 0 && D.log[i].value == 30u && qa_find(VOCAB_CMD_PLAY_CUE, i) > i &&
              D.view.status.volume == VOCAB_VOL_ICON_LOW && strcmp(D.view.hint.text, VOCAB_STR_TOAST_VOL_LOW) == 0,
          "DOWN LONG: 30 %%, tick, toast (AC-33)");
    qa_wait(300);
    qa_mark();
    qa_hold(VOCAB_KEY_DOWN, 700);
    i = qa_find(VOCAB_CMD_SET_VOLUME, 0);
    CHECK(i >= 0 && D.log[i].value == 0u && qa_find(VOCAB_CMD_STOP_AUDIO, i) > i && qa_count(VOCAB_CMD_PLAY_CUE) == 0 &&
              D.view.status.volume == VOCAB_VOL_ICON_MUTE && strcmp(D.view.hint.text, VOCAB_STR_TOAST_MUTE) == 0,
          "DOWN LONG: mute, stop, no tick (AC-33)");
    CHECK(qa_count(VOCAB_CMD_SAVE) == 0, "volume changes do not write (AC-43)");
    qa_wait(VOCAB_TOAST_MS + 100);
    // A whole muted session: no I2S request at all, WORD_PULSE on every would-be clip.
    qa_mark();
    unsigned before = pulses();
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(pulses() == before + 1u && strcmp(D.view.hint.text, VOCAB_STR_HINT_LOOK) == 0,
          "muted card entry: WORD_PULSE, hint 'look again' (AC-34)");
    before = pulses();
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(pulses() == before + 1u, "muted replay: WORD_PULSE (AC-34)");
    qa_wait(300);
    const vocab_key_t right = qa_correct_key();
    before = pulses();
    qa_tap(qa_other_key(right), 80);
    CHECK(pulses() == before + 1u, "muted wrong answer: the word pulses instead of the clip");
    qa_settle();
    qa_wait(300);
    qa_tap(right, 80);
    finish_session();
    CHECK(qa_count(VOCAB_CMD_PLAY_WORD) == 0 && qa_count(VOCAB_CMD_PLAY_CUE) == 0 &&
              qa_count(VOCAB_CMD_PLAY_CUE_THEN_WORD) == 0, "mute = no audio request at all (spec 8.4)");
    // The volume index reached storage with the SUMMARY save; a reboot keeps it.
    qa_power_cycle(1000);
    CHECK(qa_volume_index() == 2 && D.view.status.volume == VOCAB_VOL_ICON_MUTE && D.volume_pct == 0,
          "mute persisted at the next save point (AC-43, spec 8.4)");
    qa_wait(300);
    qa_mark();
    qa_hold(VOCAB_KEY_DOWN, 700);
    i = qa_find(VOCAB_CMD_SET_VOLUME, 0);
    CHECK(i >= 0 && D.log[i].value == 80u && qa_find(VOCAB_CMD_PLAY_CUE, i) > i, "mute -> 80 %% with a tick");
}

static void test_audio_unavailable_and_missing_clip(void)
{
    // Failed audio init: dim mute icon, toast, every clip pulses, no cue (AC-34).
    qa_power_on_fresh(6);
    D.audio_ok = false;
    qa_power_cycle(500);
    CHECK(D.view.status.volume == VOCAB_VOL_ICON_NO_AUDIO, "dim mute icon when audio init failed");
    qa_wait(300);
    qa_mark();
    qa_hold(VOCAB_KEY_DOWN, 700);
    CHECK(qa_count(VOCAB_CMD_SET_VOLUME) == 0 && strcmp(D.view.hint.text, VOCAB_STR_TOAST_NO_AUDIO) == 0,
          "DOWN LONG: toast 'sound unavailable', no volume command");
    qa_wait(VOCAB_TOAST_MS + 100);
    const unsigned before = pulses();
    qa_mark();
    qa_tap(VOCAB_KEY_OK, 80);
    finish_session();
    CHECK(pulses() > before && qa_count(VOCAB_CMD_PLAY_WORD) == 0 && qa_count(VOCAB_CMD_PLAY_CUE) == 0 &&
              qa_count(VOCAB_CMD_PLAY_CUE_THEN_WORD) == 0, "no audio request at all, WORD_PULSE instead");

    // One clip missing with audio on: that word pulses, cues still play, the hint follows.
    qa_power_on_fresh(20);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    const vocab_uid_t first_uid = qa_entry()->uid;
    qa_power_on_fresh(20);  // same boot seed: the same first word, now without its clip
    D.missing_uid = first_uid;
    qa_wait(300);
    qa_mark();
    const unsigned p0 = pulses();
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_entry()->uid == first_uid && pulses() == p0 + 1u && qa_count(VOCAB_CMD_PLAY_WORD) == 0 &&
              strcmp(D.view.hint.text, VOCAB_STR_HINT_LOOK) == 0, "missing clip: WORD_PULSE and 'look again'");
    qa_wait(300);
    qa_mark();
    qa_tap(qa_correct_key(), 80);
    CHECK(qa_find(VOCAB_CMD_PLAY_CUE, 0) >= 0, "cues still play when only the clip is missing");
    qa_settle();
    CHECK(strcmp(D.view.hint.text, VOCAB_STR_HINT_LISTEN) == 0, "the next word has a clip: 'listen again'");
    D.missing_uid = 0;
}

static unsigned volume_after_long(void)
{
    qa_hold(VOCAB_KEY_DOWN, 520);
    return qa_volume_index();
}

static void test_volume_everywhere(void)
{
    static const vocab_uid_t mix[5] = { 101, 203, 202, 401, 301 };
    qa_power_on_fresh(8);
    qa_fresh_records(R);
    qa_set_record(R, 203, 3, 9, VOCAB_REC_SEEN);  // Stage 3
    qa_set_record(R, 301, 3, 9, VOCAB_REC_SEEN);  // Stage 3
    qa_set_record(R, 202, 1, 9, VOCAB_REC_SEEN);  // Stage 2
    qa_set_record(R, 401, 1, 9, VOCAB_REC_SEEN);  // Stage 2
    (void)mix;
    qa_seed_storage(R, 10, 0, 0);
    qa_power_cycle(1000);
    unsigned expect = 0, changes = 0, saves = D.saves;
    const unsigned saves_at_start = saves;
    qa_wait(300);
    expect = (expect + 1u) % 3u;
    CHECK(volume_after_long() == expect && D.view.screen == VOCAB_SCREEN_HOME, "HOME: volume cycles");
    changes++;
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    unsigned seen_states = 0;
    for (int guard = 0; guard < 40 && D.view.screen != VOCAB_SCREEN_SUMMARY; guard++) {
        if (D.view.screen != VOCAB_SCREEN_CARD) {
            qa_wait(50);
            continue;
        }
        const uint8_t cur = qa_s()->cur;
        const bool outcome = qa_s()->outcome_seen;
        expect = (expect + 1u) % 3u;
        const unsigned got = volume_after_long();
        CHECK(got == expect, "DOWN LONG cycles in every state (format %d) (AC-33)", (int)qa_s()->stage_format);
        CHECK(qa_s()->cur == cur && qa_s()->outcome_seen == outcome, "DOWN LONG never answers (AC-33)");
        changes++;
        seen_states |= 1u << (unsigned)qa_s()->stage_format;
        seen_states |= 0x200u;
        qa_wait(300);
        // Play on: wrong first in Stage 1/3 to get lock windows, I know in Stage 2.
        if (qa_s()->stage_format == VOCAB_FORMAT_S2) {
            qa_wait(VOCAB_S2_FRONT_MIN_MS);
            qa_tap(VOCAB_KEY_OK, 80);
            expect = (expect + 1u) % 3u;
            CHECK(volume_after_long() == expect && D.view.panel[0].visible, "Stage 2 back: volume cycles");
            changes++;
            qa_wait(300);
            qa_tap(VOCAB_KEY_UP, 80);
        } else if (!qa_s()->outcome_seen) {
            qa_tap(qa_other_key(qa_correct_key()), 80);
            expect = (expect + 1u) % 3u;
            CHECK(volume_after_long() == expect && qa_locked() && qa_s()->cur == cur,
                  "inside a lock window: volume cycles (AC-33)");
            seen_states |= 0x100u;
            changes++;
        }
        qa_settle();
        if (D.view.screen == VOCAB_SCREEN_CARD && qa_s()->cur == cur) {
            qa_wait(300);
            qa_tap(qa_correct_key(), 80);
            qa_settle();
            if (D.view.screen == VOCAB_SCREEN_CARD && qa_s()->cur == cur) {
                qa_wait(VOCAB_S3_RETRY_LOCK_MS + 300);
                qa_tap(qa_correct_key(), 80);
                qa_settle();
            }
        }
    }
    wait_for_screen(VOCAB_SCREEN_SUMMARY, 3000);
    saves = D.saves;
    expect = (expect + 1u) % 3u;
    CHECK(volume_after_long() == expect && D.view.screen == VOCAB_SCREEN_SUMMARY, "SUMMARY lock: volume cycles");
    changes++;
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_PAUSE, "pause");
    expect = (expect + 1u) % 3u;
    CHECK(volume_after_long() == expect && D.view.screen == VOCAB_SCREEN_PAUSE, "pause card: volume cycles");
    changes++;
    CHECK(D.saves == saves, "volume changes alone never save (AC-43)");
    CHECK((seen_states & 0x30Eu) == 0x30Eu, "volume tried in Stage 1/2/3, locked and unlocked (0x%x)", seen_states);
    printf("QA flows: %u volume changes across states, %u saves overall\n", changes, D.saves - saves_at_start);
}

// ---------------------------------------------------------------------------------------
// AC-28, AC-31: input hygiene at system level
// ---------------------------------------------------------------------------------------

static void test_input_hygiene(void)
{
    qa_power_on_fresh(9);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    // DOUBLE = one click: answers once.
    qa_wait(300);
    qa_double(qa_correct_key());
    CHECK(qa_s()->outcome_seen && qa_s()->first_try == 1 && qa_s()->objective == 1, "DOUBLE = one click (AC-28)");
    qa_settle();
    // A 29 ms tap is dropped, a 30 ms tap is accepted (amendment A3).
    qa_wait(300);
    qa_tap(qa_correct_key(), VOCAB_MIN_PRESS_MS - 1u);
    CHECK(on_card(1) && !qa_s()->outcome_seen, "29 ms press dropped (AC-31)");
    qa_wait(300);
    // Cross-key phantom: OK clicked while DOWN is held for a long press -> dropped; the LONG
    // itself (volume) still works.
    const unsigned vol = qa_volume_index();
    qa_mark();
    const vocab_ms_t t = D.now + 1;
    qa_push(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, t);
    qa_push(VOCAB_KEY_OK, VOCAB_RAW_PRESS, t + 300);
    qa_push(VOCAB_KEY_OK, VOCAB_RAW_RELEASE, t + 310);
    qa_push(VOCAB_KEY_OK, VOCAB_RAW_CLICK, t + 490);
    qa_push(VOCAB_KEY_DOWN, VOCAB_RAW_LONG, t + 500);
    qa_push(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, t + 900);
    qa_run_until(t + 900);
    CHECK(qa_count(VOCAB_CMD_PLAY_WORD) == 0 && qa_volume_index() != vol && on_card(1),
          "phantom OK during a DOWN hold dropped; the hold itself acts (AC-31)");
    // Re-press within 120 ms after the LONG's release: a bounce, dropped.
    qa_push(VOCAB_KEY_DOWN, VOCAB_RAW_PRESS, t + 900 + VOCAB_LONG_REJOIN_MS - 1);
    qa_push(VOCAB_KEY_DOWN, VOCAB_RAW_RELEASE, t + 900 + VOCAB_LONG_REJOIN_MS + 79);
    qa_push(VOCAB_KEY_DOWN, VOCAB_RAW_CLICK, t + 900 + VOCAB_LONG_REJOIN_MS + 259);
    qa_run_until(t + 900 + VOCAB_LONG_REJOIN_MS + 259);
    CHECK(on_card(1) && !qa_s()->outcome_seen, "re-press within 120 ms after a LONG dropped (AC-31)");
    qa_wait(300);
    qa_tap(qa_correct_key(), VOCAB_MIN_PRESS_MS);
    CHECK(qa_s()->outcome_seen, "30 ms press accepted");
}

int main(void)
{
    test_home_and_summary();
    test_error_hold_timing();
    test_cooldown_boundary();
    test_double_tap_across_card_change();
    test_stage2_front();
    test_stage3_retry_and_reveal();
    test_spot_check(false, false);
    test_spot_check(true, false);
    test_spot_check(false, true);
    test_spot_check(true, true);
    test_too_easy_stage1(true);
    test_too_easy_stage1(false);
    test_too_easy_stage2_and_ignored();
    test_too_easy_ignored_in_stage3();
    test_pause();
    test_mute_session();
    test_audio_unavailable_and_missing_clip();
    test_volume_everywhere();
    test_input_hygiene();
    qa_report_known_defects();
    return vocab_test_report("vocab QA flow tests");
}
