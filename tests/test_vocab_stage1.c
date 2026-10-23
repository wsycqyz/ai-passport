// tests/test_vocab_stage1.c - Stage 1 two-choice meaning card through the vocab_stage.h
// contract (spec 5.1, 5.3, 3.2, 4.3 result values, 4.8, 8.3, 11.5): view model per state
// (texts, colors, panel styles, hidden flags, badges, hints), outbox contents (audio,
// timers, epochs), AC-13 side balance, AC-15 error-hold timing and keys, AC-21/AC-22 (Stage 1
// part of "too easy"), AC-27 for Stage 1 views, resume, unusable input, and a randomized
// event fuzz checked against a reference model of the card.
//
// Owner: Stage 1 developer. Compiled by tools/validate.sh with:
//   tests/test_vocab_stage1.c main/vocab_stage1.c main/vocab_lure.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
// Deterministic (fixed seeds). The harness keeps the stage timers the way vocab_session.c
// does: absolute deadlines (`now` of the arming call + ms), one shot, re-arming replaces.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_lure.h"
#include "vocab_stage1.h"
#include "vocab_strings.h"
#include "vocab_test.h"

#define T_START 100000u  // `now` of every begin()
#define SEEDS 10000u
#define FUZZ_CARDS 6000u

typedef enum { CALL_BEGIN, CALL_HANDLE, CALL_RESUME } call_t;

// Reference model of one card, advanced by step() from the predicted effect.
typedef enum { M_QUESTION, M_FEEDBACK, M_HOLD_LOCK, M_HOLD_WAIT, M_DONE } model_t;

// Exact effect of one call predicted from the model (spec 5.3, 3.2, 4.8).
typedef enum {
    E_NOTHING,       // no command, no view change, same status, no outcome
    E_REPLAY,        // PLAY_WORD only
    E_NUDGE,         // nudge_seq[correct] + 1 only
    E_CORRECT,       // first press correct -> feedback lock
    E_WRONG,         // first press wrong -> error-hold lock
    E_TOO_EASY,      // UP LONG on a LEARN question -> DONE with TOO_EASY
    E_REARM,         // timer before the deadline -> re-arm the rest
    E_FEEDBACK_END,  // feedback timer -> DONE
    E_HOLD_END,      // hold timer -> wait part
    E_FINISH,        // wait part, correct key -> DONE
    E_RESUME_LOCK,   // resume inside a lock -> a full window is re-armed
} effect_t;

typedef struct {
    const vocab_bank_t *bank;
    const vocab_entry_t *entry;
    const vocab_entry_t *lure;
    vocab_purpose_t purpose;
    bool allow_too_easy;
    vocab_side_t correct;  // read from the view after begin()
    vocab_rng_t rng;
    vocab_stage1_t st;
    vocab_view_t view;
    vocab_outbox_t out;
    vocab_stage_result_t res;
    vocab_ms_t now;
    bool armed[VOCAB_STAGE_TIMER_COUNT];
    vocab_ms_t deadline[VOCAB_STAGE_TIMER_COUNT];
    model_t model;
    vocab_ms_t lock_deadline;  // model: end of the running lock window
    int outcomes;
    vocab_result_t outcome;
    vocab_ms_t outcome_at;
    bool done;
    char ctx[80];
} fx_t;

static fx_t F;
static unsigned s_effects[E_RESUME_LOCK + 1];  // how often each effect was exercised

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

static vocab_index_t index_of(vocab_uid_t uid)
{
    vocab_index_t index = VOCAB_INDEX_NONE;
    (void)vocab_bank_find(vocab_bank_builtin(), uid, &index);
    return index;
}

static bool has_letter(const char *s)
{
    for (; *s; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) return true;
    }
    return false;
}

