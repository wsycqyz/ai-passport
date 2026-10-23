// tests/test_vocab_qa_days.c - QA system test: a fresh device used over many "days".
//
// Drives the whole engine through raw BSP key events (tests/test_vocab_qa_harness.h) with a
// deterministic "child" policy and checks every observable result against an independent
// oracle written from the spec (requirements 4.1 - 4.9), not from main/vocab_srs.c:
// - AC-6: the first session of a fresh device holds bank words 0..4 in the seeded shuffle;
// - AC-7: the composition of every session (due words by overdue-ness, then lower m, then
//   lower uid; 1..3 new words in bank order; early reviews; more new words);
// - AC-12: the format of every card (and its badge / front kind);
// - AC-11: every outcome row O1-O14 right after the deciding press (mastery, due, lapses,
//   seen, ever_mastered), incl. AC-19/AC-20 (third claim -> spot check next, target one of
//   the earlier claims, full-streak rollback on failure) and AC-21 (too easy: check two
//   cards later, pass -> m 5 / T+30, fail -> min(m,1) / T+1);
// - AC-18 / AC-9: insertion positions and the queue bound;
// - AC-10 / AC-56: SUMMARY is the only place where the tick advances; XP = 5 + first-try +
//   2 x newly mastered; score, XP and level-up texts; pause -> HOME and deep sleep keep the
//   outcomes without a tick;
// - AC-46 (host part): deep sleeps and power cycles reboot from the real store; the harness
//   checks that every reboot restores exactly the last saved state.
//
// Owner: QA engineer.
#include "test_vocab_qa_harness.h"

// ---------------------------------------------------------------------------------------
// Oracle (spec 4.1 - 4.9)
// ---------------------------------------------------------------------------------------

typedef struct {
    uint8_t m;
    vocab_tick_t due;
    bool seen;
    bool ever;
    uint8_t lapses;
} orec_t;

typedef struct {
    vocab_index_t idx;
    orec_t pre;
} oclaim_t;

static const uint8_t OINT[6] = { 0, 1, 2, 5, 10, 20 };       // spec 4.1 INTERVAL[m]
static const uint32_t OLEVEL[10] = { 0, 30, 90, 180, 300, 450, 630, 840, 1080, 1350 };

static orec_t O[VOCAB_RECORDS_MAX];
static vocab_tick_t OT;
static uint32_t OXP;
static oclaim_t OC[3];
static unsigned OCN;
static int o_spot_slot = -1;
static vocab_index_t OBASE[5];
static bool OBASE_EVER[5];
static unsigned OBASEN;
static unsigned O_FIRST, O_OBJ;
static vocab_rng_t P;  // the child's choices

static struct {
    unsigned sessions, summaries, cards, fmt[4], cloze_fronts, spot, spot_fail, too_easy, too_easy_pass,
        test_later, pauses, abandons, sleeps_mid, power_loss, levelups, idle_wakes, max_tick, o_rows[15];
} st;

static vocab_tick_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? (vocab_tick_t)0xFFFFu : (vocab_tick_t)v;
}

static uint8_t o_level_for(uint32_t xp)
{
    uint8_t level = 1;
    for (uint8_t i = 1; i < 10; i++) {
        if (xp >= OLEVEL[i]) level = (uint8_t)(i + 1u);
    }
    return level;
}

static bool too_many_failures(void)
{
    return vocab_test_failures > 30;
}

static void o_sync_from_engine(void)
{
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        const vocab_record_t *r = &D.records[i];
        O[i].m = r->mastery;
        O[i].due = r->due;
        O[i].seen = vocab_record_seen(r);
        O[i].ever = (r->flags & VOCAB_REC_EVER_MASTERED) != 0;
        O[i].lapses = r->lapses;
    }
    const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
    OT = p.tick;
    OXP = p.xp;
    OCN = 0;
    o_spot_slot = -1;
}

static void on_boot(bool loaded)
{
    (void)loaded;
    // A reboot restores the last saved state (checked by the harness); the streak and the
    // in-progress queue are gone.
    o_sync_from_engine();
}

static bool o_matches(vocab_index_t i)
{
    const vocab_record_t *r = &D.records[i];
    return r->mastery == O[i].m && r->due == O[i].due && vocab_record_seen(r) == O[i].seen &&
           ((r->flags & VOCAB_REC_EVER_MASTERED) != 0) == O[i].ever && r->lapses == O[i].lapses;
}

