// tests/test_vocab_sim.c - system-level host simulation of the whole engine (input filter,
// idle/power policy, session, stage modules) driven by scripted raw BSP events and time,
// with a fake device (backlight, battery, saves, deep sleep = reboot from the saved state).
//
// Covers: end-to-end flows through raw keys (AC-4, AC-29, AC-30, AC-47, AC-48), the
// English-safety rule on every view model after every step (AC-27), and a fuzz run of
// 100,000+ random raw events across all states with invariants (AC-32: queue <= 10 cards,
// mastery 0..5, tick monotonic, no outbox overflow, valid UTF-8, never stuck).
//
// Owner: lead developer.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_engine.h"
#include "vocab_srs.h"
#include "vocab_strings.h"
#include "vocab_test.h"

// ---------------------------------------------------------------------------------------
// Fake device
// ---------------------------------------------------------------------------------------

#define MAX_PENDING 64

typedef struct {
    vocab_engine_t eng;
    vocab_view_t view;
    vocab_outbox_t out;
    vocab_record_t records[64];
    vocab_ms_t now;
    uint32_t seed;
    bool audio_ok;
    bool clips;
    int battery;
    uint8_t backlight;
    bool key_held[VOCAB_KEY_COUNT];
    // "NVS"
    bool saved_any;
    vocab_record_t saved_records[64];
    vocab_session_persist_t saved;
    // statistics
    unsigned saves, sleeps, refused, words, cues, epochs, steps, raw;
    unsigned screens_seen;   // bit per vocab_screen_t
    unsigned formats_seen;   // bit per vocab_format_t
    unsigned spot_checks, too_easy_checks, test_later;
    vocab_tick_t max_tick;
    // pending raw events (time ordered), emulating the BSP queue
    vocab_raw_event_t pending[MAX_PENDING];
    int npending;
} sim_t;

static sim_t S;
static const vocab_bank_t *BANK;

static bool sim_has_clip(void *ctx, vocab_uid_t uid)
{
    (void)ctx;
    (void)uid;
    return S.clips;
}

static void check_invariants(void);
static void scan_view(void);

static void execute(void);

static void boot(void)
{
    vocab_engine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session.bank = BANK;
    cfg.session.records = S.records;
    cfg.session.audio_ok = S.audio_ok;
    cfg.session.has_clip = sim_has_clip;
    if (S.saved_any) {
        memcpy(S.records, S.saved_records, sizeof(S.records));
        cfg.session.tick = S.saved.tick;
        cfg.session.xp = S.saved.xp;
        cfg.session.volume_index = S.saved.volume_index;
        cfg.session.objective_total = S.saved.objective_total;
        cfg.session.first_try_total = S.saved.first_try_total;
    }
    cfg.rng_seed = S.seed++;
    vocab_view_init(&S.view);
    vocab_outbox_clear(&S.out);
    vocab_engine_init(&S.eng, &cfg, S.now, &S.view, &S.out);
    memset(S.key_held, 0, sizeof(S.key_held));
    S.npending = 0;
    execute();
}

static void sim_init(uint32_t seed, bool audio_ok, bool clips)
{
    memset(&S, 0, sizeof(S));
    BANK = vocab_bank_builtin();
    for (vocab_index_t i = 0; i < BANK->count; i++) S.records[i].uid = BANK->entries[i].uid;
    S.seed = seed;
    S.audio_ok = audio_ok;
    S.clips = clips;
    S.battery = 80;
    S.now = 5000;
    boot();
}

static void drain_after_epoch(void)
{
    // Events already queued when the epoch started (same millisecond) are drained.
    while (S.npending > 0 && S.pending[0].t_ms <= S.now) {
        const vocab_raw_event_t raw = S.pending[0];
        memmove(&S.pending[0], &S.pending[1], (size_t)(S.npending - 1) * sizeof(S.pending[0]));
        S.npending--;
        if (raw.kind == VOCAB_RAW_PRESS) S.key_held[raw.key] = true;
        if (raw.kind == VOCAB_RAW_RELEASE) S.key_held[raw.key] = false;
        vocab_engine_key_drained(&S.eng, &raw, S.now);
    }
}

static void execute(void)
{
    CHECK(!S.out.overflow, "outbox overflow");
    for (uint8_t i = 0; i < S.out.count; i++) {
        const vocab_cmd_t *c = &S.out.cmd[i];
        switch (c->kind) {
        case VOCAB_CMD_PLAY_WORD:
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            CHECK(S.audio_ok && S.clips, "a word clip is requested only when it can play");
            S.words++;
            break;
        case VOCAB_CMD_PLAY_CUE:
            CHECK(S.audio_ok && S.eng.session.cfg.volume_index != 2, "cues only with audio");
            S.cues++;
            break;
        case VOCAB_CMD_NEW_EPOCH:
            S.epochs++;
            drain_after_epoch();
            break;
        case VOCAB_CMD_SAVE:
            S.saves++;
            memcpy(S.saved_records, S.records, sizeof(S.records));
            S.saved = vocab_engine_persist(&S.eng);
            S.saved_any = true;
            break;
        case VOCAB_CMD_BACKLIGHT:
            S.backlight = (uint8_t)c->value;
            break;
        case VOCAB_CMD_READ_BATTERY:
            vocab_engine_battery(&S.eng, S.battery, &S.view);
            break;
        case VOCAB_CMD_DEEP_SLEEP:
            if (S.key_held[0] || S.key_held[1] || S.key_held[2]) {
                S.refused++;
                vocab_engine_sleep_refused(&S.eng, S.now);
            } else {
                // Deep sleep, then a key wake reboots from the saved state.
                S.sleeps++;
                S.now += 60000;
                boot();
                return;
            }
            break;
        default:
            break;
        }
    }
    vocab_outbox_clear(&S.out);
}

