// tests/test_vocab_stage2.c - Stage 2 flashcard with self-rating.
//
// Spec 3.2 (Stage 2 key-map rows), 6.1, 6.2, 8.3, 11.4-11.6 (view fields, colors, badges,
// prompts, hints, CONFIRM_FLASH), 4.3 rows O3 O4 O12 (result values), 4.8 (too easy);
// AC-17, AC-18 (stage part), AC-21/22 (Stage 2 part), AC-27 (Stage 2 views).
// The stage is driven directly through the vocab_stage.h contract. Where it matters the
// session is modelled: due timers fire, intents are dropped while the stage is locked, the
// stage timers are cancelled before a pause card and the view is restored before resume().
//
// Compiled by tools/validate.sh with:
//   tests/test_vocab_stage2.c main/vocab_stage2.c main/vocab_anticheat.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_stage2.h"
#include "vocab_strings.h"
#include "vocab_test.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// =======================================================================================
// Fixture: one card driven like the session drives it
// =======================================================================================

typedef struct {
    vocab_stage2_t st;
    vocab_view_t view;
    vocab_view_t ref;              // view after the session's reset_card (session fields)
    vocab_outbox_t out;
    vocab_rng_t rng;
    uint64_t rng_state;            // Stage 2 never draws: the state must stay
    vocab_stage_result_t res;
    const vocab_entry_t *entry;
    vocab_purpose_t purpose;
} fixture_t;

static fixture_t F;

// Session-owned view fields (and the stage counters Stage 2 never uses) get distinctive
// values; the stage must leave all of them untouched.
static void set_session_sentinels(vocab_view_t *v)
{
    v->screen = VOCAB_SCREEN_CARD;
    vocab_view_show(&v->status.progress, "3/7", VOCAB_RGB_DIM);
    v->status.volume = VOCAB_VOL_ICON_LOW;
    v->status.battery = VOCAB_BATT_ICON_2;
    v->status.battery_low = true;
    v->hint.toast = VOCAB_TOAST_VOL_LOW;
    vocab_text_copy(v->hint.text, sizeof(v->hint.text), "resolved");
    v->hint.color = 0x123456u;
    vocab_view_show(&v->home.title, "1", 0x111111u);
    vocab_view_show(&v->summary.score, "2", 0x222222u);
    v->pet.visible = true;
    v->pet.stage = VOCAB_PET_CHILD;
    v->pet.on_summary = true;
    v->xpbar.visible = true;
    v->xpbar.value = 7;
    v->xpbar.max = 60;
    v->anim.pulse_seq = 201;
    v->anim.pet_seq = 9;
    v->anim.shake_seq = 33;
    v->anim.nudge_seq[0] = 4;
    v->anim.nudge_seq[1] = 5;
    v->anim.confirm_seq[0] = 250;
    v->anim.confirm_seq[1] = 17;
}

static bool label_same(const vocab_view_label_t *a, const vocab_view_label_t *b)
{
    return a->visible == b->visible && strcmp(a->text, b->text) == 0 &&
           (!a->visible || a->color == b->color);
}

static bool panel_same(const vocab_view_panel_t *a, const vocab_view_panel_t *b)
{
    return a->visible == b->visible && a->style == b->style && a->text_en == b->text_en &&
           a->nudge_repeat == b->nudge_repeat && strcmp(a->text, b->text) == 0;
}

// Everything a stage must not write (spec 11.1 ownership, vocab_stage.h rule 8).
static bool session_fields_same(const vocab_view_t *a, const vocab_view_t *b)
{
    return a->screen == b->screen && label_same(&a->status.progress, &b->status.progress) &&
           label_same(&a->status.batt_pct, &b->status.batt_pct) &&
           a->status.volume == b->status.volume && a->status.battery == b->status.battery &&
           a->status.battery_low == b->status.battery_low && a->hint.toast == b->hint.toast &&
           strcmp(a->hint.text, b->hint.text) == 0 && a->hint.color == b->hint.color &&
           label_same(&a->card.banner, &b->card.banner) &&
           label_same(&a->home.title, &b->home.title) && label_same(&a->home.xp, &b->home.xp) &&
           label_same(&a->home.stats, &b->home.stats) && label_same(&a->home.warn, &b->home.warn) &&
           label_same(&a->home.go, &b->home.go) &&
           label_same(&a->summary.score, &b->summary.score) &&
           label_same(&a->summary.xp, &b->summary.xp) &&
           label_same(&a->summary.levelup, &b->summary.levelup) &&
           a->pet.visible == b->pet.visible && a->pet.stage == b->pet.stage &&
           a->pet.on_summary == b->pet.on_summary && a->pet.idle == b->pet.idle &&
           a->pet.level_up == b->pet.level_up && a->xpbar.visible == b->xpbar.visible &&
           a->xpbar.on_summary == b->xpbar.on_summary && a->xpbar.value == b->xpbar.value &&
           a->xpbar.max == b->xpbar.max && a->anim.pulse_seq == b->anim.pulse_seq &&
           a->anim.pet_seq == b->anim.pet_seq && a->anim.shake_seq == b->anim.shake_seq &&
           a->anim.nudge_seq[0] == b->anim.nudge_seq[0] &&
           a->anim.nudge_seq[1] == b->anim.nudge_seq[1] &&
           a->anim.confirm_seq[1] == b->anim.confirm_seq[1];
}

// The whole view, field by field.
static bool view_same(const vocab_view_t *a, const vocab_view_t *b)
{
    const vocab_view_card_t *x = &a->card;
    const vocab_view_card_t *y = &b->card;
    return session_fields_same(a, b) && x->frame == y->frame && label_same(&x->word, &y->word) &&
           label_same(&x->ipa, &y->ipa) && label_same(&x->meaning, &y->meaning) &&
           label_same(&x->sentence, &y->sentence) && label_same(&x->example, &y->example) &&
           label_same(&x->s3_meaning, &y->s3_meaning) && label_same(&x->prompt, &y->prompt) &&
           x->skeleton.visible == y->skeleton.visible &&
           panel_same(&a->panel[0], &b->panel[0]) && panel_same(&a->panel[1], &b->panel[1]) &&
           a->badge == b->badge && a->hint.kind == b->hint.kind &&
           a->anim.confirm_seq[0] == b->anim.confirm_seq[0];
}

// ---------------------------------------------------------------------------------------
// Outbox and result expectations
// ---------------------------------------------------------------------------------------

static vocab_cmd_t mk(vocab_cmd_kind_t kind, vocab_cue_t cue, vocab_timer_id_t timer, vocab_uid_t uid,
                      uint32_t value)
{
    const vocab_cmd_t cmd = { kind, cue, timer, uid, value };
    return cmd;
}

#define C_WORD(uid)          mk(VOCAB_CMD_PLAY_WORD, VOCAB_CUE_NONE, VOCAB_TIMER_STAGE, (uid), 0)
#define C_CUE(cue)           mk(VOCAB_CMD_PLAY_CUE, (cue), VOCAB_TIMER_STAGE, VOCAB_UID_NONE, 0)
#define C_CUE_WORD(cue, uid) mk(VOCAB_CMD_PLAY_CUE_THEN_WORD, (cue), VOCAB_TIMER_STAGE, (uid), 0)
#define C_TIMER(id, ms)      mk(VOCAB_CMD_TIMER_START, VOCAB_CUE_NONE, (id), VOCAB_UID_NONE, (ms))
#define C_CANCEL(id)         mk(VOCAB_CMD_TIMER_CANCEL, VOCAB_CUE_NONE, (id), VOCAB_UID_NONE, 0)
#define C_EPOCH()            mk(VOCAB_CMD_NEW_EPOCH, VOCAB_CUE_NONE, VOCAB_TIMER_STAGE, VOCAB_UID_NONE, 0)

static bool cmd_same(const vocab_cmd_t *a, const vocab_cmd_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case VOCAB_CMD_PLAY_WORD: return a->uid == b->uid;
    case VOCAB_CMD_PLAY_CUE: return a->cue == b->cue;
    case VOCAB_CMD_PLAY_CUE_THEN_WORD: return a->cue == b->cue && a->uid == b->uid;
    case VOCAB_CMD_TIMER_START: return a->timer == b->timer && a->value == b->value;
    case VOCAB_CMD_TIMER_CANCEL: return a->timer == b->timer;
    default: return true;
    }
}

static bool out_is(const vocab_outbox_t *out, const vocab_cmd_t *want, size_t n)
{
    if (out->overflow || out->count != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (!cmd_same(&out->cmd[i], &want[i])) return false;
    }
    return true;
}