static void o_check_all(const char *what)
{
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        if (o_matches(i)) continue;
        const vocab_record_t *r = &D.records[i];
        CHECK(false, "%s: '%s' engine m%u due %u seen %d ever %d lapses %u; oracle m%u due %u seen %d ever %d "
              "lapses %u (tick %u)", what, D.bank->entries[i].text, (unsigned)r->mastery, (unsigned)r->due,
              (int)vocab_record_seen(r), (int)((r->flags & VOCAB_REC_EVER_MASTERED) != 0), (unsigned)r->lapses,
              (unsigned)O[i].m, (unsigned)O[i].due, (int)O[i].seen, (int)O[i].ever, (unsigned)O[i].lapses,
              (unsigned)OT);
        o_sync_from_engine();  // report once, then continue from the engine's state
        return;
    }
}

static void o_level(orec_t *r, uint8_t m, vocab_tick_t due)
{
    r->m = m;
    r->due = due;
    if (m == 5) r->ever = true;
}

static void o_promote(orec_t *r)
{
    const uint8_t m = r->m < 5 ? (uint8_t)(r->m + 1u) : 5u;
    o_level(r, m, sat16((uint32_t)OT + OINT[m]));
}

static void o_lapse(orec_t *r)
{
    if (r->lapses < 255) r->lapses++;
}

// Spec 4.3 rows O1 - O14 (row numbers counted in st.o_rows).
static void o_apply(vocab_index_t idx, vocab_purpose_t purpose, vocab_format_t fmt, vocab_result_t res,
                    bool was_seen)
{
    orec_t *r = &O[idx];
    const vocab_tick_t next = sat16((uint32_t)OT + 1u);
    r->seen = true;
    if (res == VOCAB_RESULT_TOO_EASY) {
        st.o_rows[12]++;
        return;
    }
    if (fmt == VOCAB_FORMAT_S1 || fmt == VOCAB_FORMAT_S3) O_OBJ++;
    switch (purpose) {
    case VOCAB_PURPOSE_LEARN:
        if (fmt == VOCAB_FORMAT_S2 && res == VOCAB_RESULT_KNOW) {
            if (OCN < 3) {
                OC[OCN].idx = idx;
                OC[OCN].pre = *r;
                OCN++;
            }
            o_promote(r);
            st.o_rows[3]++;
        } else if (fmt == VOCAB_FORMAT_S2 && res == VOCAB_RESULT_FORGOT) {
            o_level(r, r->m < 1 ? r->m : 1u, next);
            o_lapse(r);
            OCN = 0;
            st.o_rows[4]++;
        } else if (fmt == VOCAB_FORMAT_S1 && res == VOCAB_RESULT_CORRECT_FIRST) {
            o_level(r, 1, next);
            O_FIRST++;
            st.o_rows[1]++;
        } else if (fmt == VOCAB_FORMAT_S1 && res == VOCAB_RESULT_WRONG) {
            o_level(r, 0, next);
            if (was_seen) o_lapse(r);
            st.o_rows[2]++;
        } else if (fmt == VOCAB_FORMAT_S3 && res == VOCAB_RESULT_CORRECT_FIRST) {
            o_promote(r);
            O_FIRST++;
            st.o_rows[7]++;
        } else if (fmt == VOCAB_FORMAT_S3 && res == VOCAB_RESULT_CORRECT_SECOND) {
            r->due = next;
            st.o_rows[8]++;
        } else if (fmt == VOCAB_FORMAT_S3 && res == VOCAB_RESULT_WRONG) {
            o_level(r, 2, next);
            o_lapse(r);
            st.o_rows[9]++;
        } else {
            CHECK(false, "oracle: unexpected LEARN row (format %d result %d)", (int)fmt, (int)res);
        }
        break;
    case VOCAB_PURPOSE_TEST_LATER:
        if (res == VOCAB_RESULT_CORRECT_FIRST) {
            O_FIRST++;
            st.o_rows[5]++;
        } else {
            o_level(r, 0, next);
            o_lapse(r);
            st.o_rows[6]++;
        }
        break;
    case VOCAB_PURPOSE_SPOT_CHECK:
        if (res == VOCAB_RESULT_CORRECT_FIRST) {
            O_FIRST++;
            st.o_rows[10]++;
        } else if (o_spot_slot >= 0) {
            // O11: the whole streak is voided; the target is demoted (ever_mastered restored
            // from the claim snapshot: interfaces.md section 4, lead decision).
            const oclaim_t target = OC[o_spot_slot];
            for (unsigned k = 0; k < OCN; k++) {
                if (OC[k].idx == target.idx) continue;
                orec_t *o = &O[OC[k].idx];
                o->m = OC[k].pre.m;
                o->due = OC[k].pre.due < next ? OC[k].pre.due : next;
                o->ever = OC[k].pre.ever;
            }
            orec_t *t = &O[target.idx];
            t->m = target.pre.m < 1 ? target.pre.m : 1u;
            t->due = next;
            t->ever = target.pre.ever;
            o_lapse(t);
            st.o_rows[11]++;
            st.spot_fail++;
        }
        OCN = 0;
        o_spot_slot = -1;
        break;
    case VOCAB_PURPOSE_TOO_EASY_CHECK:
        if (res == VOCAB_RESULT_CORRECT_FIRST) {
            o_level(r, 5, sat16((uint32_t)OT + VOCAB_TOO_EASY_INTERVAL));
            O_FIRST++;
            st.o_rows[13]++;
            st.too_easy_pass++;
        } else {
            o_level(r, r->m < 1 ? r->m : 1u, next);
            o_lapse(r);
            st.o_rows[14]++;
        }
        break;
    default:
        break;
    }
}

