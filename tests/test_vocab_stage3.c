// tests/test_vocab_stage3.c - Stage 3 phonics cloze card (spec 7.3, 7.4, 8.3, 11.5, 11.6;
// AC-22 Stage 3 part, AC-24, AC-25 side balance and stable blank geometry, AC-27 for
// Stage 3 views, randomized event fuzz).
//
// Owner: Stage 3 developer. Compiled by tools/validate.sh with:
//   tests/test_vocab_stage3.c main/vocab_stage3.c main/vocab_cloze.c
//   main/vocab_bank.c main/vocab_rng.c main/vocab_view.c
//
// The rig below plays the session's part of the vocab_stage.h contract: it keeps the two
// stage timers as absolute one-shot deadlines (re-arm replaces), fires them when due, counts
// outcomes, checks every command, and verifies that the stage writes only its view region.
// In "session" mode intents are dropped while the last result was locked (spec 3.3 R5);
// scripted tests also send keys during locks directly to prove the stage ignores them.
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_cloze.h"
#include "vocab_config.h"
#include "vocab_stage3.h"
#include "vocab_strings.h"
#include "vocab_test.h"

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define T0 1000u

typedef struct {
    vocab_stage3_t st;
    vocab_view_t view;
    vocab_outbox_t out;          // commands of the last call
    vocab_rng_t rng;
    vocab_stage_input_t in;
    const vocab_entry_t *entry;
    vocab_cloze_t cloze;
    vocab_key_t right_key;       // key of the panel holding the right chunk
    vocab_ms_t now;
    vocab_stage_result_t res;    // last result
    bool armed[VOCAB_STAGE_TIMER_COUNT];
    vocab_ms_t deadline[VOCAB_STAGE_TIMER_COUNT];
    unsigned outcomes;
    vocab_result_t outcome;
    bool done;
    unsigned cmd_errors;
} rig_t;

static rig_t R;

static const vocab_entry_t *entry_by_uid(vocab_uid_t uid)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    vocab_index_t index = 0;
    return vocab_bank_find(bank, uid, &index) ? &bank->entries[index] : NULL;
}

static vocab_key_t other_key(vocab_key_t key)
{
    return key == VOCAB_KEY_UP ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
}

// Session-owned fields get sentinels so that any stage write to them is detected.
static void set_session_sentinels(vocab_view_t *v)
{
    v->screen = VOCAB_SCREEN_CARD;
    vocab_view_show(&v->status.progress, "3/7", VOCAB_RGB_DIM);
    v->status.volume = VOCAB_VOL_ICON_LOW;
    v->hint.toast = VOCAB_TOAST_VOL_LOW;
    vocab_text_copy(v->hint.text, sizeof(v->hint.text), "sentinel");
    v->hint.color = 0x123456u;
    v->anim.pulse_seq = 5;
    v->anim.pet_seq = 9;
    v->anim.confirm_seq[0] = 3;
    v->anim.confirm_seq[1] = 4;
    v->xpbar.value = 7;
}

static void check_session_region(const vocab_view_t *before, const char *where)
{
    const vocab_view_t *v = &R.view;
    const bool same =
        before->screen == v->screen &&
        memcmp(&before->status, &v->status, sizeof(v->status)) == 0 &&
        before->hint.toast == v->hint.toast &&
        memcmp(before->hint.text, v->hint.text, sizeof(v->hint.text)) == 0 &&
        before->hint.color == v->hint.color &&
        memcmp(&before->card.banner, &v->card.banner, sizeof(v->card.banner)) == 0 &&
        memcmp(&before->home, &v->home, sizeof(v->home)) == 0 &&
        memcmp(&before->summary, &v->summary, sizeof(v->summary)) == 0 &&
        memcmp(&before->pet, &v->pet, sizeof(v->pet)) == 0 &&
        memcmp(&before->xpbar, &v->xpbar, sizeof(v->xpbar)) == 0 &&
        before->anim.pulse_seq == v->anim.pulse_seq && before->anim.pet_seq == v->anim.pet_seq &&
        before->anim.confirm_seq[0] == v->anim.confirm_seq[0] &&
        before->anim.confirm_seq[1] == v->anim.confirm_seq[1];
    CHECK(same, "%s: the stage wrote a session-owned view field", where);
}

// Checks and applies the commands of the last call (vocab_stage.h rules 4, 6, 7).
static void apply_commands(bool epoch_allowed, const char *where)
{
    CHECK(!R.out.overflow, "%s: outbox overflow", where);
    for (uint8_t i = 0; i < R.out.count; i++) {
        const vocab_cmd_t *cmd = &R.out.cmd[i];
        switch (cmd->kind) {
        case VOCAB_CMD_PLAY_WORD:
            if (cmd->uid != R.entry->uid) R.cmd_errors++;
            break;
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            if (cmd->uid != R.entry->uid ||
                (cmd->cue != VOCAB_CUE_CHEER && cmd->cue != VOCAB_CUE_WRONG)) {
                R.cmd_errors++;
            }
            break;
        case VOCAB_CMD_TIMER_START:
            if (cmd->timer != VOCAB_TIMER_STAGE || cmd->value == 0u) {
                R.cmd_errors++;
            } else {
                R.armed[cmd->timer] = true;
                R.deadline[cmd->timer] = R.now + cmd->value;
            }
            break;
        case VOCAB_CMD_TIMER_CANCEL:
            if (!vocab_timer_is_stage(cmd->timer)) {
                R.cmd_errors++;
            } else {
                R.armed[cmd->timer] = false;
            }
            break;
        case VOCAB_CMD_NEW_EPOCH:
            if (!epoch_allowed) R.cmd_errors++;
            break;
        case VOCAB_CMD_STOP_AUDIO:
            break;
        default:
            R.cmd_errors++;  // PLAY_CUE alone and session-only commands are never used
            break;
        }
    }
    CHECK(R.cmd_errors == 0, "%s: unexpected command", where);
}

static void record(vocab_stage_result_t res, const char *where)
{
    R.res = res;
    if (res.outcome_ready) {
        R.outcomes++;
        R.outcome = res.result;
        CHECK(res.result == VOCAB_RESULT_CORRECT_FIRST || res.result == VOCAB_RESULT_CORRECT_SECOND ||
              res.result == VOCAB_RESULT_WRONG, "%s: outcome value %d", where, (int)res.result);
    }
    CHECK(R.outcomes <= 1u, "%s: outcome reported more than once", where);
    if (res.status == VOCAB_STAGE_DONE) {
        R.done = true;
        R.armed[0] = R.armed[1] = false;  // the session cancels the stage timers
    }
}