static void after_step(void)
{
    S.steps++;
    S.screens_seen |= 1u << (unsigned)S.view.screen;
    if (S.eng.session.screen == VOCAB_SCREEN_CARD) {
        const vocab_card_t *c = &S.eng.session.cards[S.eng.session.cur];
        S.formats_seen |= 1u << (unsigned)S.eng.session.stage_format;
        if (c->purpose == VOCAB_PURPOSE_SPOT_CHECK) S.spot_checks++;
        if (c->purpose == VOCAB_PURPOSE_TOO_EASY_CHECK) S.too_easy_checks++;
        if (c->purpose == VOCAB_PURPOSE_TEST_LATER) S.test_later++;
    }
    scan_view();
    check_invariants();
}

// Runs every engine deadline up to time `t`.
static void advance_to(vocab_ms_t t)
{
    for (int guard = 0; guard < 10000; guard++) {
        vocab_ms_t d;
        if (!vocab_engine_next_deadline(&S.eng, &d) || !vocab_ms_reached(t, d)) break;
        if (vocab_ms_reached(S.now, d)) d = S.now;
        S.now = d;
        vocab_engine_poll(&S.eng, S.now, &S.view, &S.out);
        execute();
        after_step();
    }
    if (vocab_ms_reached(t, S.now)) S.now = t;
}

static void push_event(vocab_key_t key, vocab_raw_kind_t kind, vocab_ms_t t)
{
    if (S.npending >= MAX_PENDING) return;
    int i = S.npending++;
    while (i > 0 && (int32_t)(S.pending[i - 1].t_ms - t) > 0) {
        S.pending[i] = S.pending[i - 1];
        i--;
    }
    S.pending[i].key = key;
    S.pending[i].kind = kind;
    S.pending[i].t_ms = t;
}

// Delivers pending events up to time `t` (inclusive), running deadlines in between.
static void run_until(vocab_ms_t t)
{
    while (S.npending > 0 && vocab_ms_reached(t, S.pending[0].t_ms)) {
        const vocab_raw_event_t raw = S.pending[0];
        memmove(&S.pending[0], &S.pending[1], (size_t)(S.npending - 1) * sizeof(S.pending[0]));
        S.npending--;
        advance_to(raw.t_ms);
        if (raw.kind == VOCAB_RAW_PRESS) S.key_held[raw.key] = true;
        if (raw.kind == VOCAB_RAW_RELEASE) S.key_held[raw.key] = false;
        S.raw++;
        vocab_engine_key(&S.eng, &raw, S.now, &S.view, &S.out);
        execute();
        after_step();
    }
    advance_to(t);
}

static void wait_ms(uint32_t ms)
{
    run_until(S.now + ms);
}

// A tap: PRESS, RELEASE after `ms`, CLICK 180 ms later (BSP timing).
static void tap(vocab_key_t key, uint32_t ms)
{
    const vocab_ms_t t = S.now + 1;
    push_event(key, VOCAB_RAW_PRESS, t);
    push_event(key, VOCAB_RAW_RELEASE, t + ms);
    push_event(key, VOCAB_RAW_CLICK, t + ms + 180);
    run_until(t + ms + 181);
}

// A long hold: PRESS, LONG at 500 ms, RELEASE after `ms` (no CLICK).
static void hold(vocab_key_t key, uint32_t ms)
{
    const vocab_ms_t t = S.now + 1;
    push_event(key, VOCAB_RAW_PRESS, t);
    if (ms >= 500) push_event(key, VOCAB_RAW_LONG, t + 500);
    push_event(key, VOCAB_RAW_RELEASE, t + ms);
    run_until(t + ms + 1);
}

// ---------------------------------------------------------------------------------------
// English-safety scanner (AC-27) and invariants (AC-32)
// ---------------------------------------------------------------------------------------

static bool valid_utf8(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0) return false;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += n;
    }
    return true;
}

static bool has_ascii_letter(const char *s)
{
    for (; *s; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) return true;
    }
    return false;
}

static bool is_text(const char *s, int field)
{
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        const vocab_entry_t *e = &BANK->entries[i];
        const char *v = field == 0 ? e->text : field == 1 ? e->ipa : field == 2 ? e->example_cloze
                      : field == 3 ? e->example : e->zh;
        if (v && strcmp(v, s) == 0) return true;
    }
    return false;
}