static const char *describe_out(const vocab_outbox_t *out)
{
    static char buf[320];
    size_t len = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < out->count && len + 48 < sizeof(buf); i++) {
        const vocab_cmd_t *c = &out->cmd[i];
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "[kind %d cue %d timer %d uid %u val %u]",
                                (int)c->kind, (int)c->cue, (int)c->timer, (unsigned)c->uid,
                                (unsigned)c->value);
    }
    if (out->overflow && len + 12 < sizeof(buf)) snprintf(buf + len, sizeof(buf) - len, "+overflow");
    return out->count == 0 && !out->overflow ? "(empty)" : buf;
}

#define EXPECT_OUT(what, ...)                                                                  \
    do {                                                                                       \
        const vocab_cmd_t want_[] = { __VA_ARGS__ };                                           \
        CHECK(out_is(&F.out, want_, ARRAY_LEN(want_)), "%s: outbox %s", (what),               \
              describe_out(&F.out));                                                           \
    } while (0)

#define EXPECT_NO_OUT(what)                                                                    \
    CHECK(F.out.count == 0 && !F.out.overflow, "%s: unexpected outbox %s", (what),            \
          describe_out(&F.out))

static bool res_is(vocab_stage_result_t r, vocab_stage_status_t status, bool locked, bool outcome,
                   vocab_result_t result)
{
    if (r.status != status || r.outcome_ready != outcome) return false;
    if (status == VOCAB_STAGE_RUNNING && r.locked != locked) return false;
    return !outcome || r.result == result;
}

#define EXPECT_RES(what, st_, lk_, oc_, rs_)                                                   \
    CHECK(res_is(F.res, (st_), (lk_), (oc_), (rs_)),                                           \
          "%s: result status %d locked %d outcome %d result %d", (what), (int)F.res.status,     \
          (int)F.res.locked, (int)F.res.outcome_ready, (int)F.res.result)
#define EXPECT_RUNNING(what, lk_)                                                              \
    EXPECT_RES((what), VOCAB_STAGE_RUNNING, (lk_), false, VOCAB_RESULT_NONE)
#define EXPECT_DONE(what) EXPECT_RES((what), VOCAB_STAGE_DONE, false, false, VOCAB_RESULT_NONE)

// ---------------------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------------------

static vocab_index_t index_of(vocab_uid_t uid)
{
    vocab_index_t index = VOCAB_INDEX_NONE;
    const bool found = vocab_bank_find(vocab_bank_builtin(), uid, &index);
    CHECK(found, "uid %u is in the builtin bank", (unsigned)uid);
    return index;
}

static void begin_with(const vocab_bank_t *bank, vocab_index_t index, vocab_purpose_t purpose,
                       bool cloze_front, bool allow_too_easy, vocab_ms_t now)
{
    memset(&F, 0, sizeof(F));
    F.entry = vocab_bank_at(bank, index);
    F.purpose = purpose;
    vocab_view_init(&F.view);
    set_session_sentinels(&F.view);
    vocab_view_reset_card(&F.view);  // what the session does before begin()
    F.ref = F.view;
    vocab_rng_seed(&F.rng, 4242u);
    F.rng_state = F.rng.state;
    const vocab_stage_input_t in = {
        .bank = bank,
        .index = index,
        .purpose = purpose,
        .format = VOCAB_FORMAT_S2,
        .mastery = 1,
        .lure_index = VOCAB_INDEX_NONE,
        .cloze_front = cloze_front,
        .allow_too_easy = allow_too_easy,
        .rng = &F.rng,
    };
    vocab_outbox_clear(&F.out);
    F.res = vocab_stage2_begin(&F.st, &in, now, &F.view, &F.out);
}

// A LEARN card of the builtin bank with "too easy" allowed (the usual session input).
static void begin_uid(vocab_uid_t uid, bool cloze_front, vocab_ms_t now)
{
    begin_with(vocab_bank_builtin(), index_of(uid), VOCAB_PURPOSE_LEARN, cloze_front, true, now);
}

static void send(vocab_event_t ev, vocab_ms_t now)
{
    vocab_outbox_clear(&F.out);
    F.res = vocab_stage2_handle(&F.st, &ev, now, &F.view, &F.out);
}

static void click(vocab_key_t key, vocab_ms_t now) { send(vocab_event_click(key), now); }
static void hold(vocab_key_t key, vocab_ms_t now) { send(vocab_event_long(key), now); }
static void fire(vocab_timer_id_t id, vocab_ms_t now) { send(vocab_event_timer(id), now); }

static void resume_at(vocab_ms_t now)
{
    vocab_outbox_clear(&F.out);
    F.res = vocab_stage2_resume(&F.st, now, &F.view, &F.out);
}

// ---------------------------------------------------------------------------------------
// View expectations per face (spec 6.1, 6.2, 11.4, 11.5) and English safety (AC-27)
// ---------------------------------------------------------------------------------------

typedef enum { FACE_WORD, FACE_CLOZE, FACE_BACK, FACE_ACK } face_t;

static void expect_label(const char *ctx, const char *name, const vocab_view_label_t *l,
                         const char *text, uint32_t color)
{
    CHECK(l->visible, "%s: %s visible", ctx, name);
    CHECK(strcmp(l->text, text ? text : "") == 0, "%s: %s text '%s' (want '%s')", ctx, name,
          l->text, text ? text : "");
    CHECK(l->color == color, "%s: %s color %06X (want %06X)", ctx, name, (unsigned)l->color,
          (unsigned)color);
}

static void expect_hidden(const char *ctx, const char *name, const vocab_view_label_t *l)
{
    CHECK(!l->visible && l->text[0] == '\0', "%s: %s hidden (text '%s')", ctx, name, l->text);
}

static void expect_panel(const char *ctx, vocab_side_t side, const char *text,
                         vocab_panel_style_t style)
{
    const vocab_view_panel_t *p = &F.view.panel[side];
    CHECK(p->visible, "%s: panel %d visible", ctx, (int)side);
    CHECK(strcmp(p->text, text) == 0, "%s: panel %d text '%s'", ctx, (int)side, p->text);
    CHECK(p->style == style, "%s: panel %d style %d (want %d)", ctx, (int)side, (int)p->style,
          (int)style);
    CHECK(!p->text_en && !p->nudge_repeat, "%s: panel %d is Chinese text without nudges", ctx,
          (int)side);
}

static bool has_ascii_letter(const char *s)
{
    for (; s && *s; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) return true;
    }
    return false;
}

static bool same_text(const char *a, const char *b)
{
    return b && strcmp(a, b) == 0;
}

// Number of runs of '_' in `s`; *longest receives the longest run.
static int underscore_runs(const char *s, size_t *longest)
{
    int runs = 0;
    size_t run = 0;
    *longest = 0;
    for (;; s++) {
        if (*s == '_') {
            if (run++ == 0) runs++;
            if (run > *longest) *longest = run;
        } else {
            run = 0;
            if (*s == '\0') break;
        }
    }
    return runs;
}

// AC-27 for the stage region: every English string is bank text of this card (the entry
// text, its IPA, its example, or its cloze sentence with exactly one four-underscore
// blank); panels never carry English; the Stage 3 skeleton is never shown.
static void expect_english_safe(const char *ctx, const vocab_view_t *v, const vocab_entry_t *e)
{
    const struct {
        const char *name;
        const vocab_view_label_t *label;
    } labels[] = {
        { "word", &v->card.word },       { "ipa", &v->card.ipa },
        { "meaning", &v->card.meaning }, { "sentence", &v->card.sentence },
        { "example", &v->card.example }, { "s3_meaning", &v->card.s3_meaning },
        { "prompt", &v->card.prompt },   { "banner", &v->card.banner },
    };
    for (size_t i = 0; i < ARRAY_LEN(labels); i++) {
        const vocab_view_label_t *l = labels[i].label;
        if (!l->visible || !has_ascii_letter(l->text)) continue;
        const bool ok = same_text(l->text, e->text) || same_text(l->text, e->ipa) ||
                        same_text(l->text, e->example) ||
                        (l == &v->card.sentence && same_text(l->text, e->example_cloze));
        CHECK(ok, "%s: English '%s' in %s is not bank text of uid %u", ctx, l->text,
              labels[i].name, (unsigned)e->uid);
    }
    for (int p = 0; p < 2; p++) {
        CHECK(!v->panel[p].visible || (!v->panel[p].text_en && !has_ascii_letter(v->panel[p].text)),
              "%s: panel %d shows no English", ctx, p);
    }
    CHECK(!v->card.skeleton.visible, "%s: no Stage 3 skeleton", ctx);
    if (v->card.sentence.visible) {
        size_t longest = 0;
        CHECK(underscore_runs(v->card.sentence.text, &longest) == 1 && longest == 4u,
              "%s: the cloze sentence '%s' has exactly one blank of four underscores", ctx,
              v->card.sentence.text);
        CHECK(!v->card.word.visible && !v->card.ipa.visible,
              "%s: the cloze front hides the word and its IPA", ctx);
    }
}