static vocab_stage_result_t rig_begin_ex(const vocab_entry_t *e, vocab_purpose_t purpose,
                                         uint32_t seed, vocab_ms_t t0, bool allow_too_easy)
{
    memset(&R, 0, sizeof(R));
    const vocab_bank_t *bank = vocab_bank_builtin();
    R.entry = e;
    vocab_cloze_from_entry(e, &R.cloze);
    vocab_rng_seed(&R.rng, seed);
    vocab_view_init(&R.view);
    set_session_sentinels(&R.view);
    vocab_view_reset_card(&R.view);
    vocab_outbox_clear(&R.out);
    R.now = t0;
    R.in.bank = bank;
    R.in.index = (vocab_index_t)(e - bank->entries);
    R.in.purpose = purpose;
    R.in.format = VOCAB_FORMAT_S3;
    R.in.mastery = purpose == VOCAB_PURPOSE_LEARN ? 3u : 4u;
    R.in.lure_index = VOCAB_INDEX_NONE;
    R.in.cloze_front = false;
    R.in.allow_too_easy = allow_too_easy;
    R.in.rng = &R.rng;
    vocab_view_t before;
    memcpy(&before, &R.view, sizeof(before));
    const vocab_stage_result_t res = vocab_stage3_begin(&R.st, &R.in, R.now, &R.view, &R.out);
    check_session_region(&before, "begin");
    apply_commands(false, "begin");
    record(res, "begin");
    R.right_key = strcmp(R.view.panel[0].text, R.cloze.right) == 0 ? VOCAB_KEY_UP : VOCAB_KEY_DOWN;
    return res;
}

static vocab_stage_result_t rig_begin(vocab_uid_t uid, vocab_purpose_t purpose, uint32_t seed)
{
    const vocab_entry_t *e = entry_by_uid(uid);
    CHECK(e != NULL, "uid %u in the bank", uid);
    return rig_begin_ex(e, purpose, seed, T0, false);
}

static vocab_stage_result_t rig_event(vocab_event_t ev)
{
    vocab_view_t before;
    memcpy(&before, &R.view, sizeof(before));
    vocab_outbox_clear(&R.out);
    const vocab_stage_result_t res = vocab_stage3_handle(&R.st, &ev, R.now, &R.view, &R.out);
    check_session_region(&before, "handle");
    apply_commands(true, "handle");
    record(res, "handle");
    return res;
}

static vocab_stage_result_t rig_click(vocab_key_t key)
{
    return rig_event(vocab_event_click(key));
}

// Fires due stage timers in deadline order (each arm fires once), like vocab_session_poll.
static void rig_fire_due(void)
{
    for (int guard = 0; guard < 8 && !R.done; guard++) {
        int best = -1;
        for (int i = 0; i < (int)VOCAB_STAGE_TIMER_COUNT; i++) {
            if (!R.armed[i] || !vocab_ms_reached(R.now, R.deadline[i])) continue;
            if (best < 0 || (int32_t)(R.deadline[i] - R.deadline[best]) < 0) best = i;
        }
        if (best < 0) return;
        R.armed[best] = false;
        rig_event(vocab_event_timer((vocab_timer_id_t)best));
    }
}

static void rig_wait(uint32_t ms)
{
    R.now += ms;
    rig_fire_due();
}