// Spec 4.4, written independently: D by overdue-ness (due ascending), then lower m, then
// lower uid; N in bank order; E by due ascending, then lower m, then lower uid.
static bool o_before(vocab_index_t a, vocab_index_t b)
{
    if (O[a].due != O[b].due) return O[a].due < O[b].due;
    if (O[a].m != O[b].m) return O[a].m < O[b].m;
    return D.bank->entries[a].uid < D.bank->entries[b].uid;
}

static void o_sort(vocab_index_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        const vocab_index_t x = v[i];
        unsigned j = i;
        while (j > 0 && o_before(x, v[j - 1])) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

static unsigned o_compose(vocab_index_t *out)
{
    vocab_index_t d[VOCAB_RECORDS_MAX], n[VOCAB_RECORDS_MAX], e[VOCAB_RECORDS_MAX];
    unsigned nd = 0, nn = 0, ne = 0;
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        if (!O[i].seen) {
            n[nn++] = i;
        } else if (O[i].due <= OT) {
            d[nd++] = i;
        } else {
            e[ne++] = i;
        }
    }
    o_sort(d, nd);
    o_sort(e, ne);
    const unsigned size = D.bank->count < 5 ? D.bank->count : 5u;
    unsigned n_new = nd >= 4 ? 1u : 5u - nd;
    if (n_new > 3) n_new = 3;
    if (n_new > nn) n_new = nn;
    unsigned n_rev = nd < 5u - n_new ? nd : 5u - n_new;
    unsigned count = 0;
    for (unsigned i = 0; i < n_rev && count < size; i++) out[count++] = d[i];
    for (unsigned i = 0; i < n_new && count < size; i++) out[count++] = n[i];
    for (unsigned i = 0; i < ne && count < size; i++) out[count++] = e[i];
    for (unsigned i = n_new; i < nn && count < size; i++) out[count++] = n[i];
    return count;
}

// Spec 4.6.
static vocab_format_t o_format(const vocab_card_t *c, bool *cloze_front)
{
    const vocab_entry_t *e = vocab_bank_at(D.bank, c->index);
    const uint8_t m = O[c->index].m;
    const bool cloze = vocab_entry_has_cloze(e);
    *cloze_front = false;
    switch (c->purpose) {
    case VOCAB_PURPOSE_LEARN:
        if (m == 0) return VOCAB_FORMAT_S1;  // every builtin entry has a lure
        if (m >= 3 && cloze) return VOCAB_FORMAT_S3;
        if (m >= 2) *cloze_front = vocab_entry_has_example_cloze(e) && qa_word_audio(e->uid);
        return VOCAB_FORMAT_S2;
    case VOCAB_PURPOSE_TEST_LATER:
        return VOCAB_FORMAT_S1;
    case VOCAB_PURPOSE_SPOT_CHECK:
        if (o_spot_slot < 0) return VOCAB_FORMAT_NONE;
        return cloze && OC[o_spot_slot].pre.m >= 2 ? VOCAB_FORMAT_S3 : VOCAB_FORMAT_S1;
    case VOCAB_PURPOSE_TOO_EASY_CHECK:
    default:
        return cloze ? VOCAB_FORMAT_S3 : VOCAB_FORMAT_S1;
    }
}

static vocab_badge_t o_badge(vocab_purpose_t purpose, vocab_format_t fmt)
{
    switch (purpose) {
    case VOCAB_PURPOSE_TEST_LATER: return VOCAB_BADGE_TEST_AGAIN;
    case VOCAB_PURPOSE_SPOT_CHECK: return VOCAB_BADGE_SPOT_CHECK;
    case VOCAB_PURPOSE_TOO_EASY_CHECK: return VOCAB_BADGE_TOO_EASY;
    case VOCAB_PURPOSE_LEARN:
    default:
        return fmt == VOCAB_FORMAT_S1 ? VOCAB_BADGE_NEW : fmt == VOCAB_FORMAT_S2 ? VOCAB_BADGE_RECALL
                                                                                   : VOCAB_BADGE_SPELL;
    }
}

// ---------------------------------------------------------------------------------------
// The child
// ---------------------------------------------------------------------------------------