static bool same_view(const vocab_view_t *a, const vocab_view_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static vocab_cmd_t cmd_make(vocab_cmd_kind_t kind, vocab_cue_t cue, vocab_uid_t uid,
                            uint32_t value)
{
    const vocab_cmd_t c = { kind, cue, VOCAB_TIMER_STAGE, uid, value };
    return c;
}

static vocab_cmd_t cmd_word(vocab_uid_t uid)
{
    return cmd_make(VOCAB_CMD_PLAY_WORD, VOCAB_CUE_NONE, uid, 0);
}

static vocab_cmd_t cmd_cue(vocab_cue_t cue)
{
    return cmd_make(VOCAB_CMD_PLAY_CUE, cue, VOCAB_UID_NONE, 0);
}

static vocab_cmd_t cmd_cue_word(vocab_cue_t cue, vocab_uid_t uid)
{
    return cmd_make(VOCAB_CMD_PLAY_CUE_THEN_WORD, cue, uid, 0);
}

static vocab_cmd_t cmd_timer(uint32_t ms)
{
    return cmd_make(VOCAB_CMD_TIMER_START, VOCAB_CUE_NONE, VOCAB_UID_NONE, ms);
}

static vocab_cmd_t cmd_epoch(void)
{
    return cmd_make(VOCAB_CMD_NEW_EPOCH, VOCAB_CUE_NONE, VOCAB_UID_NONE, 0);
}

// True when the outbox holds exactly `want` (kinds and the fields relevant per kind).
static bool outbox_is(const vocab_outbox_t *out, const vocab_cmd_t *want, size_t n)
{
    if (out->overflow || out->count != n) return false;
    for (size_t i = 0; i < n; i++) {
        const vocab_cmd_t *c = &out->cmd[i];
        if (c->kind != want[i].kind) return false;
        switch (c->kind) {
        case VOCAB_CMD_PLAY_WORD:
            if (c->uid != want[i].uid) return false;
            break;
        case VOCAB_CMD_PLAY_CUE:
            if (c->cue != want[i].cue) return false;
            break;
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            if (c->cue != want[i].cue || c->uid != want[i].uid) return false;
            break;
        case VOCAB_CMD_TIMER_START:
            if (c->timer != want[i].timer || c->value != want[i].value) return false;
            break;
        default:
            break;
        }
    }
    return true;
}

static vocab_badge_t expected_badge(vocab_purpose_t purpose)
{
    switch (purpose) {
    case VOCAB_PURPOSE_TEST_LATER: return VOCAB_BADGE_TEST_AGAIN;
    case VOCAB_PURPOSE_SPOT_CHECK: return VOCAB_BADGE_SPOT_CHECK;
    case VOCAB_PURPOSE_TOO_EASY_CHECK: return VOCAB_BADGE_TOO_EASY;
    case VOCAB_PURPOSE_LEARN:
    default:
        return VOCAB_BADGE_NEW;
    }
}

// ---------------------------------------------------------------------------------------
// View checks
// ---------------------------------------------------------------------------------------

// Session-owned parts get recognizable values; no stage call may change them.
static void seed_session_region(vocab_view_t *v)
{
    v->screen = VOCAB_SCREEN_CARD;
    vocab_view_show(&v->status.progress, "3/7", VOCAB_RGB_DIM);
    v->status.volume = VOCAB_VOL_ICON_LOW;
    v->status.battery = VOCAB_BATT_ICON_2;
    v->status.battery_low = true;
    v->hint.toast = VOCAB_TOAST_VOL_LOW;
    vocab_text_copy(v->hint.text, sizeof(v->hint.text), VOCAB_STR_TOAST_VOL_LOW);
    v->hint.color = VOCAB_RGB_TEXT;
    v->anim.pulse_seq = 7;
    v->anim.pet_seq = 3;
    v->anim.shake_seq = 11;
    v->anim.confirm_seq[0] = 5;
    v->anim.confirm_seq[1] = 9;
    v->anim.nudge_seq[0] = 254;  // near the wrap: counters are compared modulo 256
    v->anim.nudge_seq[1] = 255;
    v->xpbar.value = 4;
    v->xpbar.max = 30;
}

static void check_session_region(const fx_t *f, const vocab_view_t *b)
{
    const vocab_view_t *a = &f->view;
    CHECK(a->screen == b->screen, "%s: screen untouched", f->ctx);
    CHECK(memcmp(&a->status, &b->status, sizeof(a->status)) == 0, "%s: status bar untouched",
          f->ctx);
    CHECK(a->hint.toast == b->hint.toast && a->hint.color == b->hint.color &&
              memcmp(a->hint.text, b->hint.text, sizeof(a->hint.text)) == 0,
          "%s: resolved hint and toast untouched", f->ctx);
    CHECK(memcmp(&a->card.banner, &b->card.banner, sizeof(a->card.banner)) == 0,
          "%s: banner untouched", f->ctx);
    CHECK(memcmp(&a->home, &b->home, sizeof(a->home)) == 0 &&
              memcmp(&a->summary, &b->summary, sizeof(a->summary)) == 0 &&
              memcmp(&a->pet, &b->pet, sizeof(a->pet)) == 0 &&
              memcmp(&a->xpbar, &b->xpbar, sizeof(a->xpbar)) == 0,
          "%s: HOME, SUMMARY, pet and XP bar untouched", f->ctx);
    CHECK(a->anim.pulse_seq == b->anim.pulse_seq && a->anim.pet_seq == b->anim.pet_seq &&
              a->anim.shake_seq == b->anim.shake_seq &&
              a->anim.confirm_seq[0] == b->anim.confirm_seq[0] &&
              a->anim.confirm_seq[1] == b->anim.confirm_seq[1],
          "%s: only nudge counters may change", f->ctx);
}

// AC-27 for Stage 1: English appears only as the entry text (word label) and its IPA; the
// options are Chinese meanings (the entry's and the lure's) and never English.
static void check_english(const fx_t *f)
{
    const vocab_view_card_t *c = &f->view.card;
    CHECK(!has_letter(c->word.text) || strcmp(c->word.text, f->entry->text) == 0,
          "%s: the word label holds only the entry text ('%s')", f->ctx, c->word.text);
    CHECK(!has_letter(c->ipa.text) || (f->entry->ipa && strcmp(c->ipa.text, f->entry->ipa) == 0),
          "%s: the IPA label holds only the entry IPA", f->ctx);
    const vocab_view_label_t *chinese_only[] = {
        &c->meaning, &c->sentence, &c->example, &c->s3_meaning, &c->prompt, &c->banner,
    };
    for (size_t i = 0; i < sizeof(chinese_only) / sizeof(chinese_only[0]); i++) {
        CHECK(!has_letter(chinese_only[i]->text), "%s: no English in label %u ('%s')", f->ctx,
              (unsigned)i, chinese_only[i]->text);
    }
    CHECK(!has_letter(c->skeleton.pre) && !has_letter(c->skeleton.suf) &&
              !has_letter(c->skeleton.right) && !has_letter(c->skeleton.other),
          "%s: no Stage 3 skeleton parts", f->ctx);
    for (int side = 0; side < 2; side++) {
        const vocab_view_panel_t *p = &f->view.panel[side];
        CHECK(!p->text_en && !has_letter(p->text), "%s: panel %d holds Chinese only ('%s')",
              f->ctx, side, p->text);
        if (p->visible) {
            CHECK(strcmp(p->text, f->entry->zh) == 0 || strcmp(p->text, f->lure->zh) == 0,
                  "%s: panel %d shows the meaning or the lure meaning", f->ctx, side);
        }
    }
}

// The whole stage region for the model state (spec 11.5 rows "Stage 1", "Stage 1 correct",
// "Error hold (Stage 1)", "Test-later card", "Spot-check card", "Too-easy ... card").
static void check_view(const fx_t *f)
{
    if (f->model == M_DONE) return;
    const vocab_view_t *v = &f->view;
    const vocab_view_card_t *c = &v->card;
    const vocab_view_panel_t *right = &v->panel[f->correct];
    const vocab_view_panel_t *other = &v->panel[vocab_side_other(f->correct)];
    const bool hold = f->model == M_HOLD_LOCK || f->model == M_HOLD_WAIT;
    const char *ctx = f->ctx;

    CHECK(c->frame, "%s: card frame shown", ctx);
    CHECK(c->word.visible && c->word.color == VOCAB_RGB_TEXT &&
              strcmp(c->word.text, f->entry->text) == 0,
          "%s: word shown in white", ctx);
    if (vocab_entry_has_ipa(f->entry)) {
        CHECK(c->ipa.visible && c->ipa.color == VOCAB_RGB_DIM &&
                  strcmp(c->ipa.text, f->entry->ipa) == 0,
              "%s: grey IPA line", ctx);
    } else {
        CHECK(!c->ipa.visible && c->ipa.text[0] == '\0', "%s: IPA line hidden", ctx);
    }
    CHECK(!c->meaning.visible && !c->sentence.visible && !c->example.visible &&
              !c->s3_meaning.visible && !c->skeleton.visible && !c->banner.visible,
          "%s: labels of other stages hidden", ctx);

    if (f->model == M_QUESTION) {
        CHECK(c->prompt.visible && c->prompt.color == VOCAB_RGB_DIM &&
                  strcmp(c->prompt.text, VOCAB_STR_S1_PROMPT) == 0,
              "%s: question prompt", ctx);
    } else if (f->model == M_FEEDBACK) {
        CHECK(c->prompt.visible && c->prompt.color == VOCAB_RGB_SUCCESS &&
                  strcmp(c->prompt.text, VOCAB_STR_CORRECT) == 0,
              "%s: correct prompt in green", ctx);
    } else {
        CHECK(c->prompt.visible && c->prompt.color == VOCAB_RGB_YELLOW &&
                  strcmp(c->prompt.text, VOCAB_STR_HOLD_MEANING) == 0,
              "%s: hold prompt in yellow", ctx);
    }

    CHECK(right->visible && !right->text_en && strcmp(right->text, f->entry->zh) == 0,
          "%s: the correct panel shows the meaning", ctx);
    CHECK(right->style == (f->model == M_QUESTION ? VOCAB_PANEL_S1 : VOCAB_PANEL_CORRECT),
          "%s: correct panel style %d", ctx, (int)right->style);
    CHECK(right->nudge_repeat == (f->model == M_HOLD_WAIT), "%s: nudge only in the wait part",
          ctx);
    if (hold) {
        CHECK(!other->visible && other->text[0] == '\0' && !other->nudge_repeat,
              "%s: the wrong panel is hidden in the hold", ctx);
    } else {
        CHECK(other->visible && !other->text_en && other->style == VOCAB_PANEL_S1 &&
                  !other->nudge_repeat && strcmp(other->text, f->lure->zh) == 0,
              "%s: the other panel shows the lure meaning in the S1 style", ctx);
    }

    CHECK(v->badge == (hold ? VOCAB_BADGE_REMEMBER : expected_badge(f->purpose)),
          "%s: badge %d", ctx, (int)v->badge);
    vocab_hint_t hint = VOCAB_HINT_REPLAY;
    if (f->model == M_HOLD_LOCK) {
        hint = VOCAB_HINT_NONE;
    } else if (f->model == M_HOLD_WAIT) {
        hint = f->correct == VOCAB_SIDE_UP ? VOCAB_HINT_PRESS_UP : VOCAB_HINT_PRESS_DOWN;
    }
    CHECK(v->hint.kind == hint, "%s: hint %d (expected %d)", ctx, (int)v->hint.kind, (int)hint);
}

// ---------------------------------------------------------------------------------------
// Harness: calls with generic checks, a session-like timer table, the reference model
// ---------------------------------------------------------------------------------------

// Stage 1 emits only these commands (vocab_stage.h rules 4, 6, 7).
static void check_outbox(const fx_t *f, call_t kind)
{
    CHECK(!f->out.overflow, "%s: outbox overflow", f->ctx);
    for (uint8_t i = 0; i < f->out.count; i++) {
        const vocab_cmd_t *c = &f->out.cmd[i];
        switch (c->kind) {
        case VOCAB_CMD_PLAY_WORD:
            CHECK(kind != CALL_RESUME && c->uid == f->entry->uid,
                  "%s: PLAY_WORD of the card's word only, never in resume", f->ctx);
            break;
        case VOCAB_CMD_PLAY_CUE:
            CHECK(kind == CALL_HANDLE && c->cue == VOCAB_CUE_CORRECT,
                  "%s: the only plain cue is CUE_CORRECT", f->ctx);
            break;
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            CHECK(kind == CALL_HANDLE && c->cue == VOCAB_CUE_WRONG && c->uid == f->entry->uid,
                  "%s: CUE_WRONG then the card's word", f->ctx);
            break;
        case VOCAB_CMD_TIMER_START:
            CHECK(kind != CALL_BEGIN && c->timer == VOCAB_TIMER_STAGE && c->value >= 1u &&
                      c->value <= VOCAB_ERROR_HOLD_MS,
                  "%s: TIMER_START on the stage timer, 1..1500 ms (got timer %d, %u ms)", f->ctx,
                  (int)c->timer, (unsigned)c->value);
            break;
        case VOCAB_CMD_NEW_EPOCH:
            CHECK(kind == CALL_HANDLE, "%s: epochs only on key/timer changes, never in "
                                       "begin/resume", f->ctx);
            break;
        default:
            CHECK(false, "%s: unexpected command kind %d", f->ctx, (int)c->kind);
            break;
        }
    }
}

// Mirrors vocab_session.c: deadline = now of the arming call + ms; re-arming replaces.
static void apply_timers(fx_t *f)
{
    for (uint8_t i = 0; i < f->out.count; i++) {
        const vocab_cmd_t *c = &f->out.cmd[i];
        if (c->kind != VOCAB_CMD_TIMER_START && c->kind != VOCAB_CMD_TIMER_CANCEL) continue;
        if (!vocab_timer_is_stage(c->timer)) continue;
        if (c->kind == VOCAB_CMD_TIMER_START) {
            f->armed[c->timer] = true;
            f->deadline[c->timer] = f->now + (c->value ? c->value : 1u);
        } else {
            f->armed[c->timer] = false;
        }
    }
}

// Outcome bookkeeping (vocab_stage.h rule 3): exactly once per card, before or with DONE.
static void record(fx_t *f, vocab_stage_result_t r)
{
    if (r.outcome_ready) {
        CHECK(!f->done, "%s: no outcome after DONE", f->ctx);
        f->outcomes++;
        f->outcome = r.result;
        f->outcome_at = f->now;
        CHECK(f->outcomes == 1, "%s: outcome reported exactly once (count %d)", f->ctx,
              f->outcomes);
        CHECK(r.result == VOCAB_RESULT_CORRECT_FIRST || r.result == VOCAB_RESULT_WRONG ||
                  r.result == VOCAB_RESULT_TOO_EASY,
              "%s: Stage 1 result value %d", f->ctx, (int)r.result);
    }
    if (r.status == VOCAB_STAGE_DONE && !f->done) {
        CHECK(f->outcomes == 1, "%s: a finished card reported exactly one outcome", f->ctx);
        f->done = true;
    }
    f->res = r;
}

// One handle() (ev != NULL) or resume() (ev == NULL) call with the generic checks.
static vocab_stage_result_t call(fx_t *f, const vocab_event_t *ev, vocab_view_t *before)
{
    const bool was_done = f->done;
    const call_t kind = ev ? CALL_HANDLE : CALL_RESUME;
    memcpy(before, &f->view, sizeof(*before));
    vocab_outbox_clear(&f->out);
    vocab_stage_result_t r;
    if (ev) {
        // A delivered timer has fired; the stage re-arms it when it needs it again.
        if (ev->kind == VOCAB_EV_TIMER && vocab_timer_is_stage(ev->timer)) {
            f->armed[ev->timer] = false;
        }
        r = vocab_stage1_handle(&f->st, ev, f->now, &f->view, &f->out);
    } else {
        // The session cancels both stage timers before the pause card.
        f->armed[VOCAB_TIMER_STAGE] = false;
        f->armed[VOCAB_TIMER_STAGE_AUX] = false;
        r = vocab_stage1_resume(&f->st, f->now, &f->view, &f->out);
    }
    check_session_region(f, before);
    check_outbox(f, kind);
    apply_timers(f);
    if (was_done) {
        CHECK(r.status == VOCAB_STAGE_DONE && !r.outcome_ready && f->out.count == 0 &&
                  same_view(before, &f->view),
              "%s: calls after DONE are inert", f->ctx);
    }
    record(f, r);
    check_english(f);
    if (!f->done && r.locked) {
        CHECK(f->armed[VOCAB_TIMER_STAGE], "%s: never stuck: a lock always has its timer armed",
              f->ctx);
    }
    return r;
}

static effect_t predict(const fx_t *f, const vocab_event_t *ev)
{
    const model_t m = f->model;
    if (m == M_DONE) return E_NOTHING;
    const bool locked = m == M_FEEDBACK || m == M_HOLD_LOCK;
    if (!ev) return locked ? E_RESUME_LOCK : E_NOTHING;
    const vocab_key_t right = vocab_side_key(f->correct);
    const vocab_key_t wrong = vocab_side_key(vocab_side_other(f->correct));
    switch (ev->kind) {
    case VOCAB_EV_CLICK:
        if (m != M_QUESTION && m != M_HOLD_WAIT) return E_NOTHING;
        if (ev->key == VOCAB_KEY_OK) return E_REPLAY;
        if (ev->key == right) return m == M_QUESTION ? E_CORRECT : E_FINISH;
        if (ev->key == wrong) return m == M_QUESTION ? E_WRONG : E_NUDGE;
        return E_NOTHING;
    case VOCAB_EV_LONG:
        return m == M_QUESTION && ev->key == VOCAB_KEY_UP && f->allow_too_easy &&
                       f->purpose == VOCAB_PURPOSE_LEARN
                   ? E_TOO_EASY
                   : E_NOTHING;
    case VOCAB_EV_TIMER:
        if (ev->timer != VOCAB_TIMER_STAGE || !locked) return E_NOTHING;
        if (!vocab_ms_reached(f->now, f->lock_deadline)) return E_REARM;
        return m == M_FEEDBACK ? E_FEEDBACK_END : E_HOLD_END;
    case VOCAB_EV_NONE:
    default:
        return E_NOTHING;
    }
}

static void expect_status(const fx_t *f, vocab_stage_result_t r, vocab_stage_status_t status,
                          bool locked, vocab_result_t outcome, const char *what)
{
    const bool want_outcome = outcome != VOCAB_RESULT_NONE;
    CHECK(r.status == status && (status == VOCAB_STAGE_DONE || r.locked == locked) &&
              r.outcome_ready == want_outcome && (!want_outcome || r.result == outcome),
          "%s: %s -> status %d locked %d outcome %d/%d", f->ctx, what, (int)r.status,
          (int)r.locked, (int)r.outcome_ready, (int)r.result);
}

// Checks the exact effect `e` of the call that just happened and advances the model.
static void verify(fx_t *f, effect_t e, model_t m, const vocab_view_t *before,
                   vocab_stage_result_t r)
{
    const bool was_locked = m == M_FEEDBACK || m == M_HOLD_LOCK;
    const vocab_uid_t uid = f->entry->uid;
    const char *ctx = f->ctx;
    switch (e) {
    case E_NOTHING:
        CHECK(f->out.count == 0 && same_view(before, &f->view), "%s: ignored without effects",
              ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, was_locked, VOCAB_RESULT_NONE, "ignored event");
        break;
    case E_REPLAY: {
        const vocab_cmd_t want[] = { cmd_word(uid) };
        CHECK(outbox_is(&f->out, want, 1) && same_view(before, &f->view),
              "%s: OK only replays the word", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, false, VOCAB_RESULT_NONE, "replay");
        break;
    }
    case E_NUDGE: {
        vocab_view_t expect;
        memcpy(&expect, before, sizeof(expect));
        expect.anim.nudge_seq[f->correct]++;
        CHECK(f->out.count == 0 && same_view(&expect, &f->view),
              "%s: the other key only nudges the correct panel once", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, false, VOCAB_RESULT_NONE, "nudge");
        break;
    }
    case E_CORRECT: {
        const vocab_cmd_t want[] = { cmd_cue(VOCAB_CUE_CORRECT), cmd_epoch(),
                                     cmd_timer(VOCAB_CORRECT_FEEDBACK_MS) };
        CHECK(outbox_is(&f->out, want, 3), "%s: correct -> CUE_CORRECT, epoch, 600 ms lock", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, true, VOCAB_RESULT_CORRECT_FIRST, "correct");
        f->model = M_FEEDBACK;
        f->lock_deadline = f->now + VOCAB_CORRECT_FEEDBACK_MS;
        break;
    }
    case E_WRONG: {
        const vocab_cmd_t want[] = { cmd_cue_word(VOCAB_CUE_WRONG, uid), cmd_epoch(),
                                     cmd_timer(VOCAB_ERROR_HOLD_MS) };
        CHECK(outbox_is(&f->out, want, 3),
              "%s: wrong -> CUE_WRONG then the word, epoch, 1500 ms lock", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, true, VOCAB_RESULT_WRONG, "wrong");
        f->model = M_HOLD_LOCK;
        f->lock_deadline = f->now + VOCAB_ERROR_HOLD_MS;
        break;
    }
    case E_TOO_EASY:
        CHECK(f->out.count == 0 && same_view(before, &f->view),
              "%s: too easy ends the card without commands or view changes", ctx);
        expect_status(f, r, VOCAB_STAGE_DONE, false, VOCAB_RESULT_TOO_EASY, "too easy");
        f->model = M_DONE;
        break;
    case E_REARM: {
        const vocab_cmd_t want[] = { cmd_timer(f->lock_deadline - f->now) };
        CHECK(outbox_is(&f->out, want, 1) && same_view(before, &f->view),
              "%s: an early timer re-arms the rest of the lock", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, true, VOCAB_RESULT_NONE, "early timer");
        break;
    }
    case E_FEEDBACK_END:
        CHECK(f->out.count == 0 && same_view(before, &f->view), "%s: feedback end is silent",
              ctx);
        expect_status(f, r, VOCAB_STAGE_DONE, false, VOCAB_RESULT_NONE, "feedback end");
        CHECK(f->now - f->outcome_at >= VOCAB_CORRECT_FEEDBACK_MS,
              "%s: feedback lasted %u ms (>= 600)", ctx, (unsigned)(f->now - f->outcome_at));
        f->model = M_DONE;
        break;
    case E_HOLD_END: {
        const vocab_cmd_t want[] = { cmd_epoch() };
        CHECK(outbox_is(&f->out, want, 1), "%s: the lock end is an input epoch", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, false, VOCAB_RESULT_NONE, "hold end");
        CHECK(f->now - f->outcome_at >= VOCAB_ERROR_HOLD_MS,
              "%s: the error-hold lock lasted %u ms (>= 1500)", ctx,
              (unsigned)(f->now - f->outcome_at));
        f->model = M_HOLD_WAIT;
        break;
    }
    case E_FINISH:
        CHECK(f->out.count == 0 && same_view(before, &f->view), "%s: finishing is silent", ctx);
        expect_status(f, r, VOCAB_STAGE_DONE, false, VOCAB_RESULT_NONE, "finish");
        f->model = M_DONE;
        break;
    case E_RESUME_LOCK: {
        const uint32_t full = m == M_FEEDBACK ? VOCAB_CORRECT_FEEDBACK_MS : VOCAB_ERROR_HOLD_MS;
        const vocab_cmd_t want[] = { cmd_timer(full) };
        CHECK(outbox_is(&f->out, want, 1) && same_view(before, &f->view),
              "%s: resume inside a lock re-arms a full window", ctx);
        expect_status(f, r, VOCAB_STAGE_RUNNING, true, VOCAB_RESULT_NONE, "resume in a lock");
        f->lock_deadline = f->now + full;
        break;
    }
    }
}

// Delivers `ev` (resume when NULL), checks the predicted effect and the view of the new
// model state.
static vocab_stage_result_t step(fx_t *f, const vocab_event_t *ev)
{
    const model_t m = f->model;
    const effect_t e = predict(f, ev);
    vocab_view_t before;
    const vocab_stage_result_t r = call(f, ev, &before);
    if (m != M_DONE) {
        verify(f, e, m, &before, r);
        s_effects[e]++;
    }
    check_view(f);
    return r;
}

static vocab_stage_result_t click(fx_t *f, vocab_key_t key)
{
    const vocab_event_t ev = vocab_event_click(key);
    return step(f, &ev);
}

static vocab_stage_result_t hold(fx_t *f, vocab_key_t key)
{
    const vocab_event_t ev = vocab_event_long(key);
    return step(f, &ev);
}

static vocab_stage_result_t timer(fx_t *f, vocab_timer_id_t id)
{
    const vocab_event_t ev = vocab_event_timer(id);
    return step(f, &ev);
}

static vocab_key_t right_key(const fx_t *f)
{
    return vocab_side_key(f->correct);
}

static vocab_key_t wrong_key(const fx_t *f)
{
    return vocab_side_key(vocab_side_other(f->correct));
}

// Session-like poll: fires every armed stage timer whose deadline has been reached.
static void poll(fx_t *f)
{
    for (int guard = 0; guard < 4; guard++) {
        int best = -1;
        for (int i = 0; i < (int)VOCAB_STAGE_TIMER_COUNT; i++) {
            if (!f->armed[i] || !vocab_ms_reached(f->now, f->deadline[i])) continue;
            if (best < 0 || (int32_t)(f->deadline[i] - f->deadline[best]) < 0) best = i;
        }
        if (best < 0) return;
        timer(f, (vocab_timer_id_t)best);
    }
}

// ---------------------------------------------------------------------------------------
// Card setup
// ---------------------------------------------------------------------------------------

static void fx_init(fx_t *f, const vocab_bank_t *bank, vocab_index_t index,
                    vocab_purpose_t purpose, bool allow_too_easy, uint32_t seed)
{
    memset(f, 0, sizeof(*f));
    f->bank = bank;
    f->entry = vocab_bank_at(bank, index);
    f->purpose = purpose;
    f->allow_too_easy = allow_too_easy;
    f->now = T_START;
    vocab_rng_seed(&f->rng, seed);
    vocab_view_init(&f->view);
    seed_session_region(&f->view);
    vocab_view_reset_card(&f->view);
    snprintf(f->ctx, sizeof(f->ctx), "uid %u purpose %d allow %d seed %u",
             f->entry ? (unsigned)f->entry->uid : 0u, (int)purpose, (int)allow_too_easy,
             (unsigned)seed);
}

// begin() with an explicit lure; checks the begin contract and reads the correct side.
static void fx_start(fx_t *f, vocab_index_t lure)
{
    f->lure = vocab_bank_at(f->bank, lure);
    const vocab_stage_input_t in = {
        .bank = f->bank, .index = (vocab_index_t)(f->entry - f->bank->entries),
        .purpose = f->purpose, .format = VOCAB_FORMAT_S1, .mastery = 0, .lure_index = lure,
        .cloze_front = false, .allow_too_easy = f->allow_too_easy, .rng = &f->rng,
    };
    vocab_view_t before;
    memcpy(&before, &f->view, sizeof(before));
    vocab_rng_t ref = f->rng;
    (void)vocab_rng_next(&ref);
    vocab_outbox_clear(&f->out);
    const vocab_stage_result_t r = vocab_stage1_begin(&f->st, &in, f->now, &f->view, &f->out);
    check_session_region(f, &before);
    check_outbox(f, CALL_BEGIN);
    apply_timers(f);
    record(f, r);
    expect_status(f, r, VOCAB_STAGE_RUNNING, false, VOCAB_RESULT_NONE, "begin");
    const vocab_cmd_t want[] = { cmd_word(f->entry->uid) };
    CHECK(outbox_is(&f->out, want, 1), "%s: begin requests exactly the word clip", f->ctx);
    CHECK(f->rng.state == ref.state, "%s: begin draws exactly one RNG value", f->ctx);
    f->correct = strcmp(f->view.panel[VOCAB_SIDE_UP].text, f->entry->zh) == 0 ? VOCAB_SIDE_UP
                                                                               : VOCAB_SIDE_DOWN;
    f->model = M_QUESTION;
    check_view(f);
    check_english(f);
}

// Like the session: the lure is picked with the shared RNG, then begin() draws the side.
static void fx_begin(fx_t *f, vocab_index_t index, vocab_purpose_t purpose, bool allow_too_easy,
                     uint32_t seed)
{
    fx_init(f, vocab_bank_builtin(), index, purpose, allow_too_easy, seed);
    vocab_index_t lure = VOCAB_INDEX_NONE;
    CHECK(vocab_lure_pick(f->bank, index, VOCAB_INDEX_NONE, &f->rng, &lure), "%s: lure picked",
          f->ctx);
    fx_start(f, lure);
}

// fx_begin() with the first seed that puts the correct meaning on `side`.
static void fx_begin_side(fx_t *f, vocab_index_t index, vocab_purpose_t purpose,
                          bool allow_too_easy, vocab_side_t side)
{
    for (uint32_t seed = 1; seed < 200u; seed++) {
        fx_begin(f, index, purpose, allow_too_easy, seed);
        if (f->correct == side) return;
    }
    CHECK(false, "no seed puts the correct meaning on side %d", (int)side);
}

// Drives the card to its end: timers when locked, else the correct key.
static void finish(fx_t *f)
{
    for (int i = 0; i < 6 && !f->done; i++) {
        if (f->res.locked) {
            if (!f->armed[VOCAB_TIMER_STAGE]) break;  // reported by call()
            if (!vocab_ms_reached(f->now, f->deadline[VOCAB_TIMER_STAGE])) {
                f->now = f->deadline[VOCAB_TIMER_STAGE];
            }
            timer(f, VOCAB_TIMER_STAGE);
        } else {
            click(f, right_key(f));
        }
    }
    CHECK(f->done && f->outcomes == 1, "%s: the card ends with exactly one outcome", f->ctx);
}

// ---------------------------------------------------------------------------------------
// Card states (spec 5.1, 5.3, 11.5)
// ---------------------------------------------------------------------------------------

// begin(): word, IPA (or none), question prompt, both meanings in the S1 style, badge per
// purpose, replay hint, PLAY_WORD only (checked inside fx_start/check_view).
static void test_begin_views(void)
{
    const vocab_uid_t uids[] = {
        101,  // word with IPA
        706,  // phrase with IPA (ice cream)
        902,  // phrase without IPA
        905,  // longest phrase, no IPA
        903,  // word in a phrase category, with IPA
    };
    const vocab_purpose_t purposes[] = { VOCAB_PURPOSE_LEARN, VOCAB_PURPOSE_TEST_LATER,
                                         VOCAB_PURPOSE_SPOT_CHECK, VOCAB_PURPOSE_TOO_EASY_CHECK };
    for (size_t u = 0; u < sizeof(uids) / sizeof(uids[0]); u++) {
        for (size_t p = 0; p < sizeof(purposes) / sizeof(purposes[0]); p++) {
            for (int s = 0; s < 2; s++) {
                const vocab_side_t side = s ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP;
                fx_begin_side(&F, index_of(uids[u]), purposes[p], true, side);
                CHECK(F.correct == side && strcmp(F.view.panel[side].text, F.entry->zh) == 0,
                      "%s: correct meaning on side %d", F.ctx, s);
                CHECK(F.view.badge == expected_badge(purposes[p]), "%s: badge", F.ctx);
                CHECK(F.view.card.ipa.visible == vocab_entry_has_ipa(F.entry), "%s: IPA line",
                      F.ctx);
                CHECK(F.lure != F.entry && strcmp(F.lure->zh, F.entry->zh) != 0,
                      "%s: the lure is another meaning", F.ctx);
            }
        }
    }
}

// Every bank entry through both outcome paths (all words/phrases, all IPA cases).
static void test_all_entries(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    for (vocab_index_t i = 0; i < bank->count; i++) {
        fx_begin(&F, i, VOCAB_PURPOSE_LEARN, true, 500u + i);
        click(&F, right_key(&F));
        finish(&F);
        CHECK(F.outcome == VOCAB_RESULT_CORRECT_FIRST, "%s: correct path", F.ctx);

        fx_begin(&F, i, VOCAB_PURPOSE_TEST_LATER, false, 900u + i);
        click(&F, wrong_key(&F));
        F.now += VOCAB_ERROR_HOLD_MS;
        poll(&F);
        click(&F, VOCAB_KEY_OK);
        click(&F, right_key(&F));
        CHECK(F.done && F.outcome == VOCAB_RESULT_WRONG, "%s: wrong path", F.ctx);
    }
}

// Correct first press (O1/O5/O10/O13): green panel, "correct" prompt, CUE_CORRECT, epoch,
// 600 ms lock in which every key is ignored, then DONE without a second outcome.
static void test_correct_first(void)
{
    const vocab_purpose_t purposes[] = { VOCAB_PURPOSE_LEARN, VOCAB_PURPOSE_TEST_LATER,
                                         VOCAB_PURPOSE_SPOT_CHECK, VOCAB_PURPOSE_TOO_EASY_CHECK };
    const vocab_uid_t uids[] = { 101, 902 };
    for (size_t u = 0; u < 2; u++) {
        for (size_t p = 0; p < 4; p++) {
            for (int s = 0; s < 2; s++) {
                fx_begin_side(&F, index_of(uids[u]), purposes[p], true,
                              s ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP);
                const vocab_ms_t t0 = F.now;
                const vocab_stage_result_t r = click(&F, right_key(&F));
                CHECK(r.outcome_ready && r.result == VOCAB_RESULT_CORRECT_FIRST && r.locked,
                      "%s: CORRECT_FIRST, locked", F.ctx);
                CHECK(F.view.panel[F.correct].style == VOCAB_PANEL_CORRECT &&
                          F.view.panel[vocab_side_other(F.correct)].visible,
                      "%s: chosen panel green, the other unchanged", F.ctx);
                const vocab_ms_t offsets[] = { 0, 1, 300, VOCAB_CORRECT_FEEDBACK_MS - 1u };
                for (size_t k = 0; k < 4; k++) {
                    F.now = t0 + offsets[k];
                    click(&F, VOCAB_KEY_UP);
                    click(&F, VOCAB_KEY_DOWN);
                    click(&F, VOCAB_KEY_OK);
                    hold(&F, VOCAB_KEY_UP);
                }
                timer(&F, VOCAB_TIMER_STAGE);  // at 599 ms: re-armed for 1 ms
                CHECK(!F.done && F.res.locked, "%s: still locked at 599 ms", F.ctx);
                F.now = t0 + VOCAB_CORRECT_FEEDBACK_MS;
                timer(&F, VOCAB_TIMER_STAGE_AUX);  // not Stage 1's timer: ignored
                timer(&F, VOCAB_TIMER_STAGE);
                CHECK(F.done && F.outcomes == 1, "%s: DONE after 600 ms", F.ctx);
                click(&F, right_key(&F));  // inert after DONE (checked by call())
            }
        }
    }
}

// AC-15 error hold (O2/O6/O11/O14): the same update hides the wrong panel and marks the
// correct one; CUE_WRONG + word; 1500 ms lock (epochs at both ends) ignoring every key,
// including the correct key at 1499 ms; afterwards only the correct key completes, the other
// key nudges, OK replays.
static void test_error_hold(void)
{
    const vocab_purpose_t purposes[] = { VOCAB_PURPOSE_LEARN, VOCAB_PURPOSE_TEST_LATER,
                                         VOCAB_PURPOSE_SPOT_CHECK, VOCAB_PURPOSE_TOO_EASY_CHECK };
    const vocab_uid_t uids[] = { 101, 902 };
    for (size_t u = 0; u < 2; u++) {
        for (size_t p = 0; p < 4; p++) {
            for (int s = 0; s < 2; s++) {
                const vocab_side_t side = s ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP;
                fx_begin_side(&F, index_of(uids[u]), purposes[p], true, side);
                F.now += 2000;  // the child thinks for a while
                const vocab_ms_t t0 = F.now;
                const vocab_stage_result_t r = click(&F, wrong_key(&F));
                CHECK(r.outcome_ready && r.result == VOCAB_RESULT_WRONG && r.locked,
                      "%s: WRONG, locked", F.ctx);
                CHECK(!F.view.panel[vocab_side_other(side)].visible &&
                          F.view.panel[side].style == VOCAB_PANEL_CORRECT &&
                          F.view.badge == VOCAB_BADGE_REMEMBER,
                      "%s: wrong panel hidden in the same update", F.ctx);

                const vocab_ms_t offsets[] = { 0, 1, 250, 750, 1000, VOCAB_ERROR_HOLD_MS - 1u };
                for (size_t k = 0; k < sizeof(offsets) / sizeof(offsets[0]); k++) {
                    F.now = t0 + offsets[k];
                    click(&F, right_key(&F));
                    click(&F, wrong_key(&F));
                    click(&F, VOCAB_KEY_OK);
                    hold(&F, VOCAB_KEY_UP);
                    timer(&F, VOCAB_TIMER_STAGE_AUX);
                }
                CHECK(!F.done && F.res.locked && F.outcomes == 1,
                      "%s: the correct key at 1499 ms is ignored", F.ctx);
                timer(&F, VOCAB_TIMER_STAGE);  // delivered 1 ms early: re-armed
                CHECK(F.res.locked && F.armed[VOCAB_TIMER_STAGE] &&
                          F.deadline[VOCAB_TIMER_STAGE] == t0 + VOCAB_ERROR_HOLD_MS,
                      "%s: an early timer keeps the lock until 1500 ms", F.ctx);

                F.now = t0 + VOCAB_ERROR_HOLD_MS;
                poll(&F);
                CHECK(!F.res.locked && F.model == M_HOLD_WAIT, "%s: wait part at 1500 ms", F.ctx);
                CHECK(F.view.hint.kind == (side == VOCAB_SIDE_UP ? VOCAB_HINT_PRESS_UP
                                                                 : VOCAB_HINT_PRESS_DOWN) &&
                          F.view.panel[side].nudge_repeat,
                      "%s: 'press the correct key' hint and repeating nudge", F.ctx);

                const uint8_t nudges = F.view.anim.nudge_seq[side];
                for (int k = 0; k < 3; k++) click(&F, wrong_key(&F));
                CHECK((uint8_t)(F.view.anim.nudge_seq[side] - nudges) == 3u,
                      "%s: three wrong presses -> three nudges", F.ctx);
                click(&F, VOCAB_KEY_OK);
                hold(&F, VOCAB_KEY_UP);
                timer(&F, VOCAB_TIMER_STAGE);  // stale
                CHECK(!F.done, "%s: only the correct key completes", F.ctx);
                click(&F, right_key(&F));
                CHECK(F.done && F.outcomes == 1 && F.outcome == VOCAB_RESULT_WRONG,
                      "%s: one WRONG outcome for the whole card", F.ctx);
            }
        }
    }
}

// AC-15 boundary sweep: every millisecond of both lock windows, with session-like polling.
static void test_lock_sweeps(void)
{
    fx_begin_side(&F, index_of(301), VOCAB_PURPOSE_LEARN, true, VOCAB_SIDE_DOWN);
    vocab_ms_t t0 = F.now;
    click(&F, wrong_key(&F));
    for (vocab_ms_t dt = 0; dt < VOCAB_ERROR_HOLD_MS; dt++) {
        F.now = t0 + dt;
        poll(&F);
        CHECK(F.model == M_HOLD_LOCK, "error hold still locked at %u ms", (unsigned)dt);
        click(&F, right_key(&F));
    }
    F.now = t0 + VOCAB_ERROR_HOLD_MS;
    poll(&F);
    CHECK(F.model == M_HOLD_WAIT, "error hold unlocks exactly at 1500 ms");
    click(&F, right_key(&F));
    CHECK(F.done, "the correct key at 1500 ms completes the card");

    fx_begin_side(&F, index_of(301), VOCAB_PURPOSE_LEARN, true, VOCAB_SIDE_UP);
    t0 = F.now;
    click(&F, right_key(&F));
    for (vocab_ms_t dt = 0; dt < VOCAB_CORRECT_FEEDBACK_MS; dt++) {
        F.now = t0 + dt;
        poll(&F);
        CHECK(F.model == M_FEEDBACK, "feedback still locked at %u ms", (unsigned)dt);
        click(&F, wrong_key(&F));
    }
    F.now = t0 + VOCAB_CORRECT_FEEDBACK_MS;
    poll(&F);
    CHECK(F.done && F.outcome == VOCAB_RESULT_CORRECT_FIRST, "feedback ends exactly at 600 ms");
}

// OK replays the word in the question without deciding anything: the first UP/DOWN press
// still decides the outcome (spec 4.3).
static void test_ok_replay(void)
{
    fx_begin(&F, index_of(204), VOCAB_PURPOSE_LEARN, true, 21);
    for (int k = 0; k < 3; k++) click(&F, VOCAB_KEY_OK);
    CHECK(F.outcomes == 0, "OK never decides");
    click(&F, wrong_key(&F));
    CHECK(F.outcome == VOCAB_RESULT_WRONG, "the first answer press after replays decides");
    finish(&F);

    fx_begin(&F, index_of(204), VOCAB_PURPOSE_SPOT_CHECK, false, 22);
    click(&F, VOCAB_KEY_OK);
    click(&F, right_key(&F));
    CHECK(F.outcome == VOCAB_RESULT_CORRECT_FIRST, "replay, then a correct first press");
    finish(&F);
}

// AC-21/AC-22 (Stage 1 part): UP LONG ends a LEARN question with TOO_EASY (O12); it is
// ignored without allow_too_easy, on inserted cards, in locks and in the wait part; DOWN and
// OK LONG never act inside the stage.
static void test_too_easy(void)
{
    fx_begin(&F, index_of(201), VOCAB_PURPOSE_LEARN, true, 31);
    hold(&F, VOCAB_KEY_UP);
    CHECK(F.done && F.outcome == VOCAB_RESULT_TOO_EASY, "UP LONG on a LEARN question");

    fx_begin(&F, index_of(201), VOCAB_PURPOSE_LEARN, true, 32);
    click(&F, VOCAB_KEY_OK);
    hold(&F, VOCAB_KEY_DOWN);
    hold(&F, VOCAB_KEY_OK);
    hold(&F, VOCAB_KEY_UP);
    CHECK(F.done && F.outcome == VOCAB_RESULT_TOO_EASY, "still a question after replays");

    fx_begin(&F, index_of(201), VOCAB_PURPOSE_LEARN, false, 33);
    hold(&F, VOCAB_KEY_UP);
    CHECK(!F.done && F.outcomes == 0, "ignored without allow_too_easy");
    click(&F, right_key(&F));
    finish(&F);

    const vocab_purpose_t inserted[] = { VOCAB_PURPOSE_TEST_LATER, VOCAB_PURPOSE_SPOT_CHECK,
                                         VOCAB_PURPOSE_TOO_EASY_CHECK };
    for (size_t p = 0; p < 3; p++) {
        for (int allow = 0; allow < 2; allow++) {
            fx_begin(&F, index_of(201), inserted[p], allow != 0, 40u + (uint32_t)p);
            hold(&F, VOCAB_KEY_UP);
            CHECK(!F.done && F.outcomes == 0, "%s: UP LONG ignored on inserted cards", F.ctx);
            finish(&F);
        }
    }

    fx_begin(&F, index_of(201), VOCAB_PURPOSE_LEARN, true, 50);
    click(&F, right_key(&F));
    hold(&F, VOCAB_KEY_UP);
    CHECK(F.outcome == VOCAB_RESULT_CORRECT_FIRST, "ignored in the feedback lock");
    finish(&F);

    fx_begin(&F, index_of(201), VOCAB_PURPOSE_LEARN, true, 51);
    click(&F, wrong_key(&F));
    hold(&F, VOCAB_KEY_UP);
    F.now += VOCAB_ERROR_HOLD_MS;
    poll(&F);
    hold(&F, VOCAB_KEY_UP);
    CHECK(F.outcome == VOCAB_RESULT_WRONG && F.model == M_HOLD_WAIT,
          "ignored in the hold lock and the wait part");
    finish(&F);
}

// Resume after a pause card: re-renders the restored region, no audio/epoch/outcome; inside
// a lock (never offered by the session) a full window is re-armed so the card cannot stick.
static void test_resume(void)
{
    fx_begin(&F, index_of(305), VOCAB_PURPOSE_LEARN, true, 61);
    F.now += 4000;
    step(&F, NULL);  // question: nothing changes

    // A pause card that drew over the stage region and was not restored: resume repairs it.
    vocab_view_set_panel(&F.view, VOCAB_SIDE_UP, VOCAB_STR_PAUSE_CONTINUE, VOCAB_PANEL_S1, false);
    vocab_view_set_panel(&F.view, VOCAB_SIDE_DOWN, VOCAB_STR_PAUSE_HOME, VOCAB_PANEL_NEUTRAL,
                         false);
    vocab_view_show(&F.view.card.prompt, VOCAB_STR_PAUSE_PROMPT, VOCAB_RGB_SOFT);
    vocab_view_hide(&F.view.card.word);
    F.view.badge = VOCAB_BADGE_PAUSE;
    F.view.hint.kind = VOCAB_HINT_NONE;
    vocab_view_t before;
    const vocab_stage_result_t r = call(&F, NULL, &before);
    CHECK(r.status == VOCAB_STAGE_RUNNING && !r.locked && F.out.count == 0,
          "resume of the question: running, unlocked, no command");
    check_view(&F);

    // Wait part: unchanged, nudge flag kept.
    click(&F, wrong_key(&F));
    F.now += VOCAB_ERROR_HOLD_MS;
    poll(&F);
    F.now += 3000;
    step(&F, NULL);
    CHECK(F.model == M_HOLD_WAIT && F.view.panel[F.correct].nudge_repeat, "wait part resumed");
    click(&F, right_key(&F));
    CHECK(F.done, "the correct key completes after resume");
    step(&F, NULL);  // DONE: inert

    // Lock windows (contract violation by the caller): full window re-armed, never stuck.
    fx_begin(&F, index_of(305), VOCAB_PURPOSE_LEARN, true, 62);
    vocab_ms_t t0 = F.now;
    click(&F, right_key(&F));
    F.now = t0 + 100;
    step(&F, NULL);
    F.now = t0 + 100 + VOCAB_CORRECT_FEEDBACK_MS - 1u;
    poll(&F);
    CHECK(!F.done, "feedback re-armed in full by resume");
    F.now += 1;
    poll(&F);
    CHECK(F.done, "feedback ends a full window after resume");

    fx_begin(&F, index_of(305), VOCAB_PURPOSE_LEARN, true, 63);
    t0 = F.now;
    click(&F, wrong_key(&F));
    F.now = t0 + 1400;
    step(&F, NULL);
    F.now = t0 + 1400 + VOCAB_ERROR_HOLD_MS - 1u;
    poll(&F);
    CHECK(F.model == M_HOLD_LOCK, "hold re-armed in full by resume");
    F.now += 1;
    poll(&F);
    CHECK(F.model == M_HOLD_WAIT, "hold ends a full window after resume");
    finish(&F);
}

// ---------------------------------------------------------------------------------------
// Unusable input, side balance, palette
// ---------------------------------------------------------------------------------------

// begin() on unusable input: DONE without an outcome, nothing shown or requested, no RNG
// draw; later handle()/resume() calls stay inert.
static void expect_unusable(const vocab_stage_input_t *in, const char *what)
{
    vocab_stage1_t st;
    vocab_view_t view;
    vocab_view_t before;
    vocab_outbox_t out;
    memset(&st, 0xA5, sizeof(st));  // stale storage from an earlier card
    vocab_view_init(&view);
    seed_session_region(&view);
    vocab_view_reset_card(&view);
    memcpy(&before, &view, sizeof(before));
    vocab_outbox_clear(&out);
    const uint64_t rng_state = in && in->rng ? in->rng->state : 0u;
    vocab_stage_result_t r = vocab_stage1_begin(&st, in, T_START, &view, &out);
    CHECK(r.status == VOCAB_STAGE_DONE && !r.outcome_ready, "%s: begin -> DONE, no outcome",
          what);
    CHECK(out.count == 0 && same_view(&before, &view), "%s: nothing shown or requested", what);
    CHECK(!in || !in->rng || in->rng->state == rng_state, "%s: no RNG draw", what);
    const vocab_event_t ev = vocab_event_click(VOCAB_KEY_UP);
    r = vocab_stage1_handle(&st, &ev, T_START + 300u, &view, &out);
    CHECK(r.status == VOCAB_STAGE_DONE && !r.outcome_ready && out.count == 0 &&
              same_view(&before, &view),
          "%s: handle() afterwards is inert", what);
    r = vocab_stage1_resume(&st, T_START + 600u, &view, &out);
    CHECK(r.status == VOCAB_STAGE_DONE && out.count == 0 && same_view(&before, &view),
          "%s: resume() afterwards is inert", what);
}

static void test_unusable_input(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    const vocab_index_t apple = index_of(101);
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 71);
    const vocab_stage_input_t in = {
        .bank = bank, .index = apple, .purpose = VOCAB_PURPOSE_LEARN, .format = VOCAB_FORMAT_S1,
        .mastery = 0, .lure_index = index_of(301), .cloze_front = false, .allow_too_easy = true,
        .rng = &rng,
    };
    vocab_stage_input_t t = in;
    t.lure_index = VOCAB_INDEX_NONE;
    expect_unusable(&t, "no lure");
    t = in;
    t.lure_index = apple;
    expect_unusable(&t, "lure is the word itself");
    t = in;
    t.lure_index = bank->count;
    expect_unusable(&t, "lure out of range");
    t = in;
    t.index = bank->count;
    expect_unusable(&t, "word out of range");
    t = in;
    t.index = VOCAB_INDEX_NONE;
    expect_unusable(&t, "word NONE");
    t = in;
    t.bank = NULL;
    expect_unusable(&t, "no bank");
    t = in;
    t.rng = NULL;
    expect_unusable(&t, "no RNG");
    t = in;
    t.index = index_of(403);       // book
    t.lure_index = index_of(405);  // schoolbag: its meaning contains "book"
    expect_unusable(&t, "lure meaning contains the meaning");
    expect_unusable(NULL, "no input");

    vocab_stage1_t st;
    vocab_view_t view;
    vocab_outbox_t out;
    vocab_view_init(&view);
    vocab_outbox_clear(&out);
    const vocab_event_t ev = vocab_event_click(VOCAB_KEY_UP);
    CHECK(vocab_stage1_begin(NULL, &in, T_START, &view, &out).status == VOCAB_STAGE_DONE,
          "NULL state");
    CHECK(vocab_stage1_begin(&st, &in, T_START, NULL, &out).status == VOCAB_STAGE_DONE &&
              out.count == 0,
          "NULL view");
    CHECK(vocab_stage1_begin(&st, &in, T_START, &view, NULL).status == VOCAB_STAGE_DONE,
          "NULL outbox");
    CHECK(vocab_stage1_handle(NULL, &ev, T_START, &view, &out).status == VOCAB_STAGE_DONE &&
              vocab_stage1_resume(NULL, T_START, &view, &out).status == VOCAB_STAGE_DONE,
          "NULL state in handle/resume");

    // A running card ignores a NULL event.
    fx_begin(&F, apple, VOCAB_PURPOSE_LEARN, true, 74);
    vocab_outbox_clear(&F.out);
    const vocab_stage_result_t r = vocab_stage1_handle(&F.st, NULL, F.now, &F.view, &F.out);
    CHECK(r.status == VOCAB_STAGE_RUNNING && !r.locked && !r.outcome_ready && F.out.count == 0,
          "NULL event ignored");

    // A same-category lure is usable (the picker's fallback tier, spec 5.2 step 3).
    fx_init(&F, bank, apple, VOCAB_PURPOSE_LEARN, true, 72);
    fx_start(&F, index_of(102));
    CHECK(!F.done && F.model == M_QUESTION, "same-category lure accepted");
    finish(&F);

    // Synthetic entries: texts are shown whole or the card is not shown at all (a truncated
    // English word would be a misspelling, AC-27).
    char too_long[VOCAB_VIEW_TEXT_CAP + 1];
    char longest[VOCAB_VIEW_TEXT_CAP];
    memset(too_long, 'a', VOCAB_VIEW_TEXT_CAP);
    too_long[VOCAB_VIEW_TEXT_CAP] = '\0';
    memset(longest, 'b', VOCAB_VIEW_TEXT_CAP - 1u);
    longest[VOCAB_VIEW_TEXT_CAP - 1u] = '\0';
    vocab_entry_t e[2] = { bank->entries[apple], bank->entries[index_of(301)] };
    const vocab_bank_t syn = { e, 2 };
    vocab_stage_input_t s = in;
    s.bank = &syn;
    s.index = 0;
    s.lure_index = 1;

    fx_init(&F, &syn, 0, VOCAB_PURPOSE_LEARN, true, 73);
    fx_start(&F, 1);
    CHECK(F.model == M_QUESTION && !F.done, "synthetic baseline runs");
    finish(&F);
    e[0].text = longest;
    fx_init(&F, &syn, 0, VOCAB_PURPOSE_LEARN, true, 73);
    fx_start(&F, 1);
    CHECK(F.model == M_QUESTION && strcmp(F.view.card.word.text, longest) == 0,
          "the longest text that fits is shown whole");
    finish(&F);

    e[0].text = NULL;
    expect_unusable(&s, "word without text");
    e[0].text = "";
    expect_unusable(&s, "word with empty text");
    e[0].text = too_long;
    expect_unusable(&s, "word text too long for its label");
    e[0] = bank->entries[apple];
    e[0].zh = NULL;
    expect_unusable(&s, "word without meaning");
    e[0] = bank->entries[apple];
    e[1].zh = NULL;
    expect_unusable(&s, "lure without meaning");
    e[1].zh = too_long;
    expect_unusable(&s, "lure meaning too long for its panel");
    e[1].zh = e[0].zh;
    expect_unusable(&s, "lure with the same meaning");
    e[1] = bank->entries[index_of(301)];
    e[1].uid = e[0].uid;
    expect_unusable(&s, "lure with the same uid");

    // An IPA that does not fit its label is hidden; the card still runs.
    e[1] = bank->entries[index_of(301)];
    e[0].ipa = too_long;
    vocab_view_init(&view);
    vocab_view_reset_card(&view);
    vocab_outbox_clear(&out);
    vocab_rng_seed(&rng, 75);
    const vocab_stage_result_t ri = vocab_stage1_begin(&st, &s, T_START, &view, &out);
    CHECK(ri.status == VOCAB_STAGE_RUNNING && view.card.word.visible && !view.card.ipa.visible,
          "an IPA too long for its label is hidden");
}