static bool skeleton_ok(const vocab_view_skeleton_t *sk)
{
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        const vocab_entry_t *e = &BANK->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        const size_t pos = e->cloze_pos;
        const size_t len = strlen(e->cloze_right);
        if (strlen(sk->pre) != pos || strncmp(sk->pre, e->text, pos) != 0) continue;
        if (strcmp(sk->suf, e->text + pos + len) != 0) continue;
        if (strcmp(sk->right, e->cloze_right) != 0) continue;
        if (strcmp(sk->other, e->cloze_wrong) != 0) continue;
        return true;
    }
    return false;
}

static bool is_chunk(const char *s)
{
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        const vocab_entry_t *e = &BANK->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        if (strcmp(s, e->cloze_right) == 0 || strcmp(s, e->cloze_wrong) == 0) return true;
    }
    return false;
}

// No visible text contains a misspelled word (the entry text with the wrong chunk).
static void check_no_misspelling(const char *s)
{
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        const vocab_entry_t *e = &BANK->entries[i];
        if (!vocab_entry_has_cloze(e)) continue;
        char bad[64];
        const size_t pos = e->cloze_pos;
        snprintf(bad, sizeof(bad), "%.*s%s%s", (int)pos, e->text, e->cloze_wrong,
                 e->text + pos + strlen(e->cloze_right));
        CHECK(strstr(s, bad) == NULL, "misspelling '%s' shown in '%s' (AC-27)", bad, s);
    }
}

static void label(const vocab_view_label_t *l, int kind, const char *what)
{
    if (!l->visible) return;
    CHECK(valid_utf8(l->text), "%s: invalid UTF-8", what);
    check_no_misspelling(l->text);
    switch (kind) {
    case 0: case 1: case 2: case 3: case 4:
        CHECK(is_text(l->text, kind), "%s '%s' is not bank data (AC-27)", what, l->text);
        break;
    default:  // Chinese UI text: no English letters at all
        CHECK(!has_ascii_letter(l->text), "%s '%s' contains English letters", what, l->text);
        break;
    }
}

static void scan_view(void)
{
    const vocab_view_t *v = &S.view;
    label(&v->card.word, 0, "word");
    label(&v->card.ipa, 1, "ipa");
    label(&v->card.sentence, 2, "sentence");
    label(&v->card.example, 3, "example");
    label(&v->card.meaning, 4, "meaning");
    label(&v->card.s3_meaning, 4, "s3_meaning");
    label(&v->card.prompt, 9, "prompt");
    label(&v->card.banner, 9, "banner");
    label(&v->home.title, 9, "home title");
    label(&v->home.xp, 9, "home xp");
    label(&v->home.stats, 9, "home stats");
    label(&v->home.warn, 9, "home warn");
    label(&v->home.go, 9, "home go");
    label(&v->summary.score, 9, "summary score");
    label(&v->summary.xp, 9, "summary xp");
    label(&v->summary.levelup, 9, "summary levelup");
    CHECK(!has_ascii_letter(v->hint.text), "hint '%s' contains English", v->hint.text);
    if (v->status.progress.visible) {
        const char *p = v->status.progress.text;
        CHECK(strncmp(p, "Lv.", 3) == 0 || !has_ascii_letter(p), "progress '%s'", p);
    }
    if (v->card.skeleton.visible) {
        const vocab_view_skeleton_t *sk = &v->card.skeleton;
        CHECK(skeleton_ok(sk), "skeleton '%s|%s|%s' not a bank cloze (AC-27)", sk->pre, sk->right, sk->suf);
        char shown[64];
        snprintf(shown, sizeof(shown), "%s%s%s", sk->pre, sk->filled ? sk->right : "", sk->suf);
        check_no_misspelling(shown);
    }
    for (int i = 0; i < 2; i++) {
        const vocab_view_panel_t *p = &v->panel[i];
        if (!p->visible) continue;
        CHECK(valid_utf8(p->text), "panel UTF-8");
        if (p->text_en) {
            CHECK(is_chunk(p->text), "panel chunk '%s' is not an isolated bank chunk (AC-27)", p->text);
        } else {
            CHECK(!has_ascii_letter(p->text), "panel '%s' contains English", p->text);
        }
    }
}

static void check_invariants(void)
{
    const vocab_session_t *s = &S.eng.session;
    CHECK(s->card_count <= VOCAB_MAX_CARDS, "queue <= 10 cards (AC-32)");
    CHECK(s->cur <= s->card_count, "cursor in range");
    if (s->screen == VOCAB_SCREEN_CARD) CHECK(s->stage_format != VOCAB_FORMAT_NONE, "card has a stage");
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        CHECK(S.records[i].mastery <= VOCAB_MASTERY_MAX, "mastery 0..5 (AC-32)");
    }
    const vocab_tick_t tick = vocab_engine_persist(&S.eng).tick;
    CHECK(tick >= S.max_tick, "tick monotonic (AC-32)");
    S.max_tick = tick;
    CHECK(S.view.screen == (vocab_screen_t)s->screen || s->screen == VOCAB_SCREEN_BOOT,
          "view screen follows the session");
}