static uint32_t roll(uint32_t n)
{
    return vocab_rng_below(&P, n);
}

static void think(void)
{
    qa_wait(300 + roll(900));  // always past the 250 ms UP/DOWN cooldown (R4)
}

static uint32_t press_ms(void)
{
    return 60 + roll(120);
}

static bool on_same_card(uint8_t pos)
{
    return qa_s()->screen == VOCAB_SCREEN_CARD && qa_s()->cur == pos;
}

// Verifies the insertion made by the deciding press of the card at `pos`.
static void check_later_insertion(uint8_t pos, uint8_t count_before, vocab_index_t idx, vocab_purpose_t purpose)
{
    const vocab_session_t *s = qa_s();
    const unsigned want = pos + 3u < count_before ? pos + 3u : count_before;
    CHECK(s->card_count == count_before + 1u && s->cards[want].purpose == purpose && s->cards[want].index == idx,
          "%s card for '%s' at %u of %u (spec 4.5, AC-18/AC-21)",
          purpose == VOCAB_PURPOSE_TEST_LATER ? "TEST_LATER" : "TOO_EASY_CHECK", D.bank->entries[idx].text, want,
          (unsigned)s->card_count);
}

typedef enum { CARD_DONE, CARD_LEFT } card_end_t;

// Pause and resume the same card (spec 3.2, AC-5).
static void pause_and_resume(uint8_t pos)
{
    vocab_view_t before = D.view;
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_PAUSE && D.view.badge == VOCAB_BADGE_PAUSE, "OK LONG pauses (AC-5)");
    qa_wait(300);
    qa_tap(roll(2) ? VOCAB_KEY_UP : VOCAB_KEY_OK, press_ms());
    CHECK(on_same_card(pos) && strcmp(D.view.card.word.text, before.card.word.text) == 0 &&
              strcmp(D.view.panel[0].text, before.panel[0].text) == 0 &&
              strcmp(D.view.panel[1].text, before.panel[1].text) == 0 &&
              D.view.card.skeleton.visible == before.card.skeleton.visible,
          "UP/OK resume the same card (AC-5)");
    st.pauses++;
}

static card_end_t abandon(uint32_t how)
{
    if (how == 0) {
        qa_hold(VOCAB_KEY_OK, 700);  // pause -> HOME (spec 3.2)
        qa_wait(300);
        const vocab_tick_t tick = vocab_engine_persist(&D.eng).tick;
        qa_mark();
        qa_tap(VOCAB_KEY_DOWN, press_ms());
        CHECK(D.view.screen == VOCAB_SCREEN_HOME && qa_count(VOCAB_CMD_SAVE) >= 1 &&
                  vocab_engine_persist(&D.eng).tick == tick, "pause -> HOME saves, no tick (AC-5, AC-10)");
        OCN = 0;
        o_spot_slot = -1;
        o_check_all("after pause -> HOME");
        st.abandons++;
    } else if (how == 1) {
        qa_wait(VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + 1000);  // walk away: deep sleep
        CHECK(D.view.screen == VOCAB_SCREEN_HOME, "deep sleep + key wake -> HOME");
        st.sleeps_mid++;
    } else {
        qa_power_cycle(5000);  // battery pulled: back to the last saved state
        st.power_loss++;
    }
    return CARD_LEFT;
}

static card_end_t play_s1(uint8_t pos, const vocab_card_t *card, bool was_seen)
{
    const vocab_session_t *s = qa_s();
    const uint8_t count_before = s->card_count;
    if (card->purpose == VOCAB_PURPOSE_LEARN && roll(100) < 7) {
        qa_wait(200 + roll(600));
        qa_hold(VOCAB_KEY_UP, 600 + roll(400));  // too easy (O12)
        CHECK(D.view.screen == VOCAB_SCREEN_BANNER && strcmp(D.view.card.banner.text, VOCAB_STR_TOO_EASY_ACK) == 0,
              "too-easy acknowledgement (AC-21)");
        o_apply(card->index, card->purpose, VOCAB_FORMAT_S1, VOCAB_RESULT_TOO_EASY, was_seen);
        check_later_insertion(pos, count_before, card->index, VOCAB_PURPOSE_TOO_EASY_CHECK);
        st.too_easy++;
        return CARD_DONE;
    }
    think();
    if (roll(100) < 10) {
        qa_mark();
        qa_tap(VOCAB_KEY_OK, press_ms());  // replay
        CHECK(on_same_card(pos), "OK replays without answering");
        think();
    }
    const vocab_key_t right = qa_correct_key();
    const bool good = roll(100) < 75;
    qa_tap(good ? right : qa_other_key(right), press_ms());
    o_apply(card->index, card->purpose, VOCAB_FORMAT_S1, good ? VOCAB_RESULT_CORRECT_FIRST : VOCAB_RESULT_WRONG,
            was_seen);
    o_check_all("Stage 1 outcome");
    if (!good) {
        CHECK(on_same_card(pos) && qa_locked() && D.view.badge == VOCAB_BADGE_REMEMBER &&
                  !D.view.panel[qa_panel_of(qa_other_key(right))].visible &&
                  D.view.panel[qa_panel_of(right)].style == VOCAB_PANEL_CORRECT,
              "error hold (spec 5.3)");
        qa_wait(VOCAB_ERROR_HOLD_MS + 300);
        if (roll(100) < 30) {
            qa_tap(qa_other_key(right), press_ms());  // nudge only
            CHECK(on_same_card(pos), "the other key only nudges (AC-15)");
            qa_wait(300);
        }
        qa_tap(right, press_ms());
    }
    qa_settle();
    return CARD_DONE;
}