// AC-13 (side part): for every bank entry over 10,000 seeds (lure picked first, as the
// session does) the correct meaning is on UP in 45-55 % of the cards; begin() draws exactly
// one RNG value and shows the picked lure on the other side.
static void test_side_balance(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    static vocab_view_t view;
    unsigned long total_up = 0;
    unsigned long total = 0;
    vocab_view_init(&view);
    for (vocab_index_t t = 0; t < bank->count; t++) {
        const vocab_entry_t *e = &bank->entries[t];
        unsigned up = 0;
        unsigned bad = 0;
        for (uint32_t seed = 1; seed <= SEEDS; seed++) {
            vocab_rng_t rng;
            vocab_rng_seed(&rng, seed);
            vocab_index_t lure = VOCAB_INDEX_NONE;
            if (!vocab_lure_pick(bank, t, VOCAB_INDEX_NONE, &rng, &lure)) {
                bad++;
                continue;
            }
            const vocab_stage_input_t in = {
                .bank = bank, .index = t, .purpose = VOCAB_PURPOSE_LEARN,
                .format = VOCAB_FORMAT_S1, .mastery = 0, .lure_index = lure,
                .cloze_front = false, .allow_too_easy = true, .rng = &rng,
            };
            vocab_stage1_t st;
            vocab_outbox_t out;
            vocab_view_reset_card(&view);
            vocab_outbox_clear(&out);
            vocab_rng_t ref = rng;
            (void)vocab_rng_next(&ref);
            const vocab_stage_result_t r = vocab_stage1_begin(&st, &in, T_START, &view, &out);
            const bool on_up = strcmp(view.panel[VOCAB_SIDE_UP].text, e->zh) == 0;
            const char *other = view.panel[on_up ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP].text;
            if (r.status != VOCAB_STAGE_RUNNING || rng.state != ref.state ||
                strcmp(other, bank->entries[lure].zh) != 0 ||
                strcmp(view.panel[0].text, view.panel[1].text) == 0) {
                bad++;
            }
            if (on_up) up++;
        }
        CHECK(bad == 0, "uid %u: %u malformed cards", (unsigned)e->uid, bad);
        CHECK(up >= SEEDS * 45u / 100u && up <= SEEDS * 55u / 100u,
              "uid %u: correct meaning on UP in %u of %u cards (45-55 %%)", (unsigned)e->uid, up,
              SEEDS);
        total_up += up;
        total += SEEDS;
    }
    CHECK(total_up * 100u >= total * 49u && total_up * 100u <= total * 51u,
          "overall UP share %lu / %lu", total_up, total);
}