static void expect_face(const char *ctx, face_t face, vocab_hint_t hint, vocab_badge_t badge)
{
    const vocab_view_t *v = &F.view;
    const vocab_view_card_t *c = &v->card;
    const vocab_entry_t *e = F.entry;
    CHECK(c->frame, "%s: card frame", ctx);
    expect_hidden(ctx, "s3_meaning", &c->s3_meaning);
    switch (face) {
    case FACE_WORD:
        expect_label(ctx, "word", &c->word, e->text, VOCAB_RGB_TEXT);
        if (vocab_entry_has_ipa(e)) {
            expect_label(ctx, "ipa", &c->ipa, e->ipa, VOCAB_RGB_DIM);
        } else {
            expect_hidden(ctx, "ipa", &c->ipa);
        }
        expect_label(ctx, "prompt", &c->prompt, VOCAB_STR_S2_FRONT_WORD, VOCAB_RGB_YELLOW);
        expect_hidden(ctx, "meaning", &c->meaning);
        expect_hidden(ctx, "sentence", &c->sentence);
        expect_hidden(ctx, "example", &c->example);
        CHECK(!v->panel[0].visible && !v->panel[1].visible, "%s: panels hidden on the front", ctx);
        break;
    case FACE_CLOZE:
        expect_label(ctx, "sentence", &c->sentence, e->example_cloze, VOCAB_RGB_TEXT);
        expect_label(ctx, "prompt", &c->prompt, VOCAB_STR_S2_FRONT_CLOZE, VOCAB_RGB_YELLOW);
        expect_hidden(ctx, "word", &c->word);
        expect_hidden(ctx, "ipa", &c->ipa);
        expect_hidden(ctx, "meaning", &c->meaning);
        expect_hidden(ctx, "example", &c->example);
        CHECK(!v->panel[0].visible && !v->panel[1].visible, "%s: panels hidden on the front", ctx);
        break;
    case FACE_BACK:
    case FACE_ACK:
        expect_label(ctx, "word", &c->word, e->text, VOCAB_RGB_TEXT);
        expect_hidden(ctx, "ipa", &c->ipa);
        expect_hidden(ctx, "sentence", &c->sentence);
        expect_label(ctx, "meaning", &c->meaning, e->zh, VOCAB_RGB_YELLOW);
        if (face == FACE_BACK) {
            expect_label(ctx, "example", &c->example, e->example, VOCAB_RGB_SOFT);
            expect_hidden(ctx, "prompt", &c->prompt);
        } else {
            expect_hidden(ctx, "example", &c->example);
            expect_label(ctx, "prompt", &c->prompt, VOCAB_STR_S2_FORGOT_ACK, VOCAB_RGB_WARN);
        }
        expect_panel(ctx, VOCAB_SIDE_UP, VOCAB_STR_S2_KNOW, VOCAB_PANEL_KNOW);
        expect_panel(ctx, VOCAB_SIDE_DOWN, VOCAB_STR_S2_FORGOT, VOCAB_PANEL_FORGOT);
        break;
    }
    CHECK(v->badge == badge, "%s: badge %d (want %d)", ctx, (int)v->badge, (int)badge);
    CHECK(v->hint.kind == hint, "%s: hint %d (want %d)", ctx, (int)v->hint.kind, (int)hint);
    CHECK(session_fields_same(v, &F.ref), "%s: session-owned view fields untouched", ctx);
    CHECK(F.rng.state == F.rng_state, "%s: Stage 2 never draws from the RNG", ctx);
    expect_english_safe(ctx, v, e);
}

// The hint bar text the session resolves for the current hint kind (spec 11.5).
static void expect_hint_text(const char *ctx, const char *with_audio, const char *without_audio)
{
    vocab_view_t copy = F.view;
    copy.hint.toast = VOCAB_TOAST_NONE;
    vocab_view_resolve(&copy, true);
    CHECK(strcmp(copy.hint.text, with_audio) == 0 && copy.hint.color == VOCAB_RGB_HINT,
          "%s: hint '%s' with word audio", ctx, copy.hint.text);
    vocab_view_resolve(&copy, false);
    CHECK(strcmp(copy.hint.text, without_audio) == 0, "%s: hint '%s' without word audio", ctx,
          copy.hint.text);
}

// =======================================================================================
// Tests
// =======================================================================================