static card_end_t play_s2(uint8_t pos, const vocab_card_t *card, bool was_seen)
{
    const vocab_session_t *s = qa_s();
    const uint8_t count_before = s->card_count;
    if (card->purpose == VOCAB_PURPOSE_LEARN && roll(100) < 8) {
        qa_wait(100 + roll(1500));
        qa_hold(VOCAB_KEY_UP, 600 + roll(400));  // too easy on the front (O12)
        CHECK(D.view.screen == VOCAB_SCREEN_BANNER, "too easy on the Stage 2 front (AC-21)");
        o_apply(card->index, card->purpose, VOCAB_FORMAT_S2, VOCAB_RESULT_TOO_EASY, was_seen);
        check_later_insertion(pos, count_before, card->index, VOCAB_PURPOSE_TOO_EASY_CHECK);
        st.too_easy++;
        return CARD_DONE;
    }
    qa_wait(VOCAB_S2_FRONT_MIN_MS + roll(600));
    static const vocab_key_t keys[3] = { VOCAB_KEY_UP, VOCAB_KEY_DOWN, VOCAB_KEY_OK };
    qa_mark();
    qa_tap(keys[roll(3)], press_ms());  // any key flips
    const int flip = qa_find(VOCAB_CMD_PLAY_CUE, 0) >= 0 ? qa_find(VOCAB_CMD_PLAY_CUE, 0)
                                                          : qa_find(VOCAB_CMD_PLAY_CUE_THEN_WORD, 0);
    CHECK(on_same_card(pos) && D.view.panel[0].visible && D.view.panel[1].visible && D.view.card.meaning.visible,
          "flip to the back (AC-17)");
    if (D.audio_ok && D.volume_pct > 0) CHECK(flip >= 0 && D.log[flip].cue == VOCAB_CUE_FLIP, "flip cue (AC-17)");
    think();
    const bool know = roll(100) < 72;
    qa_tap(know ? VOCAB_KEY_UP : VOCAB_KEY_DOWN, press_ms());
    o_apply(card->index, card->purpose, VOCAB_FORMAT_S2, know ? VOCAB_RESULT_KNOW : VOCAB_RESULT_FORGOT, was_seen);
    o_check_all("Stage 2 outcome");
    if (!know) {
        check_later_insertion(pos, count_before, card->index, VOCAB_PURPOSE_TEST_LATER);
        st.test_later++;
    } else if (OCN == 3) {
        // AC-19: the third claim inserts a spot check as the next card, targeting one of the
        // earlier claims (never the most recent one).
        CHECK(s->card_count == count_before + 1u && s->cards[pos + 1].purpose == VOCAB_PURPOSE_SPOT_CHECK,
              "spot check inserted next (AC-19)");
        o_spot_slot = -1;
        for (unsigned k = 0; k + 1 < OCN; k++) {
            if (OC[k].idx == s->cards[pos + 1].index && OC[k].idx != OC[OCN - 1].idx) o_spot_slot = (int)k;
        }
        CHECK(o_spot_slot >= 0, "spot-check target '%s' is one of the earlier claims (AC-19)",
              D.bank->entries[s->cards[pos + 1].index].text);
        if (o_spot_slot < 0) OCN = 0;
        st.spot++;
    }
    qa_settle();
    return CARD_DONE;
}