// ---------------------------------------------------------------------------------------
// Helpers to play cards through raw keys
// ---------------------------------------------------------------------------------------

static void settle(void)
{
    for (int i = 0; i < 80 && vocab_session_locked(&S.eng.session); i++) wait_ms(100);
}

static vocab_key_t sim_correct_key(void)
{
    const vocab_session_t *s = &S.eng.session;
    const vocab_entry_t *e = vocab_bank_at(BANK, s->cards[s->cur].index);
    const char *want = S.view.card.skeleton.visible ? e->cloze_right : e->zh;
    return strcmp(S.view.panel[1].text, want) == 0 ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
}

static void play_card_correctly(void)
{
    settle();
    const vocab_session_t *s = &S.eng.session;
    if (s->screen != VOCAB_SCREEN_CARD) return;
    wait_ms(300);  // past the 250 ms cooldown
    if (s->stage_format == VOCAB_FORMAT_S2) {
        wait_ms(VOCAB_S2_FRONT_MIN_MS);
        tap(VOCAB_KEY_OK, 80);
        wait_ms(300);
        tap(VOCAB_KEY_UP, 80);
    } else {
        tap(sim_correct_key(), 80);
    }
    wait_ms(100);
}

// ---------------------------------------------------------------------------------------
// Scripted scenarios
// ---------------------------------------------------------------------------------------

static void test_session_through_raw_keys(void)
{
    sim_init(1, true, true);
    CHECK(S.view.screen == VOCAB_SCREEN_HOME && S.backlight == VOCAB_BACKLIGHT_ON, "boot -> HOME, backlight");
    tap(VOCAB_KEY_OK, 80);
    CHECK(S.view.screen == VOCAB_SCREEN_CARD, "OK tap starts a session (AC-4)");
    CHECK(S.words > 0, "word clip requested on card entry");

    // A reflex UP within 250 ms of the new card is dropped (AC-29).
    const uint8_t cur = S.eng.session.cur;
    push_event(VOCAB_KEY_UP, VOCAB_RAW_PRESS, S.now + 100);
    push_event(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, S.now + 160);
    push_event(VOCAB_KEY_UP, VOCAB_RAW_CLICK, S.now + 340);
    wait_ms(400);
    CHECK(S.eng.session.cur == cur && !S.eng.session.outcome_seen, "cooldown swallowed the reflex press");

    for (int guard = 0; guard < 30 && S.view.screen != VOCAB_SCREEN_SUMMARY; guard++) play_card_correctly();
    settle();
    CHECK(S.view.screen == VOCAB_SCREEN_SUMMARY, "session completes through raw keys");
    CHECK(S.saves >= 1 && S.saved.tick == 1, "SUMMARY saved with the new tick");
    wait_ms(VOCAB_SUMMARY_LOCK_MS);
    tap(VOCAB_KEY_UP, 80);
    CHECK(S.view.screen == VOCAB_SCREEN_HOME, "UP on SUMMARY -> HOME");
}

static void test_idle_and_wake(void)
{
    sim_init(2, true, true);
    tap(VOCAB_KEY_OK, 80);
    const unsigned saves = S.saves;
    wait_ms(VOCAB_IDLE_DIM_MS);
    CHECK(S.backlight == VOCAB_BACKLIGHT_DIM, "dim at 20 s (AC-47)");
    wait_ms(VOCAB_IDLE_OFF_MS - VOCAB_IDLE_DIM_MS);
    CHECK(S.backlight == 0 && S.saves == saves + 1, "off at 30 s with a save (AC-47)");
    CHECK(vocab_engine_power(&S.eng) == VOCAB_POWER_OFF, "SCREEN_OFF");
    const vocab_screen_t screen = S.view.screen;
    const uint8_t cur = S.eng.session.cur;
    // Short press: wakes only; its delayed CLICK is swallowed (AC-30).
    tap(VOCAB_KEY_DOWN, 80);
    wait_ms(50);
    CHECK(S.backlight == VOCAB_BACKLIGHT_ON && S.view.screen == screen && S.eng.session.cur == cur &&
              !S.eng.session.outcome_seen, "wake only (AC-30)");
    // 1 s hold while off: wakes only, LONG swallowed.
    wait_ms(VOCAB_IDLE_OFF_MS + 100);
    CHECK(S.backlight == 0, "off again");
    const uint8_t volume = S.eng.session.cfg.volume_index;
    hold(VOCAB_KEY_DOWN, 1000);
    CHECK(S.eng.session.cfg.volume_index == volume, "a waking hold does not change the volume (AC-30)");
    wait_ms(450);
    hold(VOCAB_KEY_DOWN, 700);
    CHECK(S.eng.session.cfg.volume_index != volume, "the next hold acts");
}