// The Stage 1 view fields resolve to the spec 11.2 / 11.5 colors and texts.
static void test_palette_and_texts(void)
{
    uint32_t bg = 0;
    uint32_t line = 0;
    CHECK(VOCAB_RGB_TEXT == 0xF8FAFCu && VOCAB_RGB_DIM == 0x94A3B8u &&
              VOCAB_RGB_SUCCESS == 0x34D399u && VOCAB_RGB_YELLOW == 0xFDE047u,
          "label colors");
    vocab_view_panel_colors(VOCAB_PANEL_S1, &bg, &line);
    CHECK(bg == 0x1E293Bu && line == 0x3B82F6u, "S1 option panel colors");
    vocab_view_panel_colors(VOCAB_PANEL_CORRECT, &bg, &line);
    CHECK(bg == 0x065F46u && line == 0x34D399u, "correct panel colors");
    const struct {
        vocab_badge_t badge;
        const char *text;
        uint32_t accent;
        uint32_t bg;
    } badges[] = {
        { VOCAB_BADGE_NEW, VOCAB_STR_BADGE_NEW, 0x3B82F6u, 0x1E3A8Au },
        { VOCAB_BADGE_TEST_AGAIN, VOCAB_STR_BADGE_TEST_AGAIN, 0x3B82F6u, 0x1E3A8Au },
        { VOCAB_BADGE_SPOT_CHECK, VOCAB_STR_BADGE_SPOT_CHECK, 0xFBBF24u, 0x78350Fu },
        { VOCAB_BADGE_TOO_EASY, VOCAB_STR_BADGE_TOO_EASY, 0xF472B6u, 0x831843u },
        { VOCAB_BADGE_REMEMBER, VOCAB_STR_BADGE_REMEMBER, 0x34D399u, 0x065F46u },
    };
    for (size_t i = 0; i < sizeof(badges) / sizeof(badges[0]); i++) {
        uint32_t accent = 0;
        vocab_view_badge_colors(badges[i].badge, &accent, &bg);
        CHECK(strcmp(vocab_view_badge_text(badges[i].badge), badges[i].text) == 0 &&
                  accent == badges[i].accent && bg == badges[i].bg,
              "badge %d text and colors", (int)badges[i].badge);
    }
    CHECK(strcmp(vocab_view_hint_text(VOCAB_HINT_REPLAY, true), VOCAB_STR_HINT_LISTEN) == 0 &&
              strcmp(vocab_view_hint_text(VOCAB_HINT_REPLAY, false), VOCAB_STR_HINT_LOOK) == 0 &&
              strcmp(vocab_view_hint_text(VOCAB_HINT_PRESS_UP, true), VOCAB_STR_HINT_PRESS_UP) ==
                  0 &&
              strcmp(vocab_view_hint_text(VOCAB_HINT_PRESS_DOWN, true),
                     VOCAB_STR_HINT_PRESS_DOWN) == 0 &&
              vocab_view_hint_text(VOCAB_HINT_NONE, true)[0] == '\0',
          "hint texts");
}