static card_end_t play_s3(uint8_t pos, const vocab_card_t *card, bool was_seen)
{
    think();
    if (roll(100) < 10) {
        qa_tap(VOCAB_KEY_OK, press_ms());  // replay
        think();
    }
    const vocab_key_t right = qa_correct_key();
    const vocab_key_t wrong = qa_other_key(right);
    if (card->purpose != VOCAB_PURPOSE_LEARN) {
        const bool good = roll(100) < 70;
        qa_tap(good ? right : wrong, press_ms());
        o_apply(card->index, card->purpose, VOCAB_FORMAT_S3, good ? VOCAB_RESULT_CORRECT_FIRST : VOCAB_RESULT_WRONG,
                was_seen);
        o_check_all("Stage 3 check card outcome");
        if (!good) {
            CHECK(D.view.card.skeleton.filled && D.view.badge == VOCAB_BADGE_REMEMBER,
                  "check card: the first miss reveals (AC-24)");
            qa_wait(VOCAB_ERROR_HOLD_MS + 300);
            qa_tap(right, press_ms());
        }
        qa_settle();
        return CARD_DONE;
    }
    const uint32_t r = roll(100);
    if (r < 60) {
        qa_tap(right, press_ms());
        o_apply(card->index, card->purpose, VOCAB_FORMAT_S3, VOCAB_RESULT_CORRECT_FIRST, was_seen);
        o_check_all("O7");
        CHECK(D.view.card.skeleton.filled, "success shows the full word");
    } else {
        const uint8_t shakes = D.view.anim.shake_seq;
        qa_tap(wrong, press_ms());
        CHECK(on_same_card(pos) && qa_locked() && !D.view.card.skeleton.filled && D.view.anim.shake_seq != shakes &&
                  D.view.panel[0].visible && D.view.panel[1].visible, "miss 1: shake, retry lock (AC-24)");
        o_check_all("no outcome after miss 1");
        qa_wait(VOCAB_S3_RETRY_LOCK_MS + 350);
        const bool good = r < 85;
        qa_tap(good ? right : wrong, press_ms());
        o_apply(card->index, card->purpose, VOCAB_FORMAT_S3,
                good ? VOCAB_RESULT_CORRECT_SECOND : VOCAB_RESULT_WRONG, was_seen);
        o_check_all(good ? "O8" : "O9");
        if (!good) {
            CHECK(D.view.card.skeleton.filled && D.view.badge == VOCAB_BADGE_REMEMBER &&
                      !D.view.panel[qa_panel_of(wrong)].visible, "miss 2: reveal hold (AC-24)");
            qa_wait(VOCAB_ERROR_HOLD_MS + 300);
            qa_tap(right, press_ms());
        }
    }
    qa_settle();
    return CARD_DONE;
}

static card_end_t play_card(void)
{
    const vocab_session_t *s = qa_s();
    const uint8_t pos = s->cur;
    const vocab_card_t card = s->cards[pos];
    const vocab_format_t fmt = s->stage_format;
    bool cloze_front = false;
    const vocab_format_t want = o_format(&card, &cloze_front);
    const bool was_seen = O[card.index].seen;
    O[card.index].seen = true;  // spec 4.1: seen when first shown
    st.cards++;
    st.fmt[fmt]++;
    CHECK(fmt == want, "'%s' purpose %d: format %d, oracle %d (AC-12)", D.bank->entries[card.index].text,
          (int)card.purpose, (int)fmt, (int)want);
    CHECK(D.view.badge == o_badge(card.purpose, fmt), "badge %d for purpose %d format %d", (int)D.view.badge,
          (int)card.purpose, (int)fmt);
    if (fmt == VOCAB_FORMAT_S2) {
        CHECK(D.view.card.sentence.visible == cloze_front, "'%s' front kind: cloze %d, oracle %d (AC-12)",
              D.bank->entries[card.index].text, (int)D.view.card.sentence.visible, (int)cloze_front);
        st.cloze_fronts += cloze_front;
    }
    o_check_all("card start");

    const uint32_t r = roll(1000);
    if (r < 12) return abandon(0);
    if (r < 18) return abandon(1);
    if (r < 24) return abandon(2);
    if (r < 70 && fmt != VOCAB_FORMAT_S2) pause_and_resume(pos);
    if (r >= 70 && r < 100) {
        qa_hold(VOCAB_KEY_DOWN, 600);  // volume (AC-33): never answers
        CHECK(on_same_card(pos) && !qa_s()->outcome_seen, "DOWN LONG never selects the DOWN option (AC-33)");
        qa_wait(200);
    }
    switch (fmt) {
    case VOCAB_FORMAT_S1: return play_s1(pos, &card, was_seen);
    case VOCAB_FORMAT_S2: return play_s2(pos, &card, was_seen);
    case VOCAB_FORMAT_S3: return play_s3(pos, &card, was_seen);
    case VOCAB_FORMAT_NONE:
    default:
        CHECK(false, "card without a format");
        qa_recover_to_home();
        return CARD_LEFT;
    }
}

// ---------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------