static void test_deep_sleep(void)
{
    sim_init(3, false, false);
    tap(VOCAB_KEY_OK, 80);
    play_card_correctly();
    const vocab_index_t word = S.eng.session.cards[0].index;
    // A key held at the deep-sleep moment (read through the ADC, no new event): postponed,
    // retried 5 s later (AC-48).
    wait_ms(VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS - 2000);
    S.key_held[VOCAB_KEY_UP] = true;
    wait_ms(2500);
    CHECK(S.refused == 1 && S.sleeps == 0, "deep sleep refused while a key is held (AC-48)");
    S.key_held[VOCAB_KEY_UP] = false;
    wait_ms(VOCAB_SLEEP_RETRY_MS + 10);
    CHECK(S.sleeps == 1, "retried after 5 s and slept (AC-47)");
    CHECK(S.view.screen == VOCAB_SCREEN_HOME, "key wake reboots to HOME (AC-3 logic)");
    CHECK(vocab_record_seen(&S.records[word]) && S.records[word].mastery == 1,
          "progress survived the deep sleep (saved before sleeping)");
}

// ---------------------------------------------------------------------------------------
// The three real stages end to end (integration): every learning path of each stage played
// through raw keys, and each card's outcome checked against the SRS rules applied to a
// shadow copy of the records (so the session passed the stage's purpose, format and result).
// ---------------------------------------------------------------------------------------

static vocab_index_t index_of(vocab_uid_t uid)
{
    vocab_index_t i = VOCAB_INDEX_NONE;
    CHECK(vocab_bank_find(BANK, uid, &i), "uid %u in the bank", (unsigned)uid);
    return i;
}

static vocab_key_t other_key(vocab_key_t k)
{
    return k == VOCAB_KEY_UP ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
}

static int panel_of(vocab_key_t k)
{
    return k == VOCAB_KEY_UP ? 0 : 1;
}