static bool same_view(const vocab_view_t *a, const vocab_view_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

// The visible skeleton as the renderer draws it: the blank shows nothing or `right`.
static void shown_skeleton(char *buf, size_t cap)
{
    const vocab_view_skeleton_t *sk = &R.view.card.skeleton;
    snprintf(buf, cap, "%s%s%s", sk->pre, sk->filled ? sk->right : "", sk->suf);
}

static bool has_token_ci(const char *s, const char *token)
{
    const size_t want = strlen(token);
    for (size_t i = 0; s[i] != '\0';) {
        while (s[i] != '\0' && !((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') && s[i] != '\'') i++;
        const size_t start = i;
        while (s[i] != '\0' && (((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') || s[i] == '\'')) i++;
        if (i - start == want && want > 0) {
            size_t k = 0;
            while (k < want && (s[start + k] | 0x20) == token[k]) k++;
            if (k == want) return true;
        }
    }
    return false;
}

// AC-27 for one Stage 3 view: only allowed English, never the wrong-substituted spelling.
static void check_view_safety(const char *where)
{
    const vocab_view_t *v = &R.view;
    const vocab_entry_t *e = R.entry;
    const vocab_cloze_t *c = &R.cloze;
    char pre[VOCAB_VIEW_PART_CAP];
    char suf[VOCAB_VIEW_PART_CAP];
    char bad_token[48];
    char bad_text[64];
    vocab_cloze_split(c, pre, sizeof(pre), suf, sizeof(suf));
    vocab_cloze_token(c, c->wrong, bad_token, sizeof(bad_token));
    snprintf(bad_text, sizeof(bad_text), "%s%s%s", pre, c->wrong, suf);

    CHECK(!v->card.word.visible && !v->card.ipa.visible && !v->card.meaning.visible &&
          !v->card.sentence.visible && !v->card.example.visible && !v->card.banner.visible,
          "%s: Stage 3 never shows the word, IPA, meaning, sentence or example", where);
    CHECK(v->card.frame && v->card.s3_meaning.visible &&
          strcmp(v->card.s3_meaning.text, e->zh) == 0 && v->card.s3_meaning.color == VOCAB_RGB_YELLOW,
          "%s: meaning on top", where);
    const vocab_view_skeleton_t *sk = &v->card.skeleton;
    CHECK(sk->visible && strcmp(sk->pre, pre) == 0 && strcmp(sk->suf, suf) == 0 &&
          strcmp(sk->right, c->right) == 0 && strcmp(sk->other, c->wrong) == 0,
          "%s: skeleton parts are fixed for the card", where);
    char shown[64];
    shown_skeleton(shown, sizeof(shown));
    if (sk->filled) {
        CHECK(strcmp(shown, e->text) == 0 && !sk->blink, "%s: filled blank = the correct word",
              where);
    }
    const char *prompt = v->card.prompt.text;
    CHECK(v->card.prompt.visible &&
          (strcmp(prompt, VOCAB_STR_S3_PROMPT) == 0 || strcmp(prompt, VOCAB_STR_S3_RETRY) == 0 ||
           strcmp(prompt, VOCAB_STR_S3_SUCCESS) == 0 || strcmp(prompt, VOCAB_STR_S3_REVEAL) == 0),
          "%s: Stage 3 prompt", where);
    unsigned visible = 0;
    for (int i = 0; i < 2; i++) {
        const vocab_view_panel_t *p = &v->panel[i];
        if (!p->visible) {
            CHECK(p->text[0] == '\0', "%s: hidden panel %d keeps no text", where, i);
            continue;
        }
        visible++;
        CHECK(p->text_en && (strcmp(p->text, c->right) == 0 || strcmp(p->text, c->wrong) == 0),
              "%s: panel %d holds an isolated chunk", where, i);
    }
    CHECK(visible >= 1u, "%s: the correct panel stays visible", where);
    if (visible == 2u) {
        CHECK(strcmp(v->panel[0].text, v->panel[1].text) != 0, "%s: two different chunks", where);
    }
    const char *fields[] = {
        v->card.word.text, v->card.ipa.text, v->card.meaning.text, v->card.sentence.text,
        v->card.example.text, v->card.s3_meaning.text, v->card.prompt.text, v->card.banner.text,
        v->panel[0].text, v->panel[1].text, sk->pre, sk->suf, shown, v->hint.text,
    };
    for (size_t i = 0; i < COUNT_OF(fields); i++) {
        CHECK(!strstr(fields[i], bad_text) && !has_token_ci(fields[i], bad_token),
              "%s: wrong spelling '%s' visible in field %u", where, bad_token, (unsigned)i);
    }
}

static bool cmd_is(int index, vocab_cmd_kind_t kind)
{
    return index < (int)R.out.count && R.out.cmd[index].kind == kind;
}

// ---------------------------------------------------------------------------------------
// begin()
// ---------------------------------------------------------------------------------------

static void test_begin_view(void)
{
    const vocab_entry_t *bus = entry_by_uid(301);
    vocab_rng_t ref;
    vocab_rng_seed(&ref, 31);
    const bool bit = vocab_rng_bit(&ref);
    const vocab_stage_result_t res = rig_begin(301, VOCAB_PURPOSE_LEARN, 31);
    CHECK(res.status == VOCAB_STAGE_RUNNING && !res.locked && !res.outcome_ready, "begin result");
    CHECK(R.out.count == 1u && cmd_is(0, VOCAB_CMD_PLAY_WORD) && R.out.cmd[0].uid == 301u,
          "begin requests only the word clip");
    CHECK(R.rng.state == ref.state, "begin draws exactly one RNG value");
    const vocab_side_t correct = bit ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP;
    const vocab_view_t *v = &R.view;
    CHECK(v->card.frame && v->card.s3_meaning.visible && strcmp(v->card.s3_meaning.text, bus->zh) == 0 &&
          v->card.s3_meaning.color == VOCAB_RGB_YELLOW, "meaning on top in yellow");
    const vocab_view_skeleton_t *sk = &v->card.skeleton;
    CHECK(sk->visible && strcmp(sk->pre, "b") == 0 && strcmp(sk->suf, "s") == 0 &&
          strcmp(sk->right, "u") == 0 && strcmp(sk->other, "a") == 0 && !sk->filled && sk->blink,
          "skeleton b _ s with an empty blinking blank");
    CHECK(v->card.prompt.visible && strcmp(v->card.prompt.text, VOCAB_STR_S3_PROMPT) == 0 &&
          v->card.prompt.color == VOCAB_RGB_DIM, "prompt");
    for (int i = 0; i < 2; i++) {
        CHECK(v->panel[i].visible && v->panel[i].style == VOCAB_PANEL_S3 && v->panel[i].text_en &&
              !v->panel[i].nudge_repeat, "panel %d: S3 style, English chunk", i);
    }
    CHECK(strcmp(v->panel[correct].text, "u") == 0 &&
          strcmp(v->panel[vocab_side_other(correct)].text, "a") == 0, "chunks on the RNG side");
    CHECK(v->badge == VOCAB_BADGE_SPELL && v->hint.kind == VOCAB_HINT_REPLAY, "badge and hint");
    CHECK(v->anim.shake_seq == 0 && v->anim.nudge_seq[0] == 0 && v->anim.nudge_seq[1] == 0,
          "no animation at begin");
    check_view_safety("begin");

    static const struct {
        vocab_purpose_t purpose;
        vocab_badge_t badge;
    } BADGES[] = {
        { VOCAB_PURPOSE_LEARN, VOCAB_BADGE_SPELL },
        { VOCAB_PURPOSE_SPOT_CHECK, VOCAB_BADGE_SPOT_CHECK },
        { VOCAB_PURPOSE_TOO_EASY_CHECK, VOCAB_BADGE_TOO_EASY },
        { VOCAB_PURPOSE_TEST_LATER, VOCAB_BADGE_TEST_AGAIN },
    };
    for (size_t i = 0; i < COUNT_OF(BADGES); i++) {
        rig_begin(301, BADGES[i].purpose, 7);
        CHECK(R.view.badge == BADGES[i].badge, "badge for purpose %d", (int)BADGES[i].purpose);
    }

    // Phrases keep their spaces (spec 4.7, 7.3).
    rig_begin(706, VOCAB_PURPOSE_LEARN, 3);
    CHECK(strcmp(R.view.card.skeleton.pre, "ice cr") == 0 &&
          strcmp(R.view.card.skeleton.suf, "m") == 0, "ice cr _ m");
    rig_begin(901, VOCAB_PURPOSE_LEARN, 3);
    CHECK(strcmp(R.view.card.skeleton.pre, "Good m") == 0 &&
          strcmp(R.view.card.skeleton.suf, "ning") == 0, "Good m _ ning");
}

static void test_unusable_input(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    const vocab_entry_t *bus = entry_by_uid(301);
    const vocab_entry_t *pear = entry_by_uid(103);
    vocab_stage3_t st;
    vocab_view_t view;
    vocab_view_t pristine;
    vocab_outbox_t out;
    vocab_rng_t rng;
    vocab_rng_seed(&rng, 1);
    vocab_view_init(&view);
    vocab_view_reset_card(&view);
    memcpy(&pristine, &view, sizeof(view));

    vocab_stage_input_t in = { bank, (vocab_index_t)(pear - bank->entries), VOCAB_PURPOSE_LEARN,
                               VOCAB_FORMAT_S3, 3, VOCAB_INDEX_NONE, false, false, &rng };
    // No cloze data (the session should have chosen Stage 2): DONE, no outcome, no view.
    vocab_outbox_clear(&out);
    vocab_stage_result_t res = vocab_stage3_begin(&st, &in, T0, &view, &out);
    CHECK(res.status == VOCAB_STAGE_DONE && !res.outcome_ready && out.count == 0 &&
          same_view(&view, &pristine), "no cloze data -> DONE untouched");
    const vocab_event_t up = vocab_event_click(VOCAB_KEY_UP);
    res = vocab_stage3_handle(&st, &up, T0, &view, &out);
    CHECK(res.status == VOCAB_STAGE_DONE && out.count == 0, "handle after a refused begin");
    res = vocab_stage3_resume(&st, T0, &view, &out);
    CHECK(res.status == VOCAB_STAGE_DONE && out.count == 0, "resume after a refused begin");

    in.index = (vocab_index_t)(bus - bank->entries);
    in.rng = NULL;
    res = vocab_stage3_begin(&st, &in, T0, &view, &out);
    CHECK(res.status == VOCAB_STAGE_DONE && out.count == 0 && same_view(&view, &pristine),
          "no RNG -> DONE");
    in.rng = &rng;
    in.index = bank->count;
    res = vocab_stage3_begin(&st, &in, T0, &view, &out);
    CHECK(res.status == VOCAB_STAGE_DONE && out.count == 0, "index out of range -> DONE");
    CHECK(vocab_stage3_begin(&st, NULL, T0, &view, &out).status == VOCAB_STAGE_DONE, "NULL input");

    // Inconsistent cloze data in a custom bank (texts and meanings borrowed from the bank).
    vocab_entry_t bad[3];
    for (int i = 0; i < 3; i++) bad[i] = *bus;
    bad[0].cloze_right = "a";          // text has "u" at pos 1
    bad[1].cloze_wrong = "u";          // equal chunks
    bad[2].cloze_wrong = "ae";         // unequal lengths
    const vocab_bank_t custom = { bad, 3 };
    for (vocab_index_t i = 0; i < 3; i++) {
        vocab_stage_input_t ci = in;
        ci.bank = &custom;
        ci.index = i;
        vocab_outbox_clear(&out);
        res = vocab_stage3_begin(&st, &ci, T0, &view, &out);
        CHECK(res.status == VOCAB_STAGE_DONE && out.count == 0 && same_view(&view, &pristine),
              "inconsistent cloze %u -> DONE", (unsigned)i);
    }
}

// AC-25: option sides about 50/50 over seeds; one RNG draw per card.
static void test_side_balance(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned up = 0;
    const unsigned seeds = 10000u;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
        rig_begin(301, VOCAB_PURPOSE_LEARN, seed);
        if (R.right_key == VOCAB_KEY_UP) up++;
    }
    CHECK(up >= seeds * 45u / 100u && up <= seeds * 55u / 100u, "bus: right chunk on UP in "
          "%u of %u cards", up, seeds);
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        unsigned n_up = 0;
        for (uint32_t seed = 1; seed <= 2000u; seed++) {
            rig_begin_ex(e, VOCAB_PURPOSE_LEARN, seed * 7919u, T0, false);
            if (R.right_key == VOCAB_KEY_UP) n_up++;
            const vocab_side_t right_side = R.right_key == VOCAB_KEY_UP ? VOCAB_SIDE_UP : VOCAB_SIDE_DOWN;
            if (strcmp(R.view.panel[right_side].text, e->cloze_right) != 0 ||
                strcmp(R.view.panel[vocab_side_other(right_side)].text, e->cloze_wrong) != 0) {
                CHECK(false, "uid %u seed %u: panels", e->uid, (unsigned)seed);
                break;
            }
        }
        CHECK(n_up >= 900u && n_up <= 1100u, "uid %u: UP in %u of 2000", e->uid, n_up);
    }
}

// ---------------------------------------------------------------------------------------
// AC-24: LEARN and check-card sequences with exact timer boundaries
// ---------------------------------------------------------------------------------------

static void test_learn_first_try(void)
{
    rig_begin(301, VOCAB_PURPOSE_LEARN, 31);
    rig_wait(300);
    const uint8_t shake = R.view.anim.shake_seq;
    const vocab_side_t right_side = R.right_key == VOCAB_KEY_UP ? VOCAB_SIDE_UP : VOCAB_SIDE_DOWN;
    const vocab_stage_result_t res = rig_click(R.right_key);
    CHECK(res.status == VOCAB_STAGE_RUNNING && res.locked && res.outcome_ready &&
          res.result == VOCAB_RESULT_CORRECT_FIRST, "O7: first-try correct");
    CHECK(R.out.count == 3u && cmd_is(0, VOCAB_CMD_PLAY_CUE_THEN_WORD) &&
          R.out.cmd[0].cue == VOCAB_CUE_CHEER && cmd_is(1, VOCAB_CMD_NEW_EPOCH) &&
          cmd_is(2, VOCAB_CMD_TIMER_START) && R.out.cmd[2].timer == VOCAB_TIMER_STAGE &&
          R.out.cmd[2].value == VOCAB_SUCCESS_HOLD_MS, "cheer + word, epoch, 1200 ms hold");
    const vocab_view_t *v = &R.view;
    CHECK(v->card.skeleton.filled && !v->card.skeleton.blink, "blank shows the right chunk");
    CHECK(v->panel[right_side].style == VOCAB_PANEL_CORRECT && v->panel[right_side].visible,
          "chosen panel correct style");
    CHECK(v->panel[vocab_side_other(right_side)].visible &&
          v->panel[vocab_side_other(right_side)].style == VOCAB_PANEL_S3, "other panel unchanged");
    CHECK(strcmp(v->card.prompt.text, VOCAB_STR_S3_SUCCESS) == 0 &&
          v->card.prompt.color == VOCAB_RGB_SUCCESS, "success prompt");
    CHECK(v->badge == VOCAB_BADGE_SPELL && v->hint.kind == VOCAB_HINT_REPLAY &&
          v->anim.shake_seq == shake, "badge/hint unchanged, no shake");
    check_view_safety("success");

    // Every intent during the hold is ignored, even when delivered directly.
    vocab_view_t snap;
    memcpy(&snap, &R.view, sizeof(snap));
    static const vocab_key_t KEYS[] = { VOCAB_KEY_UP, VOCAB_KEY_DOWN, VOCAB_KEY_OK };
    for (size_t i = 0; i < COUNT_OF(KEYS); i++) {
        const vocab_stage_result_t r = rig_click(KEYS[i]);
        CHECK(r.locked && !r.outcome_ready && r.status == VOCAB_STAGE_RUNNING && R.out.count == 0 &&
              same_view(&snap, &R.view), "key %d ignored in the success hold", (int)KEYS[i]);
    }
    rig_event(vocab_event_long(VOCAB_KEY_UP));
    CHECK(R.out.count == 0 && same_view(&snap, &R.view), "UP LONG ignored in the success hold");
    rig_wait(VOCAB_SUCCESS_HOLD_MS - 1u);
    CHECK(!R.done && R.res.locked, "still holding at 1199 ms");
    rig_wait(1);
    CHECK(R.done && R.outcomes == 1u && R.outcome == VOCAB_RESULT_CORRECT_FIRST,
          "DONE at 1200 ms, one outcome");
    CHECK(vocab_stage3_handle(&R.st, &(vocab_event_t){ VOCAB_EV_CLICK, VOCAB_KEY_UP,
                                                       VOCAB_TIMER_STAGE },
                              R.now, &R.view, &R.out).status == VOCAB_STAGE_DONE,
          "calls after DONE return DONE");
}

static void test_learn_retry(bool second_correct)
{
    rig_begin(203, VOCAB_PURPOSE_LEARN, 5);  // duck: d _ ck, u / a
    rig_wait(300);
    const vocab_side_t right_side = R.right_key == VOCAB_KEY_UP ? VOCAB_SIDE_UP : VOCAB_SIDE_DOWN;
    vocab_view_panel_t panels[2];
    memcpy(panels, R.view.panel, sizeof(panels));
    const uint8_t shake = R.view.anim.shake_seq;
    vocab_stage_result_t res = rig_click(other_key(R.right_key));
    CHECK(res.status == VOCAB_STAGE_RUNNING && res.locked && !res.outcome_ready,
          "miss 1 of 2: no outcome yet, locked");
    CHECK(R.out.count == 3u && cmd_is(0, VOCAB_CMD_PLAY_CUE_THEN_WORD) &&
          R.out.cmd[0].cue == VOCAB_CUE_WRONG && cmd_is(1, VOCAB_CMD_NEW_EPOCH) &&
          cmd_is(2, VOCAB_CMD_TIMER_START) && R.out.cmd[2].value == VOCAB_S3_RETRY_LOCK_MS,
          "wrong cue + word, epoch, 600 ms retry lock");
    const vocab_view_t *v = &R.view;
    CHECK(v->anim.shake_seq == (uint8_t)(shake + 1u), "the blank shakes once");
    CHECK(!v->card.skeleton.filled && !v->card.skeleton.blink, "blank stays empty");
    CHECK(strcmp(v->card.prompt.text, VOCAB_STR_S3_RETRY) == 0 &&
          v->card.prompt.color == VOCAB_RGB_WARN, "retry prompt in orange");
    CHECK(memcmp(panels, v->panel, sizeof(panels)) == 0, "both options stay unchanged");
    CHECK(v->badge == VOCAB_BADGE_SPELL && v->hint.kind == VOCAB_HINT_REPLAY, "badge/hint kept");
    check_view_safety("retry lock");

    // The correct key at 599 ms is still ignored.
    rig_wait(VOCAB_S3_RETRY_LOCK_MS - 1u);
    vocab_view_t snap;
    memcpy(&snap, &R.view, sizeof(snap));
    res = rig_click(R.right_key);
    CHECK(res.locked && !res.outcome_ready && R.out.count == 0 && same_view(&snap, &R.view),
          "correct key ignored at 599 ms");
    rig_wait(1);
    CHECK(!R.res.locked && !R.done && R.outcomes == 0u, "retry lock ends at 600 ms");
    CHECK(R.out.count == 1u && cmd_is(0, VOCAB_CMD_NEW_EPOCH), "lock end is an epoch");
    CHECK(v->card.skeleton.blink && !v->card.skeleton.filled &&
          strcmp(v->card.prompt.text, VOCAB_STR_S3_RETRY) == 0 &&
          memcmp(panels, v->panel, sizeof(panels)) == 0, "second attempt: blank blinks again");
    check_view_safety("retry attempt");
    rig_click(VOCAB_KEY_OK);
    CHECK(R.out.count == 1u && cmd_is(0, VOCAB_CMD_PLAY_WORD), "OK replays during the retry");

    if (second_correct) {
        res = rig_click(R.right_key);
        CHECK(res.outcome_ready && res.result == VOCAB_RESULT_CORRECT_SECOND && res.locked,
              "O8: correct on the second press");
        CHECK(v->card.skeleton.filled && v->panel[right_side].style == VOCAB_PANEL_CORRECT &&
              strcmp(v->card.prompt.text, VOCAB_STR_S3_SUCCESS) == 0, "success view");
        rig_wait(VOCAB_SUCCESS_HOLD_MS);
        CHECK(R.done && R.outcomes == 1u, "DONE after the success hold");
        return;
    }
    res = rig_click(other_key(R.right_key));
    CHECK(res.outcome_ready && res.result == VOCAB_RESULT_WRONG && res.locked,
          "O9: second miss reveals with outcome WRONG");
    CHECK(R.out.count == 3u && cmd_is(0, VOCAB_CMD_PLAY_CUE_THEN_WORD) &&
          R.out.cmd[0].cue == VOCAB_CUE_WRONG && cmd_is(1, VOCAB_CMD_NEW_EPOCH) &&
          cmd_is(2, VOCAB_CMD_TIMER_START) && R.out.cmd[2].value == VOCAB_ERROR_HOLD_MS,
          "reveal: wrong cue + word, epoch, 1500 ms hold");
}

// Reveal hold + wait part (generic error hold, spec 5.3 steps 3-4).
static void check_reveal_and_wait(const char *where)
{
    const vocab_side_t right_side = R.right_key == VOCAB_KEY_UP ? VOCAB_SIDE_UP : VOCAB_SIDE_DOWN;
    const vocab_view_t *v = &R.view;
    CHECK(!v->panel[vocab_side_other(right_side)].visible, "%s: wrong panel hidden", where);
    CHECK(v->panel[right_side].visible && v->panel[right_side].style == VOCAB_PANEL_CORRECT &&
          strcmp(v->panel[right_side].text, R.cloze.right) == 0 && !v->panel[right_side].nudge_repeat,
          "%s: correct panel restyled", where);
    CHECK(v->card.skeleton.filled && !v->card.skeleton.blink, "%s: full correct word", where);
    CHECK(v->badge == VOCAB_BADGE_REMEMBER && strcmp(v->card.prompt.text, VOCAB_STR_S3_REVEAL) == 0 &&
          v->card.prompt.color == VOCAB_RGB_YELLOW && v->hint.kind == VOCAB_HINT_NONE,
          "%s: badge, prompt, empty hint", where);
    check_view_safety(where);

    const vocab_ms_t start = R.now;
    rig_wait(VOCAB_ERROR_HOLD_MS - 1u);
    vocab_view_t snap;
    memcpy(&snap, &R.view, sizeof(snap));
    vocab_stage_result_t res = rig_click(R.right_key);
    CHECK(res.locked && R.out.count == 0 && same_view(&snap, &R.view) && !R.done,
          "%s: correct key ignored at 1499 ms", where);
    rig_wait(1);
    CHECK(R.now - start == VOCAB_ERROR_HOLD_MS && !R.res.locked && R.out.count == 1u &&
          cmd_is(0, VOCAB_CMD_NEW_EPOCH), "%s: lock ends at 1500 ms with an epoch", where);
    CHECK(v->hint.kind == (right_side == VOCAB_SIDE_UP ? VOCAB_HINT_PRESS_UP : VOCAB_HINT_PRESS_DOWN) &&
          v->panel[right_side].nudge_repeat, "%s: wait part hint and nudging", where);
    check_view_safety(where);

    const uint8_t nudge = v->anim.nudge_seq[right_side];
    memcpy(&snap, &R.view, sizeof(snap));
    res = rig_click(other_key(R.right_key));
    CHECK(!res.locked && !res.outcome_ready && R.out.count == 0 && !R.done &&
          v->anim.nudge_seq[right_side] == (uint8_t)(nudge + 1u), "%s: other key nudges", where);
    snap.anim.nudge_seq[right_side] = v->anim.nudge_seq[right_side];
    CHECK(same_view(&snap, &R.view), "%s: the nudge changes nothing else", where);
    rig_click(VOCAB_KEY_OK);
    CHECK(R.out.count == 1u && cmd_is(0, VOCAB_CMD_PLAY_WORD) && !R.done, "%s: OK replays", where);
    rig_event(vocab_event_long(VOCAB_KEY_UP));
    CHECK(R.out.count == 0 && !R.done, "%s: UP LONG ignored", where);
    rig_wait(5000);
    CHECK(!R.done && !R.res.locked, "%s: waits for the correct key (no timeout)", where);
    res = rig_click(R.right_key);
    CHECK(res.status == VOCAB_STAGE_DONE && !res.outcome_ready && R.outcomes == 1u,
          "%s: correct key finishes without a second outcome", where);
}

static void test_learn_sequences(void)
{
    test_learn_retry(true);
    test_learn_retry(false);
    check_reveal_and_wait("LEARN reveal");
    CHECK(R.outcome == VOCAB_RESULT_WRONG, "LEARN two misses -> WRONG");
}

// Check cards: the first press decides (spec 7.4, O10/O11/O13/O14).
static void test_check_cards(void)
{
    static const vocab_purpose_t CHECKS[] = { VOCAB_PURPOSE_SPOT_CHECK, VOCAB_PURPOSE_TOO_EASY_CHECK,
                                              VOCAB_PURPOSE_TEST_LATER };
    for (size_t i = 0; i < COUNT_OF(CHECKS); i++) {
        rig_begin(705, CHECKS[i], (uint32_t)(11u + i));  // juice
        const uint8_t shake = R.view.anim.shake_seq;
        vocab_stage_result_t res = rig_click(other_key(R.right_key));
        CHECK(res.outcome_ready && res.result == VOCAB_RESULT_WRONG && res.locked &&
              R.view.anim.shake_seq == shake, "check %d: first miss reveals at once (no retry)",
              (int)CHECKS[i]);
        check_reveal_and_wait("check reveal");

        rig_begin(705, CHECKS[i], (uint32_t)(21u + i));
        res = rig_click(R.right_key);
        CHECK(res.outcome_ready && res.result == VOCAB_RESULT_CORRECT_FIRST && res.locked,
              "check %d: first press correct", (int)CHECKS[i]);
        rig_wait(VOCAB_SUCCESS_HOLD_MS);
        CHECK(R.done && R.outcomes == 1u, "check %d: done after the success hold", (int)CHECKS[i]);
    }
}

// ---------------------------------------------------------------------------------------
// Replay, ignored events, resume
// ---------------------------------------------------------------------------------------

static void test_ignored_events(void)
{
    // UP LONG is never "too easy" in Stage 3 (AC-22), even when the session would allow it.
    rig_begin_ex(entry_by_uid(402), VOCAB_PURPOSE_LEARN, 99, T0, true);
    vocab_view_t snap;
    memcpy(&snap, &R.view, sizeof(snap));
    vocab_stage_result_t res = rig_event(vocab_event_long(VOCAB_KEY_UP));
    CHECK(!res.outcome_ready && res.status == VOCAB_STAGE_RUNNING && R.out.count == 0 &&
          same_view(&snap, &R.view), "UP LONG ignored in the question");
    static const vocab_event_t IGNORED[] = {
        { VOCAB_EV_LONG, VOCAB_KEY_DOWN, VOCAB_TIMER_STAGE },
        { VOCAB_EV_LONG, VOCAB_KEY_OK, VOCAB_TIMER_STAGE },
        { VOCAB_EV_TIMER, VOCAB_KEY_OK, VOCAB_TIMER_STAGE },      // stray: no lock running
        { VOCAB_EV_TIMER, VOCAB_KEY_OK, VOCAB_TIMER_STAGE_AUX },
        { VOCAB_EV_TIMER, VOCAB_KEY_OK, VOCAB_TIMER_SESSION },
        { VOCAB_EV_NONE, VOCAB_KEY_UP, VOCAB_TIMER_STAGE },
        { VOCAB_EV_CLICK, (vocab_key_t)7, VOCAB_TIMER_STAGE },
    };
    for (size_t i = 0; i < COUNT_OF(IGNORED); i++) {
        res = rig_event(IGNORED[i]);
        CHECK(!res.outcome_ready && !res.locked && R.out.count == 0 && same_view(&snap, &R.view),
              "event %u ignored in the question", (unsigned)i);
    }
    // OK replays the whole word and changes nothing else.
    res = rig_click(VOCAB_KEY_OK);
    CHECK(!res.outcome_ready && R.out.count == 1u && cmd_is(0, VOCAB_CMD_PLAY_WORD) &&
          R.out.cmd[0].uid == 402u && same_view(&snap, &R.view), "OK replays");

    // A stage AUX timer during a lock does not end it.
    rig_click(R.right_key);
    res = rig_event(vocab_event_timer(VOCAB_TIMER_STAGE_AUX));
    CHECK(res.locked && !R.done, "AUX timer does not end the success hold");
    rig_wait(VOCAB_SUCCESS_HOLD_MS);
    CHECK(R.done, "the stage timer does");

    // Retry lock and reveal wait ignore UP LONG and AUX timers too.
    rig_begin(402, VOCAB_PURPOSE_LEARN, 98);
    rig_click(other_key(R.right_key));
    memcpy(&snap, &R.view, sizeof(snap));
    res = rig_event(vocab_event_long(VOCAB_KEY_UP));
    CHECK(res.locked && R.out.count == 0 && same_view(&snap, &R.view), "UP LONG in the retry lock");
    rig_wait(VOCAB_S3_RETRY_LOCK_MS);
    memcpy(&snap, &R.view, sizeof(snap));
    res = rig_event(vocab_event_long(VOCAB_KEY_UP));
    CHECK(!res.locked && R.out.count == 0 && same_view(&snap, &R.view), "UP LONG in the retry");
}

static void test_resume(void)
{
    // Pause/resume emulation (session): stage region snapshot, timers cancelled, restore.
    rig_begin(604, VOCAB_PURPOSE_LEARN, 42);
    vocab_view_t snap;
    memcpy(&snap, &R.view, sizeof(snap));
    vocab_outbox_clear(&R.out);
    vocab_stage_result_t res = vocab_stage3_resume(&R.st, R.now + 5000u, &R.view, &R.out);
    CHECK(res.status == VOCAB_STAGE_RUNNING && !res.locked && !res.outcome_ready &&
          R.out.count == 0 && same_view(&snap, &R.view), "resume in the question: nothing to do");
    rig_click(other_key(R.right_key));
    rig_wait(VOCAB_S3_RETRY_LOCK_MS);
    vocab_outbox_clear(&R.out);
    res = vocab_stage3_resume(&R.st, R.now, &R.view, &R.out);
    CHECK(!res.locked && R.out.count == 0, "resume in the retry attempt");
    rig_click(other_key(R.right_key));  // reveal
    rig_wait(VOCAB_ERROR_HOLD_MS);
    vocab_outbox_clear(&R.out);
    res = vocab_stage3_resume(&R.st, R.now, &R.view, &R.out);
    CHECK(!res.locked && R.out.count == 0 && R.outcomes == 1u, "resume in the wait part");
    rig_click(R.right_key);
    CHECK(R.done, "finished after resume");
    vocab_outbox_clear(&R.out);
    CHECK(vocab_stage3_resume(&R.st, R.now, &R.view, &R.out).status == VOCAB_STAGE_DONE &&
          R.out.count == 0, "resume after DONE");

    // Defensive: a resume inside a lock re-arms the remaining time, so it can never stick.
    rig_begin(604, VOCAB_PURPOSE_LEARN, 43);
    rig_click(other_key(R.right_key));       // retry lock, 600 ms from now
    const vocab_ms_t start = R.now;
    R.armed[0] = false;                      // the session cancelled the timers
    vocab_outbox_clear(&R.out);
    res = vocab_stage3_resume(&R.st, start + 200u, &R.view, &R.out);
    CHECK(res.locked && R.out.count == 1u && cmd_is(0, VOCAB_CMD_TIMER_START) &&
          R.out.cmd[0].timer == VOCAB_TIMER_STAGE && R.out.cmd[0].value == 400u,
          "resume in a lock re-arms the remaining 400 ms");
    vocab_outbox_clear(&R.out);
    res = vocab_stage3_resume(&R.st, start + 900u, &R.view, &R.out);
    CHECK(res.locked && R.out.count == 1u && R.out.cmd[0].value == 1u,
          "an expired lock re-arms 1 ms");
    R.now = start + 900u;
    apply_commands(false, "resume");
    rig_wait(1);
    CHECK(!R.res.locked && !R.done, "the lock ends after the re-armed timer");
}

// AC-25/AC-26 from the view: the skeleton strings never change during a card, so the row
// geometry (and the blank width) is the same in every state and independent of the side.
static int32_t stub_measure(void *ctx, vocab_word_size_t size, const char *text)
{
    (void)ctx;
    int32_t w = 0;
    for (const char *p = text; *p; p++) w += (*p == 'i' || *p == 'l') ? 6 : (*p == 'm' || *p == 'w') ? 22 : 13;
    return size == VOCAB_WORD_SIZE_L ? w * 9 / 7 : w;
}

static vocab_cloze_row_t view_row(void)
{
    const vocab_view_skeleton_t *sk = &R.view.card.skeleton;
    return vocab_cloze_row_layout(sk->pre, sk->right, sk->other, sk->suf, VOCAB_WORD_SIZE_M, 120,
                                  stub_measure, NULL);
}

static void test_row_never_reflows(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        rig_begin_ex(e, VOCAB_PURPOSE_LEARN, 1, T0, false);
        const vocab_cloze_row_t row = view_row();
        const int32_t wr = stub_measure(NULL, VOCAB_WORD_SIZE_M, e->cloze_right);
        const int32_t ww = stub_measure(NULL, VOCAB_WORD_SIZE_M, e->cloze_wrong);
        CHECK(row.box_w == (wr > ww ? wr : ww) + VOCAB_BLANK_PAD_PX, "uid %u: blank width", e->uid);
        const vocab_key_t first_side = R.right_key;
        bool other_side_seen = false;
        for (uint32_t seed = 2; seed < 40u && !other_side_seen; seed++) {
            rig_begin_ex(e, VOCAB_PURPOSE_LEARN, seed, T0, false);
            if (R.right_key == first_side) continue;
            other_side_seen = true;
            const vocab_cloze_row_t other = view_row();
            CHECK(memcmp(&row, &other, sizeof(row)) == 0, "uid %u: same row on either side", e->uid);
        }
        CHECK(other_side_seen, "uid %u: both sides occur", e->uid);
        rig_click(other_key(R.right_key));                 // retry
        const vocab_cloze_row_t retry = view_row();
        rig_wait(VOCAB_S3_RETRY_LOCK_MS);
        rig_click(other_key(R.right_key));                 // reveal
        const vocab_cloze_row_t reveal = view_row();
        rig_begin_ex(e, VOCAB_PURPOSE_LEARN, 1, T0, false);
        rig_click(R.right_key);                            // success
        const vocab_cloze_row_t success = view_row();
        CHECK(memcmp(&row, &retry, sizeof(row)) == 0 && memcmp(&row, &reveal, sizeof(row)) == 0 &&
              memcmp(&row, &success, sizeof(row)) == 0, "uid %u: the row never re-flows", e->uid);
    }
}

// ---------------------------------------------------------------------------------------
// Scripted runs over the whole bank and a randomized fuzz (AC-24, AC-27, robustness)
// ---------------------------------------------------------------------------------------

static void test_all_entries_scripts(void)
{
    static const vocab_purpose_t PURPOSES[] = { VOCAB_PURPOSE_LEARN, VOCAB_PURPOSE_SPOT_CHECK,
                                                VOCAB_PURPOSE_TOO_EASY_CHECK };
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned cards = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        for (size_t p = 0; p < COUNT_OF(PURPOSES); p++) {
            const bool learn = PURPOSES[p] == VOCAB_PURPOSE_LEARN;
            for (int script = 0; script < 3; script++) {
                rig_begin_ex(e, PURPOSES[p], (uint32_t)(1000u + i * 7u + (unsigned)script), T0, false);
                check_view_safety("script begin");
                if (script == 0) {
                    rig_click(R.right_key);
                } else {
                    rig_click(other_key(R.right_key));
                    check_view_safety("script miss");
                    if (learn) {
                        rig_wait(VOCAB_S3_RETRY_LOCK_MS);
                        rig_click(script == 1 ? R.right_key : other_key(R.right_key));
                    }
                }
                check_view_safety("script answer");
                const vocab_result_t want =
                    script == 0 ? VOCAB_RESULT_CORRECT_FIRST
                    : (learn && script == 1) ? VOCAB_RESULT_CORRECT_SECOND : VOCAB_RESULT_WRONG;
                CHECK(R.outcomes == 1u && R.outcome == want, "uid %u purpose %d script %d: result",
                      e->uid, (int)PURPOSES[p], script);
                for (int k = 0; k < 4 && !R.done; k++) {
                    rig_wait(VOCAB_ERROR_HOLD_MS);
                    check_view_safety("script hold");
                    if (!R.done && !R.res.locked) rig_click(R.right_key);
                }
                CHECK(R.done && R.outcomes == 1u, "uid %u script %d finishes", e->uid, script);
                cards++;
            }
        }
    }
    CHECK(cards == 34u * 3u * 3u, "scripted cards: %u", cards);
}

static void test_fuzz(void)
{
    static const vocab_purpose_t PURPOSES[] = { VOCAB_PURPOSE_LEARN, VOCAB_PURPOSE_SPOT_CHECK,
                                                VOCAB_PURPOSE_TOO_EASY_CHECK,
                                                VOCAB_PURPOSE_TEST_LATER };
    const vocab_bank_t *bank = vocab_bank_builtin();
    unsigned cards = 0;
    unsigned events = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        for (size_t p = 0; p < COUNT_OF(PURPOSES); p++) {
            for (uint32_t seed = 1; seed <= 100u; seed++) {
                vocab_rng_t fz;
                vocab_rng_seed(&fz, seed * 104729u + i * 131u + (uint32_t)p);
                const bool session_mode = vocab_rng_below(&fz, 4) != 0u;  // 3 in 4 runs
                // Some runs start right before the 32-bit millisecond wrap.
                const vocab_ms_t t0 = vocab_rng_bit(&fz) ? 0xFFFFF000u : T0;
                rig_begin_ex(e, PURPOSES[p], seed, t0, vocab_rng_bit(&fz));
                const uint64_t rng_after_begin = R.rng.state;
                const uint8_t max_misses = vocab_stage_s3_max_misses(PURPOSES[p]);
                unsigned answers = 0;        // UP/DOWN clicks while the question was open
                unsigned shakes = 0;
                bool was_filled = false;
                for (int step = 0; step < 50 && !R.done; step++) {
                    const bool question = !R.res.locked && !R.view.card.skeleton.filled;
                    const uint8_t shake_before = R.view.anim.shake_seq;
                    const unsigned outcomes_before = R.outcomes;
                    const uint32_t action = vocab_rng_below(&fz, 10);
                    vocab_key_t key = VOCAB_KEY_OK;
                    bool answered = false;
                    if (action <= 4u) {
                        key = (vocab_key_t)vocab_rng_below(&fz, 3);
                        if (session_mode && R.res.locked) continue;  // R5: dropped by the session
                        answered = question && key != VOCAB_KEY_OK;
                        rig_click(key);
                    } else if (action == 5u) {
                        if (session_mode && R.res.locked) continue;
                        rig_event(vocab_event_long(VOCAB_KEY_UP));
                    } else if (action <= 7u) {
                        rig_wait(vocab_rng_below(&fz, 8) == 0u ? vocab_rng_below(&fz, 6000) : vocab_rng_below(&fz, 700));
                    } else if (action == 8u && !session_mode) {
                        rig_event(vocab_event_timer(vocab_rng_bit(&fz) ? VOCAB_TIMER_STAGE
                                                                       : VOCAB_TIMER_STAGE_AUX));
                    } else if (!R.res.locked) {
                        // Pause card: stage region saved, timers cancelled, restored, resume.
                        vocab_view_t saved;
                        memcpy(&saved, &R.view, sizeof(saved));
                        vocab_view_reset_card(&R.view);
                        memcpy(&R.view, &saved, sizeof(saved));
                        R.armed[0] = R.armed[1] = false;
                        vocab_outbox_clear(&R.out);
                        R.now += vocab_rng_below(&fz, 20000);
                        const vocab_stage_result_t res = vocab_stage3_resume(&R.st, R.now, &R.view,
                                                                             &R.out);
                        CHECK(R.out.count == 0 && same_view(&saved, &R.view) && !res.locked &&
                              !res.outcome_ready, "fuzz: resume in a waiting state is silent");
                        apply_commands(false, "fuzz resume");
                        record(res, "fuzz resume");
                    }
                    events++;
                    if (R.done) break;
                    // Invariants after every call.
                    check_view_safety("fuzz");
                    CHECK(!R.res.locked || R.armed[VOCAB_TIMER_STAGE], "fuzz: locked without a "
                          "timer (stuck)");
                    if (R.view.anim.shake_seq != shake_before) shakes++;
                    CHECK(shakes + 1u <= (unsigned)max_misses, "fuzz: at most %u retry shakes",
                          (unsigned)(max_misses - 1u));
                    CHECK(!was_filled || R.view.card.skeleton.filled, "fuzz: the blank never empties");
                    was_filled = R.view.card.skeleton.filled;
                    if (answered) {
                        answers++;
                        const bool correct = key == R.right_key;
                        if (R.outcomes != outcomes_before) {
                            const vocab_result_t want =
                                correct ? (answers == 1u ? VOCAB_RESULT_CORRECT_FIRST
                                                         : VOCAB_RESULT_CORRECT_SECOND)
                                        : VOCAB_RESULT_WRONG;
                            CHECK(R.outcome == want && (correct || answers == (unsigned)max_misses),
                                  "fuzz: outcome %d after answer %u", (int)R.outcome, answers);
                        } else {
                            CHECK(!correct && answers < (unsigned)max_misses, "fuzz: an answer without an "
                                  "outcome must be a non-final miss");
                        }
                    }
                }
                // Finisher: the card must always complete (never stuck).
                for (int k = 0; k < 8 && !R.done; k++) {
                    if (R.res.locked) {
                        CHECK(R.armed[VOCAB_TIMER_STAGE], "fuzz finisher: lock without timer");
                        if (!R.armed[VOCAB_TIMER_STAGE]) break;
                        R.now = R.deadline[VOCAB_TIMER_STAGE];
                        rig_fire_due();
                    } else {
                        rig_click(R.right_key);
                    }
                }
                CHECK(R.done && R.outcomes == 1u, "fuzz uid %u purpose %d seed %u: DONE with one "
                      "outcome", e->uid, (int)PURPOSES[p], (unsigned)seed);
                CHECK(R.rng.state == rng_after_begin, "fuzz: no RNG draws after begin");
                CHECK(PURPOSES[p] == VOCAB_PURPOSE_LEARN || R.outcome != VOCAB_RESULT_CORRECT_SECOND,
                      "fuzz: check cards never report CORRECT_SECOND");
                cards++;
            }
        }
    }
    CHECK(cards == 34u * 4u * 100u && events > 150000u, "fuzz volume: %u cards, %u events", cards,
          events);
}

int main(void)
{
    test_begin_view();
    test_unusable_input();
    test_side_balance();
    test_learn_first_try();
    test_learn_sequences();
    test_check_cards();
    test_ignored_events();
    test_resume();
    test_row_never_reflows();
    test_all_entries_scripts();
    test_fuzz();
    return vocab_test_report("vocab Stage 3 tests");
}