static void on_summary(void)
{
    unsigned newly = 0;
    for (unsigned b = 0; b < OBASEN; b++) newly += !OBASE_EVER[b] && O[OBASE[b]].ever;
    const uint32_t gain = 5u + O_FIRST + 2u * newly;
    const uint8_t old_level = o_level_for(OXP);
    OXP += gain;
    OT = sat16((uint32_t)OT + 1u);
    const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
    CHECK(p.tick == OT, "tick %u after SUMMARY, oracle %u (AC-10)", (unsigned)p.tick, (unsigned)OT);
    CHECK(p.xp == OXP, "XP %u after SUMMARY, oracle %u (+%u: 5 + %u first-try + 2 x %u mastered) (AC-56)",
          (unsigned)p.xp, (unsigned)OXP, (unsigned)gain, O_FIRST, newly);
    OXP = p.xp;
    char text[64];
    if (O_OBJ > 0) {
        snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_SCORE_FMT, O_FIRST, O_OBJ);
    } else {
        snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_LEARNED_FMT, OBASEN);
    }
    CHECK(strcmp(D.view.summary.score.text, text) == 0, "SUMMARY score '%s' (want '%s')", D.view.summary.score.text,
          text);
    snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_XP_FMT, (unsigned)gain);
    CHECK(strcmp(D.view.summary.xp.text, text) == 0, "SUMMARY XP '%s' (want '%s')", D.view.summary.xp.text, text);
    const uint8_t new_level = o_level_for(OXP);
    if (new_level > old_level) {
        static const char *const titles[10] = VOCAB_STR_TITLES_INIT;
        snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_LEVELUP_FMT, titles[new_level - 1]);
        CHECK(D.view.summary.levelup.visible && strcmp(D.view.summary.levelup.text, text) == 0 && D.view.pet.level_up,
              "level-up line '%s' (AC-56)", text);
        st.levelups++;
    } else {
        CHECK(!D.view.summary.levelup.visible && !D.view.pet.level_up, "no level-up line without a level up");
    }
    o_check_all("SUMMARY");
    OCN = 0;
    o_spot_slot = -1;
    st.summaries++;
    if (OT > st.max_tick) st.max_tick = OT;
}

// Starts a session with OK (from HOME or after the SUMMARY lock) and checks the composition.
static bool start_session(void)
{
    vocab_index_t want[5];
    const unsigned n = o_compose(want);
    const vocab_tick_t tick = vocab_engine_persist(&D.eng).tick;
    CHECK(vocab_engine_power(&D.eng) != VOCAB_POWER_OFF, "start_session with the screen off");
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, press_ms());
    const vocab_session_t *s = qa_s();
    if (D.view.screen != VOCAB_SCREEN_CARD) {
        CHECK(false, "OK did not start a session (screen %d) (AC-4)", (int)D.view.screen);
        return false;
    }
    CHECK(vocab_engine_persist(&D.eng).tick == tick, "starting a session never changes the tick");
    bool same = s->base_count == n;
    for (unsigned i = 0; same && i < n; i++) {
        bool found = false;
        for (unsigned j = 0; j < s->base_count; j++) found = found || s->base_index[j] == want[i];
        same = found;
    }
    CHECK(same, "session %u composition differs from spec 4.4 (tick %u, %u words) (AC-7)", st.sessions,
          (unsigned)OT, n);
    OBASEN = s->base_count;
    for (unsigned i = 0; i < OBASEN; i++) {
        OBASE[i] = s->base_index[i];
        OBASE_EVER[i] = O[OBASE[i]].ever;
    }
    O_FIRST = 0;
    O_OBJ = 0;
    OCN = 0;
    o_spot_slot = -1;
    st.sessions++;
    return true;
}

// Plays the session until SUMMARY (true) or until it is left (false).
static bool play_session(void)
{
    for (int guard = 0; guard < 40 && !too_many_failures(); guard++) {
        qa_settle();
        switch (D.view.screen) {
        case VOCAB_SCREEN_CARD:
            if (play_card() == CARD_LEFT) return false;
            break;
        case VOCAB_SCREEN_SUMMARY:
            on_summary();
            return true;
        case VOCAB_SCREEN_HOME:
            return false;
        default:
            qa_wait(100);
            break;
        }
    }
    CHECK(false, "session did not end");
    return false;
}