static void test_three_stages_end_to_end(void)
{
    sim_init(21, true, true);
    const vocab_tick_t tick = 10;
    const vocab_index_t dog = index_of(202), pen = index_of(401), cat = index_of(201);
    const vocab_index_t duck = index_of(203), bus = index_of(301);
    // Everything mastered and not due except: two new words (Stage 1), one m = 1 word
    // (Stage 2) and two m = 3 words with cloze data (Stage 3), all due now.
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        vocab_record_t *r = &S.saved_records[i];
        memset(r, 0, sizeof(*r));
        r->uid = BANK->entries[i].uid;
        r->flags = VOCAB_REC_SEEN | VOCAB_REC_EVER_MASTERED;
        r->mastery = VOCAB_MASTERY_MAX;
        r->due = (vocab_tick_t)(tick + 30u);
    }
    const vocab_index_t fresh[2] = { dog, pen };
    for (int k = 0; k < 2; k++) {
        vocab_record_t *r = &S.saved_records[fresh[k]];
        r->flags = 0;
        r->mastery = 0;
        r->due = 0;
    }
    const struct { vocab_index_t index; uint8_t mastery; } due[3] = { { cat, 1 }, { duck, 3 }, { bus, 3 } };
    for (int k = 0; k < 3; k++) {
        S.saved_records[due[k].index].flags = VOCAB_REC_SEEN;
        S.saved_records[due[k].index].mastery = due[k].mastery;
        S.saved_records[due[k].index].due = tick;
    }
    CHECK(vocab_entry_has_cloze(vocab_bank_at(BANK, duck)) && vocab_entry_has_cloze(vocab_bank_at(BANK, bus)),
          "Stage 3 words have cloze data");
    S.saved.tick = tick;
    S.saved_any = true;
    boot();
    vocab_record_t shadow[64];
    memcpy(shadow, S.records, sizeof(shadow));

    tap(VOCAB_KEY_OK, 80);
    const vocab_session_t *s = &S.eng.session;
    CHECK(S.view.screen == VOCAB_SCREEN_CARD && s->card_count == 5, "5-card session (3 due + 2 new)");
    unsigned per_format[4] = { 0, 0, 0, 0 };
    unsigned s1_learn = 0, s3_learn = 0, test_later = 0;
    for (int guard = 0; guard < 12 && S.view.screen != VOCAB_SCREEN_SUMMARY; guard++) {
        settle();
        if (S.view.screen != VOCAB_SCREEN_CARD) continue;
        const vocab_card_t card = s->cards[s->cur];
        const uint8_t cur = s->cur;
        const vocab_format_t format = s->stage_format;
        wait_ms(300);  // past the 250 ms UP/DOWN cooldown of the new card (R4)
        const vocab_key_t right = sim_correct_key();
        const vocab_key_t wrong = other_key(right);
        vocab_result_t result = VOCAB_RESULT_NONE;
        per_format[format]++;
        if (format == VOCAB_FORMAT_S1 && card.purpose == VOCAB_PURPOSE_LEARN && s1_learn++ == 0) {
            // Wrong first press: generic error hold (spec 5.3, AC-15).
            tap(wrong, 80);
            CHECK(vocab_session_locked(s) && S.view.badge == VOCAB_BADGE_REMEMBER &&
                      !S.view.panel[panel_of(wrong)].visible && S.view.panel[panel_of(right)].visible,
                  "Stage 1 error hold: wrong panel hidden, badge REMEMBER");
            tap(right, 80);
            CHECK(s->cur == cur && vocab_session_locked(s), "keys are ignored in the hold lock (AC-15)");
            wait_ms(VOCAB_ERROR_HOLD_MS);
            CHECK(!vocab_session_locked(s) && S.view.panel[panel_of(right)].nudge_repeat,
                  "wait part: the correct panel nudges");
            wait_ms(300);
            const uint8_t nudges = S.view.anim.nudge_seq[panel_of(right)];
            tap(wrong, 80);
            CHECK(s->cur == cur && S.view.anim.nudge_seq[panel_of(right)] != nudges,
                  "wait part: the other key only nudges");
            wait_ms(300);
            tap(right, 80);
            result = VOCAB_RESULT_WRONG;
        } else if (format == VOCAB_FORMAT_S1) {
            if (card.purpose == VOCAB_PURPOSE_TEST_LATER) {
                test_later++;
                CHECK(S.view.badge == VOCAB_BADGE_TEST_AGAIN && card.index == cat,
                      "the TEST_LATER card asks the forgotten word in Stage 1");
            }
            tap(right, 80);
            CHECK(S.view.panel[panel_of(right)].style == VOCAB_PANEL_CORRECT, "Stage 1 correct feedback");
            result = VOCAB_RESULT_CORRECT_FIRST;
        } else if (format == VOCAB_FORMAT_S2) {
            // Front: clicks before 1000 ms are ignored, then any key flips (spec 6, AC-17).
            tap(VOCAB_KEY_OK, 80);
            CHECK(!S.view.panel[0].visible && S.view.card.word.visible, "front ignores an early click");
            wait_ms(VOCAB_S2_FRONT_MIN_MS);
            tap(VOCAB_KEY_OK, 80);
            CHECK(S.view.panel[0].visible && S.view.panel[1].visible && S.view.card.meaning.visible,
                  "any key flips to the back");
            wait_ms(300);
            const uint8_t count = s->card_count;
            tap(VOCAB_KEY_DOWN, 80);  // "forgot, test me later" (O4)
            const unsigned pos = cur + 3u < count ? cur + 3u : count;
            CHECK(s->card_count == count + 1 && s->cards[pos].purpose == VOCAB_PURPOSE_TEST_LATER &&
                      s->cards[pos].index == card.index, "forgot inserts TEST_LATER two cards later (AC-18)");
            result = VOCAB_RESULT_FORGOT;
        } else if (format == VOCAB_FORMAT_S3 && s3_learn++ == 0) {
            // Miss 1: shake + retry lock, no outcome yet; correct second press (O8).
            const uint8_t shakes = S.view.anim.shake_seq;
            tap(wrong, 80);
            CHECK(vocab_session_locked(s) && !s->outcome_seen && !S.view.card.skeleton.filled &&
                      S.view.anim.shake_seq != shakes, "Stage 3 miss: shake and retry lock");
            wait_ms(VOCAB_S3_RETRY_LOCK_MS + 300);
            tap(right, 80);
            CHECK(S.view.card.skeleton.filled && vocab_session_locked(s), "success: full word, hold");
            result = VOCAB_RESULT_CORRECT_SECOND;
        } else if (format == VOCAB_FORMAT_S3) {
            // Two misses: reveal + error hold (O9), then only the correct key continues.
            tap(wrong, 80);
            wait_ms(VOCAB_S3_RETRY_LOCK_MS + 300);
            tap(wrong, 80);
            CHECK(S.view.card.skeleton.filled && S.view.badge == VOCAB_BADGE_REMEMBER &&
                      !S.view.panel[panel_of(wrong)].visible, "Stage 3 reveal hold");
            wait_ms(VOCAB_ERROR_HOLD_MS + 300);
            tap(right, 80);
            result = VOCAB_RESULT_WRONG;
        } else {
            CHECK(false, "unexpected card format %d", (int)format);
            play_card_correctly();
            continue;
        }
        vocab_srs_apply(&shadow[card.index], card.purpose, format, result, tick, card.was_seen);
        settle();
        CHECK(s->cur != cur || S.view.screen == VOCAB_SCREEN_SUMMARY, "card %u finished", (unsigned)cur);
    }
    CHECK(S.view.screen == VOCAB_SCREEN_SUMMARY, "the session reaches SUMMARY");
    CHECK(per_format[VOCAB_FORMAT_S1] == 3 && per_format[VOCAB_FORMAT_S2] == 1 &&
              per_format[VOCAB_FORMAT_S3] == 2 && test_later == 1,
          "2 new words in Stage 1, 1 in Stage 2 (+ TEST_LATER in Stage 1), 2 in Stage 3");
    const vocab_index_t words[5] = { dog, pen, cat, duck, bus };
    for (int k = 0; k < 5; k++) {
        const vocab_record_t *got = &S.records[words[k]];
        const vocab_record_t *want = &shadow[words[k]];
        CHECK(got->mastery == want->mastery && got->due == want->due && got->lapses == want->lapses &&
                  vocab_record_seen(got),
              "%s: m %u due %u lapses %u (expected %u / %u / %u)", vocab_bank_at(BANK, words[k])->text,
              got->mastery, got->due, got->lapses, want->mastery, want->due, want->lapses);
    }
    CHECK(S.saves >= 1 && S.saved.tick == tick + 1, "SUMMARY saved with tick + 1");
    printf("end to end: Stage 1 x%u (error hold, correct, TEST_LATER), Stage 2 x%u (forgot), "
           "Stage 3 x%u (second try, reveal); outcomes match the SRS rules\n",
           per_format[VOCAB_FORMAT_S1], per_format[VOCAB_FORMAT_S2], per_format[VOCAB_FORMAT_S3]);
}