// ---------------------------------------------------------------------------------------
// Randomized event fuzz against the reference model
// ---------------------------------------------------------------------------------------

static vocab_ms_t fuzz_dt(vocab_rng_t *g)
{
    static const vocab_ms_t edges[] = { 0, 1, 2, 50, 249, 250, 599, 600, 601,
                                        1000, 1498, 1499, 1500, 1501, 2500 };
    if (vocab_rng_bit(g)) return edges[vocab_rng_below(g, sizeof(edges) / sizeof(edges[0]))];
    return vocab_rng_below(g, 3000);
}

// One random event. Direct mode delivers everything to the stage (keys during locks, early,
// stale and foreign timers, global LONG keys, invalid keys, resume in any state). Session
// mode follows vocab_session.c: no intents while locked, timers only through poll(), DOWN
// LONG is the volume, OK LONG opens the pause card and resume follows.
static void fuzz_event(fx_t *f, vocab_rng_t *g, bool session_mode)
{
    const uint32_t r = vocab_rng_below(g, 100);
    vocab_event_t ev = vocab_event_click(VOCAB_KEY_OK);
    bool resume = false;
    if (r < 22) {
        ev = vocab_event_click(VOCAB_KEY_UP);
    } else if (r < 44) {
        ev = vocab_event_click(VOCAB_KEY_DOWN);
    } else if (r < 58) {
        ev = vocab_event_click(VOCAB_KEY_OK);
    } else if (r < 66) {
        ev = vocab_event_long(VOCAB_KEY_UP);
    } else if (r < 70) {
        ev = vocab_event_long(VOCAB_KEY_DOWN);
    } else if (r < 74) {
        ev = vocab_event_long(VOCAB_KEY_OK);
    } else if (r < 86) {
        ev = vocab_event_timer(VOCAB_TIMER_STAGE);
    } else if (r < 90) {
        ev = vocab_event_timer(VOCAB_TIMER_STAGE_AUX);
    } else if (r < 92) {
        ev.kind = VOCAB_EV_NONE;
    } else if (r < 94) {
        ev = vocab_event_click((vocab_key_t)VOCAB_KEY_COUNT);  // out-of-range key
    } else {
        resume = true;
    }

    if (!session_mode) {
        step(f, resume ? NULL : &ev);
        return;
    }
    if (ev.kind == VOCAB_EV_TIMER || ev.kind == VOCAB_EV_NONE) return;
    if (ev.kind == VOCAB_EV_CLICK && (unsigned)ev.key >= VOCAB_KEY_COUNT) return;
    if (ev.kind == VOCAB_EV_LONG && ev.key == VOCAB_KEY_DOWN) return;
    if (!f->done && f->res.locked) return;
    if (resume || (ev.kind == VOCAB_EV_LONG && ev.key == VOCAB_KEY_OK)) {
        f->now += vocab_rng_below(g, 5000);  // time on the pause card
        step(f, NULL);
        return;
    }
    step(f, &ev);
}