// Things a child does between sessions.
static void between_sessions(void)
{
    const uint32_t r = roll(100);
    if (r < 12) {
        // Screen off at 30 s, then a key only wakes (AC-30).
        qa_wait(VOCAB_IDLE_OFF_MS + 2000);
        CHECK(D.backlight == 0, "screen off on HOME after 30 s");
        qa_tap((vocab_key_t)roll(3), press_ms());
        qa_wait(450);
        CHECK(D.view.screen == VOCAB_SCREEN_HOME && D.backlight == VOCAB_BACKLIGHT_ON, "wake only, HOME kept");
        st.idle_wakes++;
    } else if (r < 20) {
        qa_wait(VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + 500);  // deep sleep, key wake
        CHECK(D.view.screen == VOCAB_SCREEN_HOME, "after deep sleep: HOME");
    } else if (r < 25) {
        qa_power_cycle(30000);
        st.power_loss++;
    } else if (r < 32) {
        // A new battery reading (incl. unavailable and <= 5 %) at the next 30 s poll; one
        // ignored UP click keeps the screen on.
        D.battery = (int)roll(102) - 1;
        qa_wait(15000);
        qa_tap(VOCAB_KEY_UP, press_ms());
        qa_wait(15100);
        CHECK(D.battery_reported == D.battery, "battery polled every 30 s (spec 9)");
    } else if (r < 36) {
        qa_hold(VOCAB_KEY_DOWN, 700);  // volume on HOME
        qa_wait(VOCAB_TOAST_MS + 100);
    }
}

static void run_days(uint32_t seed, unsigned sessions, bool check_first_order)
{
    memset(&st, 0, sizeof(st));
    D.on_boot = on_boot;
    qa_power_on_fresh(seed);
    vocab_rng_seed(&P, seed * 2654435761u + 17u);
    if (check_first_order) {
        // AC-6 at system level: fresh records, tick 0 -> bank words 0..4 in the order of the
        // boot seed's shuffle (no randomness is used before the first session).
        vocab_index_t expect[5] = { 0, 1, 2, 3, 4 };
        vocab_rng_t ref;
        vocab_rng_seed(&ref, seed);
        vocab_rng_shuffle(&ref, expect, 5);
        if (start_session()) {
            const vocab_session_t *s = qa_s();
            bool same = s->card_count == 5;
            for (unsigned i = 0; same && i < 5; i++) same = s->cards[i].index == expect[i];
            CHECK(same, "fresh device: first session order follows the seeded shuffle (AC-6)");
            CHECK(strcmp(D.view.status.progress.text, "1/5") == 0 && qa_s()->stage_format == VOCAB_FORMAT_S1,
                  "first card: 1/5 in Stage 1");
            play_session();
        }
    }
    while (st.sessions < sessions && !too_many_failures()) {
        if (D.view.screen == VOCAB_SCREEN_SUMMARY) {
            qa_wait(VOCAB_SUMMARY_LOCK_MS + 100);
            if (roll(2)) {
                if (!start_session()) qa_recover_to_home();
                if (!play_session() && D.view.screen != VOCAB_SCREEN_HOME) qa_recover_to_home();
                continue;
            }
            qa_tap(roll(2) ? VOCAB_KEY_UP : VOCAB_KEY_DOWN, press_ms());
            CHECK(D.view.screen == VOCAB_SCREEN_HOME, "UP/DOWN on SUMMARY -> HOME (AC-4)");
        }
        if (D.view.screen != VOCAB_SCREEN_HOME) qa_recover_to_home();
        between_sessions();
        if (D.view.screen != VOCAB_SCREEN_HOME) qa_recover_to_home();
        if (!start_session()) {
            qa_recover_to_home();
            continue;
        }
        if (!play_session() && D.view.screen != VOCAB_SCREEN_HOME) qa_recover_to_home();
    }
    printf("QA days seed %u: %u sessions (%u completed, tick 0 -> %u), %u cards (S1 %u, S2 %u [cloze fronts %u], "
           "S3 %u), spot checks %u (failed %u), too easy %u (passed %u), test-later %u, pauses %u, abandoned %u, "
           "deep sleeps mid-card %u, power losses %u, level-ups %u, wakes %u, reboots %u (restores checked %u), "
           "saves %u\n",
           (unsigned)seed, st.sessions, st.summaries, st.max_tick, st.cards, st.fmt[1], st.fmt[2], st.cloze_fronts,
           st.fmt[3], st.spot, st.spot_fail, st.too_easy, st.too_easy_pass, st.test_later, st.pauses, st.abandons,
           st.sleeps_mid, st.power_loss, st.levelups, st.idle_wakes, D.boots - 1u, D.restores_checked, D.saves_ok);
    printf("QA days seed %u: outcome rows O1..O14 exercised:", (unsigned)seed);
    for (int i = 1; i <= 14; i++) printf(" %u", st.o_rows[i]);
    printf("\n");
    for (int i = 1; i <= 14; i++) CHECK(st.o_rows[i] > 0, "row O%d was never exercised (seed %u)", i, (unsigned)seed);
    CHECK(st.summaries >= sessions / 2, "most sessions completed");
}

int main(void)
{
    run_days(101, 60, true);
    run_days(202, 60, true);
    run_days(303, 45, false);
    qa_report_known_defects();
    return vocab_test_report("vocab QA multi-day system test");
}