// ---------------------------------------------------------------------------------------
// Double taps with the button driver's timing (review R-1 / QA-4): PRESS, RELEASE after
// `ms1`, PRESS after `gap`, RELEASE after `ms2`, one DOUBLE 185 ms later.
// ---------------------------------------------------------------------------------------

static void push_double_tap(vocab_key_t key, vocab_ms_t t, uint32_t ms1, uint32_t gap, uint32_t ms2)
{
    push_event(key, VOCAB_RAW_PRESS, t);
    push_event(key, VOCAB_RAW_RELEASE, t + ms1);
    push_event(key, VOCAB_RAW_PRESS, t + ms1 + gap);
    push_event(key, VOCAB_RAW_RELEASE, t + ms1 + gap + ms2);
    push_event(key, VOCAB_RAW_DOUBLE, t + ms1 + gap + ms2 + 185);
}

static void test_double_taps(void)
{
    sim_init(31, true, true);
    tap(VOCAB_KEY_OK, 80);
    settle();
    CHECK(S.view.screen == VOCAB_SCREEN_CARD && S.eng.session.stage_format == VOCAB_FORMAT_S1,
          "fresh device: Stage 1 card");
    // Screen off, then a waking double tap of the wrong key (100 ms taps, 150 ms gap): it only
    // wakes (spec 3.3 R2, AC-30).
    wait_ms(VOCAB_IDLE_OFF_MS + 100);
    CHECK(S.backlight == 0, "screen off");
    const uint8_t cur = S.eng.session.cur;
    const vocab_key_t right = sim_correct_key();
    const vocab_key_t wrong = right == VOCAB_KEY_UP ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
    vocab_ms_t t = S.now + 1;
    push_double_tap(wrong, t, 100, 150, 100);
    run_until(t + 536);
    CHECK(S.backlight == VOCAB_BACKLIGHT_ON && S.eng.session.cur == cur && !S.eng.session.outcome_seen &&
              !vocab_session_locked(&S.eng.session), "a waking double tap only wakes (AC-30)");
    // A double tap on the settled card is one click.
    wait_ms(500);
    t = S.now + 1;
    push_double_tap(right, t, 100, 150, 100);
    run_until(t + 536);
    CHECK(S.eng.session.outcome_seen && S.view.panel[right == VOCAB_KEY_UP ? 0 : 1].style == VOCAB_PANEL_CORRECT,
          "a double tap on a settled card answers it once");
    // A double tap whose first press begins 30 ms before the next card appears never acts on
    // that card (spec 3.3 R3, AC-29).
    vocab_ms_t change;
    CHECK(vocab_engine_next_deadline(&S.eng, &change), "feedback timer armed");
    push_double_tap(VOCAB_KEY_DOWN, change - 30, 110, 175, 110);
    run_until(change + 1200);
    CHECK(S.eng.session.cur == cur + 1 && S.view.screen == VOCAB_SCREEN_CARD && !S.eng.session.outcome_seen,
          "a double tap straddling the card change is dropped (AC-29)");
}

// ---------------------------------------------------------------------------------------
// Fuzz (AC-32)
// ---------------------------------------------------------------------------------------

static unsigned raw_events(void)
{
    return S.raw;
}

static void recover_to_home(void)
{
    // Liveness: from any state a short key sequence reaches HOME.
    for (int attempt = 0; attempt < 12 && S.view.screen != VOCAB_SCREEN_HOME; attempt++) {
        S.npending = 0;
        memset(S.key_held, 0, sizeof(S.key_held));
        if (vocab_engine_power(&S.eng) != VOCAB_POWER_ACTIVE) {
            tap(VOCAB_KEY_OK, 80);  // wake (only)
            wait_ms(500);
        }
        settle();
        wait_ms(300);
        switch (S.view.screen) {
        case VOCAB_SCREEN_CARD:
            hold(VOCAB_KEY_OK, 700);
            break;
        case VOCAB_SCREEN_PAUSE:
            tap(VOCAB_KEY_DOWN, 80);
            break;
        case VOCAB_SCREEN_SUMMARY:
            tap(VOCAB_KEY_UP, 80);
            break;
        default:
            wait_ms(1000);
            break;
        }
    }
    CHECK(S.view.screen == VOCAB_SCREEN_HOME, "never stuck: HOME reachable (AC-32), screen %d",
          (int)S.view.screen);
}