// Spec 6.1 word front, 8.3 entry audio, 11.5 "Stage 2 word front".
static void test_word_front(void)
{
    begin_uid(201, false, 1000);  // cat, with IPA
    EXPECT_RUNNING("begin", false);
    EXPECT_OUT("begin: word clip + front minimum", C_WORD(201),
               C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS));
    expect_face("word front", FACE_WORD, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
    expect_hint_text("word front", VOCAB_STR_HINT_THINK, VOCAB_STR_HINT_THINK);

    begin_uid(902, false, 0);  // "Thank you": a phrase without IPA
    EXPECT_RUNNING("phrase begin", false);
    expect_face("phrase front", FACE_WORD, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);

    begin_uid(101, false, 0);  // apple has a cloze sentence, but the session chose the word
    expect_face("word front by request", FACE_WORD, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
}

// Spec 6.1 cloze-sentence front, 4.6, AC-27 (always "____", the word is not shown).
static void test_cloze_front(void)
{
    begin_uid(101, true, 0);  // apple
    EXPECT_RUNNING("cloze begin", false);
    EXPECT_OUT("cloze begin", C_WORD(101), C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS));
    expect_face("cloze front", FACE_CLOZE, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
    CHECK(strcmp(F.view.card.sentence.text, "I eat an ____.") == 0, "apple cloze '%s'",
          F.view.card.sentence.text);

    begin_uid(901, true, 0);  // a phrase whose blank starts the sentence
    expect_face("phrase cloze front", FACE_CLOZE, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
    CHECK(strcmp(F.view.card.sentence.text, "____, Miss Li!") == 0, "phrase cloze '%s'",
          F.view.card.sentence.text);

    begin_uid(904, true, 0);  // "How are you?" has no cloze sentence: word front
    expect_face("no cloze sentence", FACE_WORD, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
}

// Cloze-front guard: only an exact "example with the text replaced by ____" is shown.
typedef struct {
    const char *example;
    const char *cloze;
    bool accepted;
    const char *why;
} guard_case_t;

static void test_cloze_front_guard(void)
{
    static const guard_case_t cases[] = {
        { "I fly a kite.", "I fly a ____.", true, "exact derivation" },
        { "kite", "____", true, "blank only" },
        { "I fly a kite.", "I fly a _____.", false, "five underscores" },
        { "I fly a kite.", "I fly a ___.", false, "three underscores" },
        { "I fly a kite.", "I ____ a ____.", false, "two blanks" },
        { "I fly a kite.", "I fly a kite.", false, "no blank (answer visible)" },
        { "I fly a kite.", "I fli a ____.", false, "altered prefix" },
        { "I fly a kite.", "I fly a ____!", false, "altered suffix" },
        { "I fly a kite.", "I ____ a kite.", false, "blank over another word" },
        { "A kite, a kite.", "A ____, a kite.", false, "text twice in the example" },
        { "my_kite: kite", "my_kite: ____", false, "text inside another word" },
        { "kite_", "_____", false, "underscore next to the blank (five in a row)" },
        { "a_b kite", "a_b ____", false, "another underscore in the sentence" },
        { NULL, "I fly a ____.", false, "no example" },
        { "I fly a kite.", "", false, "empty cloze" },
    };
    const vocab_entry_t *meaning_src = vocab_bank_at(vocab_bank_builtin(), 0);
    vocab_entry_t entries[ARRAY_LEN(cases)];
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].uid = (vocab_uid_t)(7001u + i);
        entries[i].category = 1;
        entries[i].text = "kite";
        entries[i].ipa = "/kaɪt/";
        entries[i].zh = meaning_src->zh;
        entries[i].example = cases[i].example;
        entries[i].example_cloze = cases[i].cloze;
    }
    const vocab_bank_t bank = { entries, (vocab_index_t)ARRAY_LEN(entries) };
    for (vocab_index_t i = 0; i < bank.count; i++) {
        char ctx[96];
        snprintf(ctx, sizeof(ctx), "guard '%s'", cases[i].why);
        begin_with(&bank, i, VOCAB_PURPOSE_LEARN, true, true, 100);
        EXPECT_RUNNING(ctx, false);
        expect_face(ctx, cases[i].accepted ? FACE_CLOZE : FACE_WORD, VOCAB_HINT_THINK,
                    VOCAB_BADGE_RECALL);
        fire(VOCAB_TIMER_STAGE, 1100);
        click(VOCAB_KEY_OK, 1200);
        if (cases[i].example) {
            expect_face(ctx, FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
        } else {
            expect_hidden(ctx, "example without data", &F.view.card.example);
            expect_english_safe(ctx, &F.view, F.entry);
        }
    }
}

// Spec 6.1, 3.2 "Stage 2 front" rows, AC-17: clicks ignored for 1000 ms, then each key flips.
static void test_front_minimum(void)
{
    const vocab_key_t keys[] = { VOCAB_KEY_UP, VOCAB_KEY_DOWN, VOCAB_KEY_OK };
    const uint32_t early[] = { 0u, 1u, 250u, 500u, 998u, 999u };
    for (size_t k = 0; k < ARRAY_LEN(keys); k++) {
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "front minimum, key %d", (int)keys[k]);
        const vocab_ms_t t0 = 5000;
        begin_uid(201, false, t0);
        const vocab_view_t before = F.view;
        for (size_t i = 0; i < ARRAY_LEN(early); i++) {
            click(keys[k], t0 + early[i]);
            EXPECT_RUNNING(ctx, false);
            EXPECT_NO_OUT(ctx);
            CHECK(view_same(&F.view, &before), "%s: click at +%u ms changes nothing", ctx,
                  (unsigned)early[i]);
        }
        hold(VOCAB_KEY_DOWN, t0 + 500);   // never reaches a stage; must be ignored anyway
        EXPECT_RUNNING(ctx, false);
        EXPECT_NO_OUT(ctx);
        hold(VOCAB_KEY_OK, t0 + 600);
        EXPECT_RUNNING(ctx, false);
        EXPECT_NO_OUT(ctx);
        fire(VOCAB_TIMER_STAGE_AUX, t0 + 700);  // not Stage 2's timer
        EXPECT_RUNNING(ctx, false);
        EXPECT_NO_OUT(ctx);
        CHECK(view_same(&F.view, &before), "%s: front unchanged before the minimum", ctx);

        fire(VOCAB_TIMER_STAGE, t0 + 1000);
        EXPECT_RUNNING(ctx, false);
        EXPECT_NO_OUT(ctx);
        expect_face(ctx, FACE_WORD, VOCAB_HINT_FLIP, VOCAB_BADGE_RECALL);
        expect_hint_text(ctx, VOCAB_STR_HINT_FLIP, VOCAB_STR_HINT_FLIP);

        click(keys[k], t0 + 1000);
        EXPECT_RUNNING(ctx, false);
        EXPECT_OUT(ctx, C_CUE_WORD(VOCAB_CUE_FLIP, 201), C_EPOCH());
        expect_face(ctx, FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
        expect_hint_text(ctx, VOCAB_STR_HINT_LISTEN, VOCAB_STR_HINT_LOOK);
    }
}

// The minimum also ends at its deadline when the click is handled before the timer event;
// the pending timer is cancelled and a stale timer event is ignored. Wrap-around safe.
static void test_front_deadline_without_timer(void)
{
    const vocab_key_t keys[] = { VOCAB_KEY_UP, VOCAB_KEY_DOWN, VOCAB_KEY_OK };
    for (size_t k = 0; k < ARRAY_LEN(keys); k++) {
        begin_uid(303, false, 7000);  // bike
        click(keys[k], 7999);
        EXPECT_RUNNING("deadline - 1", false);
        EXPECT_NO_OUT("deadline - 1");
        expect_face("deadline - 1", FACE_WORD, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
        click(keys[k], 8000);
        EXPECT_RUNNING("click at the deadline", false);
        EXPECT_OUT("click at the deadline", C_CANCEL(VOCAB_TIMER_STAGE),
                   C_CUE_WORD(VOCAB_CUE_FLIP, 303), C_EPOCH());
        expect_face("click at the deadline", FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
        const vocab_view_t back = F.view;
        fire(VOCAB_TIMER_STAGE, 8000);
        EXPECT_RUNNING("stale front timer", false);
        EXPECT_NO_OUT("stale front timer");
        CHECK(view_same(&F.view, &back), "stale front timer changes nothing");
    }

    // A late timer: the click 500 ms after the deadline flips even before the timer event.
    begin_uid(303, true, 0);
    click(VOCAB_KEY_OK, 1500);
    EXPECT_OUT("late timer", C_CANCEL(VOCAB_TIMER_STAGE), C_CUE_WORD(VOCAB_CUE_FLIP, 303),
               C_EPOCH());

    // Millisecond counter wrap: the deadline wraps past zero.
    const vocab_ms_t t0 = UINT32_MAX - 399u;  // deadline = 600 after the wrap
    begin_uid(303, false, t0);
    click(VOCAB_KEY_OK, t0 + 999u);
    EXPECT_RUNNING("wrap: 999 ms", false);
    EXPECT_NO_OUT("wrap: 999 ms");
    click(VOCAB_KEY_OK, t0 + 1000u);
    EXPECT_OUT("wrap: 1000 ms", C_CANCEL(VOCAB_TIMER_STAGE), C_CUE_WORD(VOCAB_CUE_FLIP, 303),
               C_EPOCH());
    expect_face("wrap: back", FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
}

// Brings the current card to the back at `t` (front minimum via its timer).
static void flip_at(vocab_ms_t t0)
{
    fire(VOCAB_TIMER_STAGE, t0 + VOCAB_S2_FRONT_MIN_MS);
    click(VOCAB_KEY_OK, t0 + VOCAB_S2_FRONT_MIN_MS + 100u);
}

// Every intent and the AUX timer is ignored in a lock window (spec 3.2 lock rows).
static void expect_lock_ignores(const char *ctx, vocab_ms_t t)
{
    const vocab_view_t before = F.view;
    const vocab_event_t events[] = {
        vocab_event_click(VOCAB_KEY_UP),   vocab_event_click(VOCAB_KEY_DOWN),
        vocab_event_click(VOCAB_KEY_OK),   vocab_event_long(VOCAB_KEY_UP),
        vocab_event_long(VOCAB_KEY_DOWN),  vocab_event_long(VOCAB_KEY_OK),
        vocab_event_timer(VOCAB_TIMER_STAGE_AUX),
    };
    for (size_t i = 0; i < ARRAY_LEN(events); i++) {
        send(events[i], t + (vocab_ms_t)i);
        EXPECT_RUNNING(ctx, true);
        EXPECT_NO_OUT(ctx);
        CHECK(view_same(&F.view, &before), "%s: event %u changes nothing", ctx, (unsigned)i);
    }
}

// Spec 6.2 UP = I know (O3), CONFIRM_FLASH, S2_CONFIRM_MS lock, then the next card.
static void test_back_know(void)
{
    begin_uid(203, false, 0);  // duck
    flip_at(0);
    expect_face("back", FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
    const uint8_t up_seq = F.view.anim.confirm_seq[0];
    click(VOCAB_KEY_UP, 1500);
    EXPECT_RES("I know", VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_KNOW);
    EXPECT_OUT("I know", C_CUE(VOCAB_CUE_TICK), C_EPOCH(),
               C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_CONFIRM_MS));
    CHECK(F.view.anim.confirm_seq[0] == (uint8_t)(up_seq + 1u), "CONFIRM_FLASH on the UP panel");
    expect_face("confirm window", FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
    expect_lock_ignores("confirm window", 1600);
    fire(VOCAB_TIMER_STAGE, 1500 + VOCAB_S2_CONFIRM_MS);
    EXPECT_DONE("confirm ends the card");
    EXPECT_NO_OUT("confirm ends the card");

    // After DONE nothing happens (and no second outcome).
    click(VOCAB_KEY_UP, 2000);
    EXPECT_DONE("after done: click");
    EXPECT_NO_OUT("after done: click");
    fire(VOCAB_TIMER_STAGE, 2100);
    EXPECT_DONE("after done: timer");
    resume_at(2200);
    EXPECT_DONE("after done: resume");
    EXPECT_NO_OUT("after done: resume");

    // The flash counter wraps like every animation counter.
    begin_uid(203, false, 0);
    flip_at(0);
    F.view.anim.confirm_seq[0] = 255;
    click(VOCAB_KEY_UP, 1500);
    CHECK(F.view.anim.confirm_seq[0] == 0, "confirm counter wraps");
}

// Spec 6.2 DOWN = forgot, test me later (O4): orange acknowledgement for S2_FORGOT_ACK_MS.
static void test_back_forgot(void)
{
    begin_uid(1001, false, 0);  // "Stand up"
    flip_at(0);
    const uint8_t up_seq = F.view.anim.confirm_seq[0];
    click(VOCAB_KEY_DOWN, 1400);
    EXPECT_RES("forgot", VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_FORGOT);
    EXPECT_OUT("forgot", C_CUE(VOCAB_CUE_TICK), C_EPOCH(),
               C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FORGOT_ACK_MS));
    CHECK(F.view.anim.confirm_seq[0] == up_seq, "no confirm flash for forgot");
    expect_face("forgot acknowledgement", FACE_ACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
    expect_lock_ignores("forgot acknowledgement", 1500);
    fire(VOCAB_TIMER_STAGE, 1400 + VOCAB_S2_FORGOT_ACK_MS);
    EXPECT_DONE("acknowledgement ends the card");
    EXPECT_NO_OUT("acknowledgement ends the card");
}

// Spec 6.2 OK = replay on the back; UP LONG and stale timers are ignored there (AC-22).
static void test_back_replay_and_ignored(void)
{
    begin_uid(305, false, 0);  // train
    flip_at(0);
    const vocab_view_t back = F.view;
    for (int i = 0; i < 3; i++) {
        click(VOCAB_KEY_OK, (vocab_ms_t)(1200 + 100 * i));
        EXPECT_RUNNING("replay", false);
        EXPECT_OUT("replay", C_WORD(305));
        CHECK(view_same(&F.view, &back), "replay changes nothing on screen");
    }
    const vocab_event_t ignored[] = {
        vocab_event_long(VOCAB_KEY_UP), vocab_event_long(VOCAB_KEY_DOWN),
        vocab_event_long(VOCAB_KEY_OK), vocab_event_timer(VOCAB_TIMER_STAGE),
        vocab_event_timer(VOCAB_TIMER_STAGE_AUX),
    };
    for (size_t i = 0; i < ARRAY_LEN(ignored); i++) {
        send(ignored[i], 2000);
        EXPECT_RUNNING("ignored on the back", false);
        EXPECT_NO_OUT("ignored on the back");
        CHECK(view_same(&F.view, &back), "event %u ignored on the back", (unsigned)i);
    }
    click(VOCAB_KEY_UP, 2500);
    EXPECT_RES("rating still works", VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_KNOW);
}

// Spec 4.8 / 3.2: UP LONG = too easy on the front of LEARN cards only (AC-21/22).
static void test_too_easy(void)
{
    begin_uid(201, false, 0);
    hold(VOCAB_KEY_UP, 600);  // inside the front minimum: still allowed (not a lock)
    EXPECT_RES("too easy during the minimum", VOCAB_STAGE_DONE, false, true, VOCAB_RESULT_TOO_EASY);
    EXPECT_NO_OUT("too easy during the minimum");
    click(VOCAB_KEY_OK, 2000);
    EXPECT_DONE("too easy ended the card");
    EXPECT_NO_OUT("too easy ended the card");

    begin_uid(201, false, 0);
    fire(VOCAB_TIMER_STAGE, 1000);
    hold(VOCAB_KEY_UP, 1500);
    EXPECT_RES("too easy after the minimum", VOCAB_STAGE_DONE, false, true, VOCAB_RESULT_TOO_EASY);

    begin_uid(201, false, 0);
    hold(VOCAB_KEY_UP, 1200);  // minimum elapsed by time, timer not delivered yet
    EXPECT_RES("too easy at the deadline", VOCAB_STAGE_DONE, false, true, VOCAB_RESULT_TOO_EASY);

    begin_uid(204, true, 0);  // cloze front
    hold(VOCAB_KEY_UP, 10);
    EXPECT_RES("too easy on a cloze front", VOCAB_STAGE_DONE, false, true, VOCAB_RESULT_TOO_EASY);

    // Not allowed by the session: ignored, the card goes on normally.
    begin_with(vocab_bank_builtin(), index_of(201), VOCAB_PURPOSE_LEARN, false, false, 0);
    const vocab_view_t front = F.view;
    hold(VOCAB_KEY_UP, 300);
    EXPECT_RUNNING("too easy not allowed", false);
    EXPECT_NO_OUT("too easy not allowed");
    CHECK(view_same(&F.view, &front), "too easy not allowed: nothing changes");
    flip_at(0);
    EXPECT_RUNNING("flip after an ignored too easy", false);
    click(VOCAB_KEY_DOWN, 1500);
    EXPECT_RES("rating after an ignored too easy", VOCAB_STAGE_RUNNING, true, true,
               VOCAB_RESULT_FORGOT);

    // DOWN LONG / OK LONG are the session's (volume, pause): never "too easy".
    begin_uid(201, false, 0);
    hold(VOCAB_KEY_DOWN, 100);
    EXPECT_RUNNING("DOWN LONG on the front", false);
    hold(VOCAB_KEY_OK, 200);
    EXPECT_RUNNING("OK LONG on the front", false);
    EXPECT_NO_OUT("OK LONG on the front");

    // Inserted cards (only possible as a fallback) never accept too easy (AC-22).
    const vocab_purpose_t inserted[] = { VOCAB_PURPOSE_TEST_LATER, VOCAB_PURPOSE_SPOT_CHECK,
                                         VOCAB_PURPOSE_TOO_EASY_CHECK };
    for (size_t i = 0; i < ARRAY_LEN(inserted); i++) {
        begin_with(vocab_bank_builtin(), index_of(201), inserted[i], false, true, 0);
        hold(VOCAB_KEY_UP, 100);
        EXPECT_RUNNING("too easy on an inserted card", false);
        EXPECT_NO_OUT("too easy on an inserted card");
    }
}

// The stage runs any purpose (session fallback) with the purpose badge on both faces.
static void test_inserted_purpose_badges(void)
{
    const vocab_purpose_t purposes[] = { VOCAB_PURPOSE_TEST_LATER, VOCAB_PURPOSE_SPOT_CHECK,
                                         VOCAB_PURPOSE_TOO_EASY_CHECK };
    const vocab_badge_t badges[] = { VOCAB_BADGE_TEST_AGAIN, VOCAB_BADGE_SPOT_CHECK,
                                     VOCAB_BADGE_TOO_EASY };
    for (size_t i = 0; i < ARRAY_LEN(purposes); i++) {
        begin_with(vocab_bank_builtin(), index_of(402), purposes[i], false, true, 0);
        expect_face("inserted front", FACE_WORD, VOCAB_HINT_THINK, badges[i]);
        flip_at(0);
        expect_face("inserted back", FACE_BACK, VOCAB_HINT_REPLAY, badges[i]);
        click(VOCAB_KEY_UP, 1500);
        EXPECT_RES("inserted: I know", VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_KNOW);
    }
}

// vocab_stage.h rule 1: resume() after a pause card re-arms what the state needs; no audio,
// no epoch, no outcome.
static void test_resume(void)
{
    // Front minimum not elapsed: the full minimum restarts at resume.
    begin_uid(502, false, 0);  // nose
    click(VOCAB_KEY_OK, 400);
    EXPECT_NO_OUT("before the pause");
    resume_at(3000);  // pause at 500; the session cancelled the front timer
    EXPECT_RUNNING("resume on the front", false);
    EXPECT_OUT("resume on the front", C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS));
    expect_face("resume on the front", FACE_WORD, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
    click(VOCAB_KEY_UP, 3999);
    EXPECT_RUNNING("minimum restarted", false);
    EXPECT_NO_OUT("minimum restarted");
    fire(VOCAB_TIMER_STAGE, 4000);
    expect_face("resumed minimum ends", FACE_WORD, VOCAB_HINT_FLIP, VOCAB_BADGE_RECALL);
    click(VOCAB_KEY_UP, 4000);
    EXPECT_OUT("flip after resume", C_CUE_WORD(VOCAB_CUE_FLIP, 502), C_EPOCH());

    // Same, flipping by time at the new deadline.
    begin_uid(502, true, 0);
    resume_at(800);
    click(VOCAB_KEY_DOWN, 1799);
    EXPECT_NO_OUT("resumed minimum, 999 ms");
    click(VOCAB_KEY_DOWN, 1800);
    EXPECT_OUT("resumed minimum, 1000 ms", C_CANCEL(VOCAB_TIMER_STAGE),
               C_CUE_WORD(VOCAB_CUE_FLIP, 502), C_EPOCH());

    // Front ready: nothing to re-arm; any key flips at once.
    begin_uid(502, false, 0);
    fire(VOCAB_TIMER_STAGE, 1000);
    resume_at(5000);
    EXPECT_RUNNING("resume, front ready", false);
    EXPECT_NO_OUT("resume, front ready");
    expect_face("resume, front ready", FACE_WORD, VOCAB_HINT_FLIP, VOCAB_BADGE_RECALL);
    click(VOCAB_KEY_UP, 5000);
    EXPECT_OUT("flip right after resume", C_CUE_WORD(VOCAB_CUE_FLIP, 502), C_EPOCH());

    // Back: nothing to re-arm.
    resume_at(6000);
    EXPECT_RUNNING("resume on the back", false);
    EXPECT_NO_OUT("resume on the back");
    expect_face("resume on the back", FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
    click(VOCAB_KEY_DOWN, 6100);
    EXPECT_RES("rating after resume", VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_FORGOT);

    // Lock windows (the session never pauses there): the window restarts, no new outcome.
    resume_at(6200);
    EXPECT_RUNNING("resume in the forgot window", true);
    EXPECT_OUT("resume in the forgot window", C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FORGOT_ACK_MS));
    expect_face("resume in the forgot window", FACE_ACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
    fire(VOCAB_TIMER_STAGE, 6800);
    EXPECT_DONE("forgot window ends after resume");

    begin_uid(502, false, 0);
    flip_at(0);
    click(VOCAB_KEY_UP, 1500);
    const uint8_t up_seq = F.view.anim.confirm_seq[0];
    resume_at(1600);
    EXPECT_RUNNING("resume in the confirm window", true);
    EXPECT_OUT("resume in the confirm window", C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_CONFIRM_MS));
    CHECK(F.view.anim.confirm_seq[0] == up_seq, "resume does not replay the flash");
    fire(VOCAB_TIMER_STAGE, 2000);
    EXPECT_DONE("confirm window ends after resume");

    // A stage region that was not restored is redrawn from the stage state.
    begin_uid(502, false, 0);
    flip_at(0);
    vocab_view_reset_card(&F.view);
    F.view.badge = VOCAB_BADGE_PAUSE;
    resume_at(9000);
    EXPECT_NO_OUT("redraw on resume");
    expect_face("redraw on resume", FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
}

// vocab_stage.h rule 1: unusable input -> DONE without an outcome, nothing drawn or queued.
static void test_unusable_input(void)
{
    vocab_stage2_t st;
    vocab_view_t view;
    vocab_view_t before;
    vocab_outbox_t out;
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 1);
    const vocab_entry_t *src = vocab_bank_at(vocab_bank_builtin(), 0);
    vocab_entry_t bad[2];
    memcpy(&bad[0], src, sizeof(bad[0]));
    memcpy(&bad[1], src, sizeof(bad[1]));
    bad[0].text = NULL;
    bad[1].text = "";
    const vocab_bank_t bad_bank = { bad, 2 };
    const vocab_bank_t *builtin = vocab_bank_builtin();
    const struct {
        const vocab_bank_t *bank;
        vocab_index_t index;
        const char *why;
    } cases[] = {
        { NULL, 0, "no bank" },
        { builtin, builtin->count, "index past the bank" },
        { builtin, VOCAB_INDEX_NONE, "index NONE" },
        { &bad_bank, 0, "NULL text" },
        { &bad_bank, 1, "empty text" },
    };
    for (size_t i = 0; i < ARRAY_LEN(cases) + 1u; i++) {
        const char *why = i < ARRAY_LEN(cases) ? cases[i].why : "NULL input";
        vocab_view_init(&view);
        set_session_sentinels(&view);
        vocab_view_reset_card(&view);
        before = view;
        vocab_outbox_clear(&out);
        vocab_stage_result_t r;
        if (i < ARRAY_LEN(cases)) {
            const vocab_stage_input_t in = {
                .bank = cases[i].bank, .index = cases[i].index, .purpose = VOCAB_PURPOSE_LEARN,
                .format = VOCAB_FORMAT_S2, .mastery = 1, .lure_index = VOCAB_INDEX_NONE,
                .cloze_front = true, .allow_too_easy = true, .rng = &rng,
            };
            r = vocab_stage2_begin(&st, &in, 0, &view, &out);
        } else {
            r = vocab_stage2_begin(&st, NULL, 0, &view, &out);
        }
        CHECK(r.status == VOCAB_STAGE_DONE && !r.outcome_ready, "%s: begin DONE, no outcome", why);
        CHECK(out.count == 0 && !out.overflow, "%s: nothing queued", why);
        CHECK(view_same(&view, &before), "%s: nothing drawn", why);
        const vocab_event_t ev = vocab_event_click(VOCAB_KEY_OK);
        r = vocab_stage2_handle(&st, &ev, 2000, &view, &out);
        CHECK(r.status == VOCAB_STAGE_DONE && !r.outcome_ready && out.count == 0,
              "%s: handle after an unusable begin", why);
        r = vocab_stage2_resume(&st, 3000, &view, &out);
        CHECK(r.status == VOCAB_STAGE_DONE && !r.outcome_ready && out.count == 0,
              "%s: resume after an unusable begin", why);
    }

    // NULL arguments never crash.
    begin_uid(201, false, 0);
    vocab_outbox_clear(&F.out);
    F.res = vocab_stage2_handle(&F.st, NULL, 100, &F.view, &F.out);
    EXPECT_RUNNING("NULL event", false);
    EXPECT_NO_OUT("NULL event");
    const vocab_event_t ev = vocab_event_click(VOCAB_KEY_OK);
    CHECK(vocab_stage2_handle(NULL, &ev, 0, &F.view, &F.out).status == VOCAB_STAGE_DONE,
          "NULL stage: handle");
    CHECK(vocab_stage2_resume(NULL, 0, &F.view, &F.out).status == VOCAB_STAGE_DONE,
          "NULL stage: resume");
    CHECK(vocab_stage2_begin(NULL, NULL, 0, &F.view, &F.out).status == VOCAB_STAGE_DONE,
          "NULL stage: begin");
}

// AC-27 and spec 6 over the whole bank: every entry, both fronts, every ending.
static void test_every_entry(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned cloze_fronts = 0;
    unsigned with_cloze_sentence = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = vocab_bank_at(bank, i);
        if (vocab_entry_has_example_cloze(e)) with_cloze_sentence++;
        for (int cloze = 0; cloze < 2; cloze++) {
            for (int ending = 0; ending < 3; ending++) {
                char ctx[80];
                snprintf(ctx, sizeof(ctx), "uid %u cloze %d ending %d", (unsigned)e->uid, cloze,
                         ending);
                const face_t front = cloze && vocab_entry_has_example_cloze(e) ? FACE_CLOZE
                                                                               : FACE_WORD;
                begin_with(bank, i, VOCAB_PURPOSE_LEARN, cloze != 0, true, 10000);
                EXPECT_RUNNING(ctx, false);
                EXPECT_OUT(ctx, C_WORD(e->uid), C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS));
                expect_face(ctx, front, VOCAB_HINT_THINK, VOCAB_BADGE_RECALL);
                if (ending == 0 && front == FACE_CLOZE) cloze_fronts++;
                if (ending == 2) {
                    hold(VOCAB_KEY_UP, 10500);
                    EXPECT_RES(ctx, VOCAB_STAGE_DONE, false, true, VOCAB_RESULT_TOO_EASY);
                    continue;
                }
                fire(VOCAB_TIMER_STAGE, 11000);
                expect_face(ctx, front, VOCAB_HINT_FLIP, VOCAB_BADGE_RECALL);
                click(VOCAB_KEY_DOWN, 11300);
                EXPECT_OUT(ctx, C_CUE_WORD(VOCAB_CUE_FLIP, e->uid), C_EPOCH());
                expect_face(ctx, FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
                click(VOCAB_KEY_OK, 11600);
                EXPECT_OUT(ctx, C_WORD(e->uid));
                if (ending == 0) {
                    click(VOCAB_KEY_UP, 12000);
                    EXPECT_RES(ctx, VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_KNOW);
                    expect_face(ctx, FACE_BACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
                    fire(VOCAB_TIMER_STAGE, 12400);
                } else {
                    click(VOCAB_KEY_DOWN, 12000);
                    EXPECT_RES(ctx, VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_FORGOT);
                    expect_face(ctx, FACE_ACK, VOCAB_HINT_REPLAY, VOCAB_BADGE_SELF_RATE);
                    fire(VOCAB_TIMER_STAGE, 12600);
                }
                EXPECT_DONE(ctx);
                EXPECT_NO_OUT(ctx);
            }
        }
    }
    CHECK(with_cloze_sentence == 45u, "45 bank entries have a cloze sentence (got %u)",
          with_cloze_sentence);
    CHECK(cloze_fronts == with_cloze_sentence, "every bank cloze sentence passes the guard (%u/%u)",
          cloze_fronts, with_cloze_sentence);
}

// =======================================================================================
// Randomized event fuzz against a reference model of the session + stage behavior
// =======================================================================================

typedef enum {
    M_FRONT_WAIT,
    M_FRONT_READY,
    M_BACK,
    M_CONFIRM,
    M_FORGOT_ACK,
    M_DONE,
} model_phase_t;

typedef struct {
    model_phase_t phase;
    bool cloze;               // expected front kind
    bool allow;               // too easy accepted (allow flag AND LEARN)
    vocab_ms_t front_deadline;
    bool timer_armed;         // VOCAB_TIMER_STAGE in the session's table
    vocab_ms_t timer_deadline;
    int outcomes;
    vocab_result_t result;    // the reported outcome
    uint8_t confirm_seq;
} model_t;

// What the random walk exercised (checked at the end so the fuzz cannot silently degrade).
static struct {
    unsigned pauses;
    unsigned deadline_flips;  // flips by time before the minimum's timer event
    unsigned locked_intents;  // intents delivered during a lock window
} fuzz_cover;

static uint32_t fuzz_next(vocab_rng_t *r, uint32_t bound)
{
    return vocab_rng_below(r, bound);
}

// Applies the timer commands of the last call to the model's session timer table and checks
// that the outbox only holds stage-emittable commands for this card.
static void fuzz_apply_out(model_t *m, vocab_ms_t now, const char *ctx)
{
    CHECK(!F.out.overflow, "%s: no outbox overflow", ctx);
    for (uint8_t i = 0; i < F.out.count; i++) {
        const vocab_cmd_t *c = &F.out.cmd[i];
        switch (c->kind) {
        case VOCAB_CMD_PLAY_WORD:
            CHECK(c->uid == F.entry->uid, "%s: clip of the card's word", ctx);
            break;
        case VOCAB_CMD_PLAY_CUE:
            CHECK(c->cue == VOCAB_CUE_TICK, "%s: only CUE_TICK alone", ctx);
            break;
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            CHECK(c->cue == VOCAB_CUE_FLIP && c->uid == F.entry->uid, "%s: flip audio", ctx);
            break;
        case VOCAB_CMD_TIMER_START:
            CHECK(c->timer == VOCAB_TIMER_STAGE, "%s: only the main stage timer", ctx);
            m->timer_armed = true;
            m->timer_deadline = now + c->value;
            break;
        case VOCAB_CMD_TIMER_CANCEL:
            CHECK(c->timer == VOCAB_TIMER_STAGE, "%s: cancel of the main stage timer", ctx);
            m->timer_armed = false;
            break;
        case VOCAB_CMD_NEW_EPOCH:
            break;
        default:
            CHECK(false, "%s: command kind %d is not stage-emittable", ctx, (int)c->kind);
            break;
        }
    }
}

static void fuzz_expect_view(const model_t *m, const char *ctx)
{
    const vocab_badge_t front_badge = vocab_view_card_badge(F.purpose, VOCAB_FORMAT_S2);
    const vocab_badge_t back_badge =
        F.purpose == VOCAB_PURPOSE_LEARN ? VOCAB_BADGE_SELF_RATE : front_badge;
    const face_t front = m->cloze ? FACE_CLOZE : FACE_WORD;
    switch (m->phase) {
    case M_FRONT_WAIT: expect_face(ctx, front, VOCAB_HINT_THINK, front_badge); break;
    case M_FRONT_READY: expect_face(ctx, front, VOCAB_HINT_FLIP, front_badge); break;
    case M_BACK:
    case M_CONFIRM: expect_face(ctx, FACE_BACK, VOCAB_HINT_REPLAY, back_badge); break;
    case M_FORGOT_ACK: expect_face(ctx, FACE_ACK, VOCAB_HINT_REPLAY, back_badge); break;
    case M_DONE: break;
    }
    if (m->phase != M_DONE) {
        CHECK(F.view.anim.confirm_seq[0] == m->confirm_seq, "%s: confirm flashes", ctx);
    }
}

static bool model_locked(const model_t *m)
{
    return m->phase == M_CONFIRM || m->phase == M_FORGOT_ACK;
}

// Expected reaction to one event (vocab_stage2.h behavior) - checks result and outbox.
static void fuzz_event(model_t *m, vocab_event_t ev, vocab_ms_t now, const char *ctx)
{
    const vocab_uid_t uid = F.entry->uid;
    send(ev, now);
    const model_phase_t before = m->phase;
    if ((ev.kind == VOCAB_EV_CLICK || ev.kind == VOCAB_EV_LONG) && model_locked(m)) {
        fuzz_cover.locked_intents++;
    }
    if (ev.kind == VOCAB_EV_CLICK) {
        const bool front_ready = before == M_FRONT_READY ||
                                 (before == M_FRONT_WAIT && vocab_ms_reached(now, m->front_deadline));
        if (front_ready) {
            m->phase = M_BACK;
            EXPECT_RUNNING(ctx, false);
            if (before == M_FRONT_WAIT) {
                fuzz_cover.deadline_flips++;
                EXPECT_OUT(ctx, C_CANCEL(VOCAB_TIMER_STAGE), C_CUE_WORD(VOCAB_CUE_FLIP, uid), C_EPOCH());
            } else {
                EXPECT_OUT(ctx, C_CUE_WORD(VOCAB_CUE_FLIP, uid), C_EPOCH());
            }
        } else if (before == M_BACK && ev.key == VOCAB_KEY_UP) {
            m->phase = M_CONFIRM;
            m->outcomes++;
            m->result = VOCAB_RESULT_KNOW;
            m->confirm_seq++;
            EXPECT_RES(ctx, VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_KNOW);
            EXPECT_OUT(ctx, C_CUE(VOCAB_CUE_TICK), C_EPOCH(),
                       C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_CONFIRM_MS));
        } else if (before == M_BACK && ev.key == VOCAB_KEY_DOWN) {
            m->phase = M_FORGOT_ACK;
            m->outcomes++;
            m->result = VOCAB_RESULT_FORGOT;
            EXPECT_RES(ctx, VOCAB_STAGE_RUNNING, true, true, VOCAB_RESULT_FORGOT);
            EXPECT_OUT(ctx, C_CUE(VOCAB_CUE_TICK), C_EPOCH(),
                       C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FORGOT_ACK_MS));
        } else if (before == M_BACK) {
            EXPECT_RUNNING(ctx, false);
            EXPECT_OUT(ctx, C_WORD(uid));
        } else {
            EXPECT_RUNNING(ctx, model_locked(m));
            EXPECT_NO_OUT(ctx);
        }
    } else if (ev.kind == VOCAB_EV_LONG) {
        if (ev.key == VOCAB_KEY_UP && m->allow && (before == M_FRONT_WAIT || before == M_FRONT_READY)) {
            m->phase = M_DONE;
            m->outcomes++;
            m->result = VOCAB_RESULT_TOO_EASY;
            EXPECT_RES(ctx, VOCAB_STAGE_DONE, false, true, VOCAB_RESULT_TOO_EASY);
        } else {
            EXPECT_RUNNING(ctx, model_locked(m));
        }
        EXPECT_NO_OUT(ctx);
    } else if (ev.kind == VOCAB_EV_TIMER && ev.timer == VOCAB_TIMER_STAGE &&
               before == M_FRONT_WAIT) {
        m->phase = M_FRONT_READY;
        EXPECT_RUNNING(ctx, false);
        EXPECT_NO_OUT(ctx);
    } else if (ev.kind == VOCAB_EV_TIMER && ev.timer == VOCAB_TIMER_STAGE && model_locked(m)) {
        m->phase = M_DONE;
        EXPECT_DONE(ctx);
        EXPECT_NO_OUT(ctx);
    } else {
        EXPECT_RUNNING(ctx, model_locked(m));
        EXPECT_NO_OUT(ctx);
    }
    fuzz_apply_out(m, now, ctx);
}

// The session's pause card: stage timers cancelled, view restored, resume() later.
static void fuzz_pause(model_t *m, vocab_ms_t *now, vocab_rng_t *r, const char *ctx)
{
    const vocab_view_t saved = F.view;
    fuzz_cover.pauses++;
    m->timer_armed = false;
    vocab_view_reset_card(&F.view);
    F.view.badge = VOCAB_BADGE_PAUSE;
    *now += fuzz_next(r, 4000u);
    F.view = saved;
    resume_at(*now);
    switch (m->phase) {
    case M_FRONT_WAIT:
        m->front_deadline = *now + VOCAB_S2_FRONT_MIN_MS;
        EXPECT_RUNNING(ctx, false);
        EXPECT_OUT(ctx, C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS));
        break;
    case M_CONFIRM:
        EXPECT_RUNNING(ctx, true);
        EXPECT_OUT(ctx, C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_CONFIRM_MS));
        break;
    case M_FORGOT_ACK:
        EXPECT_RUNNING(ctx, true);
        EXPECT_OUT(ctx, C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FORGOT_ACK_MS));
        break;
    case M_FRONT_READY:
    case M_BACK:
        EXPECT_RUNNING(ctx, false);
        EXPECT_NO_OUT(ctx);
        break;
    case M_DONE:
        EXPECT_DONE(ctx);
        break;
    }
    fuzz_apply_out(m, *now, ctx);
}

// Invariants after every call: one outcome at most; locked only after the outcome and
// always with a pending timer; the front minimum always has its timer; DONE has the outcome.
static void fuzz_invariants(const model_t *m, const char *ctx)
{
    CHECK(m->outcomes <= 1, "%s: outcome reported at most once", ctx);
    if (m->phase == M_DONE) {
        CHECK(m->outcomes == 1, "%s: a finished card reported its outcome", ctx);
        return;
    }
    if (model_locked(m)) {
        CHECK(m->outcomes == 1 && m->timer_armed, "%s: a lock always ends by its timer", ctx);
    } else {
        CHECK(m->outcomes == 0, "%s: no outcome before the rating", ctx);
    }
    if (m->phase == M_FRONT_WAIT) {
        CHECK(m->timer_armed, "%s: the front minimum always has its timer", ctx);
    }
    CHECK(F.rng.state == F.rng_state, "%s: no RNG draws", ctx);
}

#define FUZZ_CARDS 20000

static void test_fuzz(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    vocab_rng_t r;
    vocab_rng_seed(&r, 20261023u);
    const int failures_before = vocab_test_failures;
    unsigned outcomes[VOCAB_RESULT_TOO_EASY + 1] = { 0 };
    unsigned long events = 0;
    for (int card = 0; card < FUZZ_CARDS && vocab_test_failures == failures_before; card++) {
        const vocab_index_t index = (vocab_index_t)fuzz_next(&r, bank->count);
        const vocab_entry_t *e = vocab_bank_at(bank, index);
        const bool cloze = fuzz_next(&r, 2u) != 0;
        const bool allow = fuzz_next(&r, 4u) != 0;
        const vocab_purpose_t purpose =
            fuzz_next(&r, 5u) == 0 ? (vocab_purpose_t)(1u + fuzz_next(&r, 3u)) : VOCAB_PURPOSE_LEARN;
        vocab_ms_t now = fuzz_next(&r, 8u) == 0 ? UINT32_MAX - fuzz_next(&r, 3000u) : vocab_rng_next(&r);
        char ctx[96];
        snprintf(ctx, sizeof(ctx), "fuzz card %d (uid %u)", card, (unsigned)e->uid);

        begin_with(bank, index, purpose, cloze, allow, now);
        model_t m = { 0 };
        m.phase = M_FRONT_WAIT;
        m.cloze = cloze && vocab_entry_has_example_cloze(e);
        m.allow = allow && purpose == VOCAB_PURPOSE_LEARN;
        m.front_deadline = now + VOCAB_S2_FRONT_MIN_MS;
        m.confirm_seq = F.view.anim.confirm_seq[0];
        EXPECT_RUNNING(ctx, false);
        EXPECT_OUT(ctx, C_WORD(e->uid), C_TIMER(VOCAB_TIMER_STAGE, VOCAB_S2_FRONT_MIN_MS));
        fuzz_apply_out(&m, now, ctx);
        fuzz_expect_view(&m, ctx);

        const int steps = 1 + (int)fuzz_next(&r, 40u);
        for (int s = 0; s < steps && m.phase != M_DONE; s++) {
            const uint32_t action = fuzz_next(&r, 100u);
            if (action < 30u) {
                // Time passes; the session fires a due stage timer.
                now += fuzz_next(&r, 1600u);
                if (m.timer_armed && vocab_ms_reached(now, m.timer_deadline)) {
                    m.timer_armed = false;
                    fuzz_event(&m, vocab_event_timer(VOCAB_TIMER_STAGE), now, ctx);
                    events++;
                }
            } else if (action < 85u) {
                // An intent; the session drops intents while the stage is locked (mostly).
                if (model_locked(&m) && fuzz_next(&r, 6u) != 0) continue;
                now += fuzz_next(&r, 300u);
                // OK clicks dominate so that cards live long enough to see many states.
                const uint32_t k = fuzz_next(&r, 20u);
                vocab_event_t ev;
                if (k < 10u) {
                    ev = vocab_event_click(VOCAB_KEY_OK);
                } else if (k < 17u) {
                    ev = vocab_event_click(k < 14u ? VOCAB_KEY_UP : VOCAB_KEY_DOWN);
                } else if (k < 18u) {
                    ev = vocab_event_long(VOCAB_KEY_UP);
                } else {
                    ev = vocab_event_long(k == 18u ? VOCAB_KEY_DOWN : VOCAB_KEY_OK);
                }
                fuzz_event(&m, ev, now, ctx);
                events++;
            } else if (action < 90u) {
                fuzz_event(&m, vocab_event_timer(VOCAB_TIMER_STAGE_AUX), now, ctx);  // spurious
                events++;
            } else if (action < 93u) {
                // A stale main-timer event where none is pending (e.g. after a time-based flip).
                if (m.timer_armed || m.phase == M_FRONT_WAIT) continue;
                fuzz_event(&m, vocab_event_timer(VOCAB_TIMER_STAGE), now, ctx);
                events++;
            } else {
                if (model_locked(&m) && fuzz_next(&r, 4u) != 0) continue;
                fuzz_pause(&m, &now, &r, ctx);
                events++;
            }
            fuzz_invariants(&m, ctx);
            fuzz_expect_view(&m, ctx);
        }

        // Never stuck: wait for timers and press keys until the card ends.
        for (int guard = 0; guard < 8 && m.phase != M_DONE; guard++) {
            if (m.timer_armed) {
                now = m.timer_deadline + fuzz_next(&r, 50u);
                m.timer_armed = false;
                fuzz_event(&m, vocab_event_timer(VOCAB_TIMER_STAGE), now, ctx);
            } else if (model_locked(&m)) {
                CHECK(false, "%s: stuck in a lock without a timer", ctx);
                break;
            } else if (m.phase == M_BACK) {
                fuzz_event(&m, vocab_event_click(fuzz_next(&r, 2u) ? VOCAB_KEY_UP : VOCAB_KEY_DOWN),
                           now, ctx);
            } else {
                fuzz_event(&m, vocab_event_click(VOCAB_KEY_OK), now, ctx);
            }
            events++;
            fuzz_invariants(&m, ctx);
            fuzz_expect_view(&m, ctx);
        }
        CHECK(m.phase == M_DONE, "%s: the card finishes", ctx);
        CHECK(m.outcomes == 1, "%s: exactly one outcome", ctx);
        if ((unsigned)m.result < ARRAY_LEN(outcomes)) outcomes[m.result]++;
    }
    // The random walk must have covered every ending, pauses and time-based flips.
    CHECK(outcomes[VOCAB_RESULT_KNOW] > 500u && outcomes[VOCAB_RESULT_FORGOT] > 500u &&
              outcomes[VOCAB_RESULT_TOO_EASY] > 200u,
          "fuzz endings: know %u, forgot %u, too easy %u", outcomes[VOCAB_RESULT_KNOW],
          outcomes[VOCAB_RESULT_FORGOT], outcomes[VOCAB_RESULT_TOO_EASY]);
    CHECK(fuzz_cover.pauses > 1000u && fuzz_cover.deadline_flips > 100u &&
              fuzz_cover.locked_intents > 100u,
          "fuzz coverage: pauses %u, deadline flips %u, intents during locks %u",
          fuzz_cover.pauses, fuzz_cover.deadline_flips, fuzz_cover.locked_intents);
    CHECK(events > 100000u, "fuzz drove %lu events", events);
    printf("Stage 2 fuzz: %lu events, %d cards (know %u, forgot %u, too easy %u)\n", events, FUZZ_CARDS,
           outcomes[VOCAB_RESULT_KNOW], outcomes[VOCAB_RESULT_FORGOT],
           outcomes[VOCAB_RESULT_TOO_EASY]);
}

int main(void)
{
    test_word_front();
    test_cloze_front();
    test_cloze_front_guard();
    test_front_minimum();
    test_front_deadline_without_timer();
    test_back_know();
    test_back_forgot();
    test_back_replay_and_ignored();
    test_too_easy();
    test_inserted_purpose_badges();
    test_resume();
    test_unusable_input();
    test_every_entry();
    test_fuzz();
    return vocab_test_report("vocab Stage 2 tests");
}