static void test_fuzz(bool session_mode)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned outcomes[VOCAB_RESULT_TOO_EASY + 1] = { 0 };
    vocab_rng_t g;
    vocab_rng_seed(&g, session_mode ? 0xC0FFEEu : 0xBADC0DEu);
    memset(s_effects, 0, sizeof(s_effects));
    for (unsigned c = 0; c < FUZZ_CARDS; c++) {
        const vocab_index_t index = (vocab_index_t)vocab_rng_below(&g, bank->count);
        const vocab_purpose_t purpose = (vocab_purpose_t)vocab_rng_below(&g, 4);
        const bool allow = vocab_rng_bit(&g);
        fx_begin(&F, index, purpose, allow, 7000u + c);
        snprintf(F.ctx, sizeof(F.ctx), "fuzz %s card %u uid %u purpose %d allow %d",
                 session_mode ? "session" : "direct", c, (unsigned)F.entry->uid, (int)purpose,
                 (int)allow);
        const unsigned events = 1u + vocab_rng_below(&g, 40);
        for (unsigned i = 0; i < events && !F.done; i++) {
            F.now += fuzz_dt(&g);
            if (session_mode) poll(&F);
            if (F.done) break;
            fuzz_event(&F, &g, session_mode);
        }
        finish(&F);
        if ((unsigned)F.outcome <= (unsigned)VOCAB_RESULT_TOO_EASY) outcomes[F.outcome]++;
    }
    const char *mode = session_mode ? "session" : "direct";
    CHECK(outcomes[VOCAB_RESULT_CORRECT_FIRST] > 0 && outcomes[VOCAB_RESULT_WRONG] > 0 &&
              outcomes[VOCAB_RESULT_TOO_EASY] > 0,
          "fuzz (%s) reaches every outcome (%u correct, %u wrong, %u too easy)", mode,
          outcomes[VOCAB_RESULT_CORRECT_FIRST], outcomes[VOCAB_RESULT_WRONG],
          outcomes[VOCAB_RESULT_TOO_EASY]);
    for (int e = 0; e <= (int)E_RESUME_LOCK; e++) {
        const bool session_only_absent = e == E_REARM || e == E_RESUME_LOCK;
        if (session_mode && session_only_absent) {
            CHECK(s_effects[e] == 0, "fuzz (session): effect %d never happens with session "
                                     "timing (%u)", e, s_effects[e]);
        } else {
            CHECK(s_effects[e] > 0, "fuzz (%s) exercised effect %d", mode, e);
        }
    }
}