static void fuzz(uint32_t seed, bool audio_ok, bool clips, unsigned target_events)
{
    sim_init(seed, audio_ok, clips);
    vocab_rng_t r;
    vocab_rng_seed(&r, seed * 7919u);
    // Mid-progress device: a mix of masteries so all three stages appear.
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        const uint8_t m = (uint8_t)vocab_rng_below(&r, 6);
        S.saved_records[i].uid = BANK->entries[i].uid;
        S.saved_records[i].mastery = m;
        S.saved_records[i].flags = m > 0 || vocab_rng_bit(&r) ? VOCAB_REC_SEEN : 0;
        S.saved_records[i].due = (vocab_tick_t)vocab_rng_below(&r, 6);
    }
    S.saved.tick = 3;
    S.saved_any = true;
    boot();
    const unsigned start = raw_events();
    unsigned actions = 0;
    while (raw_events() - start < target_events) {
        const vocab_key_t key = (vocab_key_t)vocab_rng_below(&r, 3);
        const uint32_t pick = vocab_rng_below(&r, 100);
        if (pick < 45) {
            tap(key, 10 + vocab_rng_below(&r, 400));             // incl. < 30 ms bounces
        } else if (pick < 55) {
            hold(key, 500 + vocab_rng_below(&r, 1500));          // LONG
        } else if (pick < 62) {
            // Double tap with the driver's timing: taps of 30-175 ms (the driver drops a second
            // press >= 180 ms), a gap of 20-185 ms, one DOUBLE 185 ms after the second release.
            const uint32_t ms1 = 30 + vocab_rng_below(&r, 146);
            const uint32_t gap = 20 + vocab_rng_below(&r, 166);
            const uint32_t ms2 = 30 + vocab_rng_below(&r, 146);
            const vocab_ms_t t = S.now + 1;
            push_double_tap(key, t, ms1, gap, ms2);
            run_until(t + ms1 + gap + ms2 + 186);
        } else if (pick < 66) {
            // Cross-key phantom: another key clicks during a long hold.
            const vocab_key_t k2 = (vocab_key_t)((key + 1u) % 3u);
            const vocab_ms_t t = S.now + 1;
            push_event(key, VOCAB_RAW_PRESS, t);
            push_event(k2, VOCAB_RAW_PRESS, t + 300);
            push_event(k2, VOCAB_RAW_RELEASE, t + 310);
            push_event(k2, VOCAB_RAW_CLICK, t + 490);
            push_event(key, VOCAB_RAW_LONG, t + 500);
            push_event(key, VOCAB_RAW_RELEASE, t + 900);
            run_until(t + 901);
        } else if (pick < 90) {
            wait_ms(vocab_rng_below(&r, 3000));
        } else if (pick < 96) {
            play_card_correctly();
        } else if (pick < 98) {
            S.battery = (int)vocab_rng_below(&r, 102) - 1;
            wait_ms(VOCAB_BATTERY_POLL_MS);
        } else {
            wait_ms(pick == 99 ? 200000 : 35000);  // screen off / deep sleep + reboot
        }
        if (++actions % 1500 == 0) recover_to_home();
    }
    recover_to_home();
    CHECK(S.saves > 0, "fuzz saved at least once");
    const unsigned screens = (1u << VOCAB_SCREEN_HOME) | (1u << VOCAB_SCREEN_CARD) |
                             (1u << VOCAB_SCREEN_PAUSE) | (1u << VOCAB_SCREEN_SUMMARY);
    CHECK((S.screens_seen & screens) == screens, "fuzz reached HOME, CARD, PAUSE, SUMMARY (0x%x)", S.screens_seen);
    const unsigned formats = (1u << VOCAB_FORMAT_S1) | (1u << VOCAB_FORMAT_S2) | (1u << VOCAB_FORMAT_S3);
    CHECK((S.formats_seen & formats) == formats, "fuzz ran all three stages (0x%x)", S.formats_seen);
    printf("fuzz seed %u: %u raw events, %u steps, %u saves, %u deep sleeps, %u refused, screens 0x%x, "
           "spot %u, too-easy %u, test-later %u, clips %u, cues %u\n", (unsigned)seed, S.raw, S.steps,
           S.saves, S.sleeps, S.refused, S.screens_seen, S.spot_checks, S.too_easy_checks, S.test_later,
           S.words, S.cues);
}

int main(void)
{
    test_session_through_raw_keys();
    test_three_stages_end_to_end();
    test_double_taps();
    test_idle_and_wake();
    test_deep_sleep();
    fuzz(11, true, true, 40000);
    fuzz(12, true, false, 30000);
    fuzz(13, false, false, 30000);
    return vocab_test_report("vocab system simulation");
}