// Chinese UI strings live in vocab_strings.h only: the Stage 1 sources are pure ASCII.
// Skipped (with a note) when the test does not run from the repository root.
static void test_sources_ascii(void)
{
    const char *files[] = { "main/vocab_stage1.c", "main/vocab_stage1.h", "main/vocab_lure.c",
                            "main/vocab_lure.h" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *fp = fopen(files[i], "rb");
        if (!fp) {
            printf("note: %s not found from the working directory; ASCII check skipped\n",
                   files[i]);
            continue;
        }
        unsigned non_ascii = 0;
        int ch;
        while ((ch = fgetc(fp)) != EOF) {
            if (ch >= 0x80) non_ascii++;
        }
        fclose(fp);
        CHECK(non_ascii == 0, "%s: %u non-ASCII bytes (use VOCAB_STR_* macros)", files[i],
              non_ascii);
    }
}

int main(void)
{
    test_palette_and_texts();
    test_begin_views();
    test_all_entries();
    test_correct_first();
    test_error_hold();
    test_lock_sweeps();
    test_ok_replay();
    test_too_easy();
    test_resume();
    test_unusable_input();
    test_side_balance();
    test_fuzz(false);
    test_fuzz(true);
    test_sources_ascii();
    return vocab_test_report("vocab Stage 1 tests");
}
