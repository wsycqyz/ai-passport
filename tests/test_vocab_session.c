// tests/test_vocab_session.c - session flow (spec 3.2, 4, 8.3/8.4, 9, 11.5; AC-4, AC-5,
// AC-8 - AC-12, AC-18 - AC-22, AC-33, AC-34, AC-43, AC-50).
//
// Owner: lead developer. The tests drive the session directly with intents and timers and
// answer cards through the view model only, so they hold for the stage placeholders and the
// real stage modules alike (real stages add lock windows, which the helpers wait out).
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_session.h"
#include "vocab_strings.h"
#include "vocab_test.h"

typedef struct {
    vocab_session_t s;
    vocab_view_t view;
    vocab_outbox_t out;
    vocab_outbox_t all;          // commands collected since the last start()/mark
    vocab_rng_t rng;
    vocab_record_t records[64];
    vocab_ms_t now;
    bool clips;
} fixture_t;

static fixture_t F;
static const vocab_bank_t *BANK;

static bool has_clip(void *ctx, vocab_uid_t uid)
{
    (void)ctx;
    (void)uid;
    return F.clips;
}

static void collect(void)
{
    for (uint8_t i = 0; i < F.out.count && F.all.count < VOCAB_OUTBOX_CAP; i++) {
        const vocab_cmd_t *c = &F.out.cmd[i];
        vocab_outbox_push(&F.all, c->kind, c->cue, c->timer, c->uid, c->value);
    }
    CHECK(!F.out.overflow, "outbox overflow");
}

static void mark(void)
{
    vocab_outbox_clear(&F.all);
}

static void init_session(vocab_tick_t tick, uint32_t xp, bool audio_ok, uint8_t volume_index)
{
    const vocab_session_config_t cfg = {
        .bank = BANK, .records = F.records, .rng = &F.rng, .audio_ok = audio_ok,
        .has_clip = has_clip, .clip_ctx = NULL, .tick = tick, .xp = xp,
        .volume_index = volume_index, .objective_total = 0, .first_try_total = 0,
        .storage_warning = false,
    };
    vocab_outbox_clear(&F.out);
    vocab_session_init(&F.s, &cfg, F.now, &F.view, &F.out);
}

static void setup_bank(const vocab_bank_t *bank, uint32_t seed, bool audio_ok, bool clips)
{
    memset(&F, 0, sizeof(F));
    BANK = bank;
    for (vocab_index_t i = 0; i < bank->count; i++) F.records[i].uid = bank->entries[i].uid;
    vocab_rng_seed(&F.rng, seed);
    vocab_view_init(&F.view);
    F.now = 1000;
    F.clips = clips;
    init_session(10, 0, audio_ok, 0);
}

static void setup(uint32_t seed, bool audio_ok, bool clips)
{
    setup_bank(vocab_bank_builtin(), seed, audio_ok, clips);
}

static void wait_ms(uint32_t ms)
{
    F.now += ms;
    vocab_outbox_clear(&F.out);
    vocab_session_poll(&F.s, F.now, &F.view, &F.out);
    collect();
}

static void send(vocab_event_kind_t kind, vocab_key_t key)
{
    wait_ms(300);
    const vocab_event_t ev = { kind, key, VOCAB_TIMER_STAGE };
    vocab_outbox_clear(&F.out);
    vocab_session_handle(&F.s, &ev, F.now, &F.view, &F.out);
    collect();
}

static void click(vocab_key_t key)
{
    send(VOCAB_EV_CLICK, key);
}

// Waits out lock windows and banners (at most 6 s).
static void settle(void)
{
    for (int i = 0; i < 60 && vocab_session_locked(&F.s); i++) wait_ms(100);
}

// Waits out the current card's own lock windows only (not banners or the SUMMARY lock).
static void settle_card(void)
{
    for (int i = 0; i < 60 && F.s.screen == VOCAB_SCREEN_CARD && vocab_session_locked(&F.s); i++) {
        wait_ms(100);
    }
}

static const vocab_card_t *card(void)
{
    return F.s.cur < F.s.card_count ? &F.s.cards[F.s.cur] : NULL;
}

static const vocab_entry_t *entry(void)
{
    const vocab_card_t *c = card();
    return c ? vocab_bank_at(BANK, c->index) : NULL;
}

// The key that answers the visible objective card correctly (view model only).
static vocab_key_t correct_key(void)
{
    const vocab_entry_t *e = entry();
    const char *want = F.view.card.skeleton.visible ? e->cloze_right : e->zh;
    return strcmp(F.view.panel[1].text, want) == 0 ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
}

static vocab_key_t other(vocab_key_t key)
{
    return key == VOCAB_KEY_UP ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
}

// Stage 2 front -> back.
static void flip(void)
{
    wait_ms(VOCAB_S2_FRONT_MIN_MS + 10);
    click(VOCAB_KEY_OK);
}

// Answers the current card. S2: `good` = I know, else forgot. S1/S3: `good` = correct first
// press, else wrong until the card reveals, then the correct key.
static void answer(bool good)
{
    settle();
    CHECK(F.s.screen == VOCAB_SCREEN_CARD, "answer() on a card (screen %d)", (int)F.s.screen);
    if (F.s.screen != VOCAB_SCREEN_CARD) return;
    const uint8_t start_cur = F.s.cur;
    if (F.s.stage_format == VOCAB_FORMAT_S2) {
        flip();
        click(good ? VOCAB_KEY_UP : VOCAB_KEY_DOWN);
        settle_card();
        return;
    }
    const vocab_key_t right = correct_key();
    click(good ? right : other(right));
    for (int guard = 0; guard < 8; guard++) {
        settle_card();
        if (F.s.screen != VOCAB_SCREEN_CARD || F.s.cur != start_cur) break;
        // Still on the card: retry state (both options) -> wrong again; reveal/hold -> right.
        const bool both = F.view.panel[0].visible && F.view.panel[1].visible;
        click(both && !good ? other(right) : right);
    }
    settle_card();
}

// Every word seen with `mastery`, due long ago (so the session takes them as reviews).
static void all_due(uint8_t mastery)
{
    for (vocab_index_t i = 0; i < BANK->count; i++) {
        F.records[i].flags = VOCAB_REC_SEEN;
        F.records[i].mastery = mastery;
        F.records[i].due = 0;
    }
}

static void start(void)
{
    mark();
    click(VOCAB_KEY_OK);
}

// Answers every remaining card (incl. banners and inserted cards) correctly.
static void finish_session_cards(void)
{
    for (int guard = 0; guard < 20; guard++) {
        settle();
        if (F.s.screen != VOCAB_SCREEN_CARD) break;
        answer(true);
    }
}

static bool all_cmd(vocab_cmd_kind_t kind)
{
    return vocab_outbox_find(&F.all, kind, 0) >= 0;
}

// ---------------------------------------------------------------------------------------

static void test_home_to_summary(void)
{
    setup(1, false, false);
    CHECK(F.view.screen == VOCAB_SCREEN_HOME, "boot shows HOME");
    CHECK(strcmp(F.view.home.go.text, VOCAB_STR_HOME_GO) == 0, "go hint");
    CHECK(strcmp(F.view.status.progress.text, "Lv.1") == 0 && F.view.pet.visible && F.view.pet.idle,
          "HOME: level, pet idle");
    click(VOCAB_KEY_UP);
    send(VOCAB_EV_LONG, VOCAB_KEY_OK);
    send(VOCAB_EV_LONG, VOCAB_KEY_UP);
    CHECK(F.view.screen == VOCAB_SCREEN_HOME, "UP click / OK LONG / UP LONG ignored on HOME");
    start();
    CHECK(F.view.screen == VOCAB_SCREEN_CARD && F.s.card_count == 5, "OK starts 5 cards (AC-4)");
    CHECK(strcmp(F.view.status.progress.text, "1/5") == 0, "progress 1/5");
    CHECK(!all_cmd(VOCAB_CMD_PLAY_WORD) && F.view.anim.pulse_seq > 0,
          "no audio: WORD_PULSE instead of the clip (AC-34)");
    CHECK(strcmp(F.view.hint.text, VOCAB_STR_HINT_LOOK) == 0, "look-again hint (AC-34)");
    CHECK(vocab_record_seen(&F.records[F.s.cards[0].index]), "word marked seen when shown");
    for (int i = 0; i < 5; i++) answer(true);
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "SUMMARY after 5 cards");
    CHECK(vocab_session_tick(&F.s) == 11, "tick + 1 at SUMMARY (AC-10)");
    CHECK(vocab_session_xp_total(&F.s) == 10, "XP 5 + 5 first-try");
    char text[64];
    snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_SCORE_FMT, 5u, 5u);
    CHECK(strcmp(F.view.summary.score.text, text) == 0, "score text");
    snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_XP_FMT, 10u);
    CHECK(strcmp(F.view.summary.xp.text, text) == 0, "XP text");
    CHECK(F.view.badge == VOCAB_BADGE_DONE && !F.view.status.progress.visible, "SUMMARY status bar");
    CHECK(F.view.pet.visible && F.view.pet.on_summary && F.view.xpbar.on_summary, "pet + XP bar");
    const int stop = vocab_outbox_find(&F.all, VOCAB_CMD_STOP_AUDIO, 0);
    const int save = vocab_outbox_find(&F.all, VOCAB_CMD_SAVE, 0);
    CHECK(save >= 0 && stop >= 0 && stop < save, "stop audio, then save (AC-43)");
    for (vocab_index_t i = 0; i < 5; i++) {
        const vocab_record_t *r = &F.records[F.s.cards[i].index];
        CHECK(r->mastery == 1 && r->due == 11, "O1 for every word");
    }

    // SUMMARY: keys ignored for 800 ms (AC-4).
    CHECK(vocab_session_locked(&F.s), "summary locked");
    const vocab_event_t ok = vocab_event_click(VOCAB_KEY_OK);
    vocab_outbox_clear(&F.out);
    vocab_session_handle(&F.s, &ok, F.now + 100, &F.view, &F.out);
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "OK ignored during the summary lock");
    wait_ms(VOCAB_SUMMARY_LOCK_MS);
    CHECK(!vocab_session_locked(&F.s) && strcmp(F.view.hint.text, VOCAB_STR_HINT_SUMMARY) == 0,
          "summary hint after the lock");
    send(VOCAB_EV_LONG, VOCAB_KEY_OK);
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "OK LONG ignored on SUMMARY");
    click(VOCAB_KEY_DOWN);
    CHECK(F.view.screen == VOCAB_SCREEN_HOME, "DOWN on SUMMARY shows HOME");
    start();
    finish_session_cards();
    wait_ms(VOCAB_SUMMARY_LOCK_MS);
    click(VOCAB_KEY_OK);
    CHECK(F.view.screen == VOCAB_SCREEN_CARD && vocab_session_tick(&F.s) == 12,
          "SUMMARY + OK starts the next session (AC-4)");
}

static void test_pause(void)
{
    setup(2, false, false);
    start();
    answer(true);                        // card 1 answered: outcome kept later
    const vocab_index_t first = F.s.cards[0].index;
    settle();
    char word[VOCAB_VIEW_TEXT_CAP];
    snprintf(word, sizeof(word), "%s", F.view.card.word.text);
    send(VOCAB_EV_LONG, VOCAB_KEY_OK);
    CHECK(F.view.screen == VOCAB_SCREEN_PAUSE && F.view.badge == VOCAB_BADGE_PAUSE, "OK LONG pauses (AC-5)");
    CHECK(strcmp(F.view.card.banner.text, VOCAB_STR_PAUSE_BANNER) == 0 &&
              strcmp(F.view.panel[0].text, VOCAB_STR_PAUSE_CONTINUE) == 0 &&
              strcmp(F.view.panel[1].text, VOCAB_STR_PAUSE_HOME) == 0, "pause card texts");
    click(VOCAB_KEY_UP);
    CHECK(F.view.screen == VOCAB_SCREEN_CARD && strcmp(F.view.card.word.text, word) == 0,
          "UP resumes the same card");
    send(VOCAB_EV_LONG, VOCAB_KEY_OK);
    click(VOCAB_KEY_OK);
    CHECK(F.view.screen == VOCAB_SCREEN_CARD && strcmp(F.view.card.word.text, word) == 0,
          "OK resumes the same card");
    send(VOCAB_EV_LONG, VOCAB_KEY_OK);
    mark();
    click(VOCAB_KEY_DOWN);
    CHECK(F.view.screen == VOCAB_SCREEN_HOME, "DOWN leaves to HOME");
    CHECK(all_cmd(VOCAB_CMD_SAVE) && all_cmd(VOCAB_CMD_STOP_AUDIO), "pause -> HOME saves (AC-43)");
    CHECK(vocab_session_tick(&F.s) == 10, "abandoned session: no tick (AC-10)");
    CHECK(F.records[first].mastery == 1, "applied outcomes are kept (AC-5)");
}

static void test_dispatch(void)
{
    // AC-12: m = 1 -> Stage 2 word front.
    setup(3, true, true);
    all_due(1);
    init_session(10, 0, true, 0);
    start();
    CHECK(F.s.stage_format == VOCAB_FORMAT_S2 && F.view.card.word.visible && !F.view.card.sentence.visible,
          "m=1: Stage 2 word front");

    // m = 2, example_cloze and audio: cloze-sentence front for words with a cloze sentence.
    setup(4, true, true);
    all_due(2);
    init_session(10, 0, true, 0);
    start();
    const vocab_entry_t *e = entry();
    CHECK(F.s.stage_format == VOCAB_FORMAT_S2, "m=2: Stage 2");
    CHECK(F.view.card.sentence.visible == vocab_entry_has_example_cloze(e), "m=2 with audio: cloze front");

    // m = 2 muted: never the cloze front (the clip identifies the word).
    setup(5, true, true);
    all_due(2);
    init_session(10, 0, true, 2);
    start();
    CHECK(!F.view.card.sentence.visible && F.view.card.word.visible, "m=2 muted: word front (AC-12)");

    // m = 3: Stage 3 with cloze data, Stage 2 otherwise.
    setup(6, true, true);
    all_due(3);
    init_session(10, 0, true, 0);
    start();
    for (int i = 0; i < 5; i++) {
        settle();
        if (F.s.screen != VOCAB_SCREEN_CARD) break;
        const vocab_format_t want = vocab_entry_has_cloze(entry()) ? VOCAB_FORMAT_S3 : VOCAB_FORMAT_S2;
        CHECK(F.s.stage_format == want, "m=3 dispatch (card %d)", i);
        answer(true);
    }

    // AC-8: a 1-entry bank has no lure: Stage 2.
    static const vocab_entry_t one[1] = {
        { .uid = 7, .category = 1, .text = "cat", .zh = "zh", .example = "The cat sat." },
    };
    const vocab_bank_t one_bank = { one, 1 };
    setup_bank(&one_bank, 7, false, false);
    start();
    CHECK(F.s.card_count == 1 && F.s.stage_format == VOCAB_FORMAT_S2, "1-entry bank -> Stage 2 (AC-8)");
    answer(true);
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "1-entry session completes");
    char text[64];
    snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_LEARNED_FMT, 1u);
    CHECK(strcmp(F.view.summary.score.text, text) == 0, "no objective card: learned text");

    // AC-14: a single-category bank still serves Stage 1 (same-category lure fallback) ...
    static const vocab_entry_t same_cat[3] = {
        { .uid = 1, .category = 4, .text = "pen", .zh = "ma", .example = "A red pen." },
        { .uid = 2, .category = 4, .text = "book", .zh = "mb", .example = "A big book." },
        { .uid = 3, .category = 4, .text = "ruler", .zh = "mc", .example = "A long ruler." },
    };
    const vocab_bank_t same_bank = { same_cat, 3 };
    setup_bank(&same_bank, 11, false, false);
    start();
    CHECK(F.s.card_count == 3, "3-entry bank: 3 cards");
    for (int i = 0; i < 3 && F.view.screen != VOCAB_SCREEN_SUMMARY; i++) {
        settle();
        CHECK(F.s.stage_format == VOCAB_FORMAT_S1 && F.view.panel[0].visible && F.view.panel[1].visible,
              "single-category bank: Stage 1 with a same-category lure (AC-14, card %d)", i);
        answer(true);
    }
    // ... and with no acceptable lure at all (one meaning contains the other) every card is
    // served as Stage 2.
    static const vocab_entry_t nested[2] = {
        { .uid = 1, .category = 7, .text = "ice", .zh = "ma", .example = "I like ice." },
        { .uid = 2, .category = 8, .text = "ice cream", .zh = "mab", .example = "Eat ice cream." },
    };
    const vocab_bank_t nested_bank = { nested, 2 };
    setup_bank(&nested_bank, 12, false, false);
    start();
    CHECK(F.s.card_count == 2 && F.view.screen == VOCAB_SCREEN_CARD, "2-entry bank: 2 cards");
    for (int i = 0; i < 2 && F.view.screen != VOCAB_SCREEN_SUMMARY; i++) {
        settle();
        CHECK(F.s.stage_format == VOCAB_FORMAT_S2, "no valid lure: Stage 2 (AC-14, card %d)", i);
        answer(true);
    }
    settle();
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "no-lure session completes");

    // A stage that refuses unusable input (a meaning too long for its label) skips the card
    // and leaves the word unseen (vocab_stage.h 1).
    static char long_zh[VOCAB_VIEW_TEXT_CAP + 8];
    memset(long_zh, 'z', sizeof(long_zh) - 1);
    long_zh[sizeof(long_zh) - 1] = '\0';
    static vocab_entry_t refused[2] = {
        { .uid = 1, .category = 1, .text = "cat", .zh = "ma", .example = "A cat." },
        { .uid = 2, .category = 2, .text = "dog", .zh = "mb", .example = "A dog." },
    };
    refused[0].zh = long_zh;
    const vocab_bank_t refused_bank = { refused, 2 };
    setup_bank(&refused_bank, 13, false, false);
    start();
    settle();
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY && F.s.card_count == 2,
          "refused cards are skipped (both Stage 1 cards involve the unusable meaning)");
    CHECK(!vocab_record_seen(&F.records[0]) && !vocab_record_seen(&F.records[1]) &&
              F.records[0].mastery == 0 && F.records[1].mastery == 0,
          "a refused card leaves its word unseen and unchanged");
}

static void test_forgot_inserts_test_later(void)
{
    setup(8, false, false);
    all_due(1);
    init_session(10, 0, false, 0);
    start();
    const vocab_index_t word = card()->index;
    answer(false);  // forgot
    CHECK(F.s.card_count == 6, "TEST_LATER inserted (N grows)");
    CHECK(F.s.cards[3].purpose == VOCAB_PURPOSE_TEST_LATER && F.s.cards[3].index == word,
          "two cards later (AC-18)");
    CHECK(F.records[word].mastery == 1 && F.records[word].due == 11 && F.records[word].lapses == 1, "O4");
    answer(true);
    answer(true);
    settle();
    CHECK(F.s.cur == 3 && F.s.stage_format == VOCAB_FORMAT_S1 && F.view.badge == VOCAB_BADGE_TEST_AGAIN,
          "TEST_LATER card in Stage 1");
    char progress[16];
    snprintf(progress, sizeof(progress), "4/6");
    CHECK(strcmp(F.view.status.progress.text, progress) == 0, "progress counts inserted cards");
    answer(false);  // O6
    CHECK(F.records[word].mastery == 0 && F.records[word].lapses == 2, "O6 on the TEST_LATER card");
    finish_session_cards();
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "session with an insertion completes");
}

static void test_spot_check(bool pass)
{
    setup(pass ? 9 : 10, false, false);
    all_due(1);
    init_session(10, 0, false, 0);
    start();
    vocab_index_t claimed[3];
    vocab_tick_t pre_due[3];
    for (int i = 0; i < 3; i++) {
        settle();
        claimed[i] = card()->index;
        pre_due[i] = F.records[claimed[i]].due;
        answer(true);  // I know
        if (i < 2) CHECK(F.s.card_count == 5, "no check before the third claim");
    }
    CHECK(F.s.card_count == 6 && F.s.cards[3].purpose == VOCAB_PURPOSE_SPOT_CHECK, "spot check next (AC-19)");
    const vocab_index_t target = F.s.cards[3].index;
    CHECK(target == claimed[0] || target == claimed[1], "target is one of the first two claims (AC-19)");
    CHECK(F.records[claimed[2]].mastery == 2, "claims promoted (O3)");
    CHECK(F.view.screen == VOCAB_SCREEN_BANNER && F.view.badge == VOCAB_BADGE_SPOT_CHECK &&
              strcmp(F.view.card.banner.text, VOCAB_STR_SPOT_BANNER) == 0 && vocab_session_locked(&F.s),
          "800 ms intro banner");
    settle();
    CHECK(F.s.screen == VOCAB_SCREEN_CARD && F.s.stage_format == VOCAB_FORMAT_S1 &&
              F.view.badge == VOCAB_BADGE_SPOT_CHECK, "spot check in Stage 1 (pre-claim m < 2)");
    answer(pass);
    if (pass) {
        for (int i = 0; i < 3; i++) CHECK(F.records[claimed[i]].mastery == 2, "O10 claims stand");
    } else {
        for (int i = 0; i < 3; i++) {
            const vocab_record_t *r = &F.records[claimed[i]];
            if (claimed[i] == target) {
                CHECK(r->mastery == 1 && r->due == 11 && r->lapses == 1, "O11 target demoted");
            } else {
                CHECK(r->mastery == 1 && r->due <= 11 && r->due == (pre_due[i] < 11 ? pre_due[i] : 11),
                      "O11 other claims restored (AC-20)");
            }
        }
    }
    CHECK(F.s.streak.count == 0, "any spot check resets the streak (AC-20)");
    finish_session_cards();
}

static void test_streak_reset(void)
{
    // Forgot resets the streak; Stage 1 cards in between do not (AC-20).
    setup(11, false, false);
    all_due(1);
    F.records[0].mastery = 0;
    F.records[0].flags = 0;  // one new word (Stage 1) in the session
    init_session(10, 0, false, 0);
    start();
    int claims = 0;
    bool forgot_done = false;
    while (F.s.screen == VOCAB_SCREEN_CARD || F.s.screen == VOCAB_SCREEN_BANNER) {
        settle();
        if (F.s.screen != VOCAB_SCREEN_CARD) break;
        if (F.s.stage_format == VOCAB_FORMAT_S2 && claims == 1 && !forgot_done) {
            answer(false);
            forgot_done = true;
            CHECK(F.s.streak.count == 0, "forgot clears the streak");
            claims = 0;
            continue;
        }
        const bool s2 = F.s.stage_format == VOCAB_FORMAT_S2;
        answer(true);
        if (s2) claims++;
        if (!s2) CHECK(F.s.streak.count == (uint8_t)claims, "Stage 1 card keeps the streak");
    }
}

static void test_too_easy(void)
{
    setup(12, false, false);
    start();  // fresh words: Stage 1 LEARN cards
    settle();
    const vocab_index_t word = card()->index;
    send(VOCAB_EV_LONG, VOCAB_KEY_UP);
    CHECK(F.view.screen == VOCAB_SCREEN_BANNER && strcmp(F.view.card.banner.text, VOCAB_STR_TOO_EASY_ACK) == 0,
          "too-easy acknowledgement (AC-21)");
    CHECK(F.s.card_count == 6 && F.s.cards[3].purpose == VOCAB_PURPOSE_TOO_EASY_CHECK &&
              F.s.cards[3].index == word, "check inserted two cards later (AC-21)");
    CHECK(vocab_record_seen(&F.records[word]) && F.records[word].mastery == 0, "O12: seen, unchanged");
    settle();
    CHECK(F.s.screen == VOCAB_SCREEN_CARD && F.s.cur == 1, "next card after the ack");
    // UP LONG on a card that already spawned / inserted cards is ignored (AC-22).
    answer(true);
    answer(true);
    CHECK(F.view.screen == VOCAB_SCREEN_BANNER && F.view.badge == VOCAB_BADGE_TOO_EASY &&
              strcmp(F.view.card.banner.text, VOCAB_STR_TOO_EASY_BANNER) == 0, "too-easy intro banner");
    settle();
    const vocab_format_t want = vocab_entry_has_cloze(vocab_bank_at(BANK, word)) ? VOCAB_FORMAT_S3
                                                                                 : VOCAB_FORMAT_S1;
    CHECK(F.s.stage_format == want, "check format: Stage 3 with cloze, else Stage 1 (AC-21)");
    send(VOCAB_EV_LONG, VOCAB_KEY_UP);
    CHECK(F.s.screen == VOCAB_SCREEN_CARD && F.s.cur == 3, "UP LONG ignored on inserted cards (AC-22)");
    answer(true);
    CHECK(F.records[word].mastery == 5 && F.records[word].due == 10 + VOCAB_TOO_EASY_INTERVAL,
          "O13: mastered, due T+30");
    finish_session_cards();
    CHECK(vocab_session_xp_total(&F.s) == 5 + 5 + 2, "XP includes the new-mastery bonus");
}

static void test_volume_and_mute(void)
{
    setup(13, true, true);
    CHECK(F.view.status.volume == VOCAB_VOL_ICON_HIGH, "80 %% at boot");
    start();
    CHECK(all_cmd(VOCAB_CMD_PLAY_WORD), "word clip with audio");
    settle();
    // Volume works during a lock (AC-33): make a lock by answering wrong.
    mark();
    send(VOCAB_EV_LONG, VOCAB_KEY_DOWN);
    int i = vocab_outbox_find(&F.all, VOCAB_CMD_SET_VOLUME, 0);
    CHECK(i >= 0 && F.all.cmd[i].value == 30u && strcmp(F.view.hint.text, VOCAB_STR_TOAST_VOL_LOW) == 0,
          "DOWN LONG -> 30 %% with toast");
    CHECK(F.s.screen == VOCAB_SCREEN_CARD && F.s.cur == 0 && !F.s.outcome_seen,
          "DOWN LONG never selects the DOWN option (AC-33)");
    mark();
    send(VOCAB_EV_LONG, VOCAB_KEY_DOWN);
    CHECK(F.view.status.volume == VOCAB_VOL_ICON_MUTE && all_cmd(VOCAB_CMD_STOP_AUDIO) &&
              !all_cmd(VOCAB_CMD_PLAY_CUE), "mute: stop, no tick");
    mark();
    const uint8_t pulses = F.view.anim.pulse_seq;
    click(VOCAB_KEY_OK);
    CHECK(!all_cmd(VOCAB_CMD_PLAY_WORD) && F.view.anim.pulse_seq != pulses &&
              strcmp(F.view.hint.text, VOCAB_STR_TOAST_MUTE) == 0, "muted replay -> WORD_PULSE (AC-34)");
    wait_ms(VOCAB_TOAST_MS);
    CHECK(strcmp(F.view.hint.text, VOCAB_STR_HINT_LOOK) == 0, "muted hint: look again (AC-34)");
    mark();
    send(VOCAB_EV_LONG, VOCAB_KEY_DOWN);
    i = vocab_outbox_find(&F.all, VOCAB_CMD_SET_VOLUME, 0);
    CHECK(i >= 0 && F.all.cmd[i].value == 80u && !all_cmd(VOCAB_CMD_SAVE), "back to 80 %%, no save (AC-43)");

    // Missing clip with audio on: pulse, cues still play.
    setup(14, true, false);
    start();
    CHECK(!all_cmd(VOCAB_CMD_PLAY_WORD) && F.view.anim.pulse_seq > 0, "missing clip -> WORD_PULSE (AC-34)");

    // Failed audio init: dim mute icon, toast, no volume command (AC-34).
    setup(15, false, false);
    CHECK(F.view.status.volume == VOCAB_VOL_ICON_NO_AUDIO, "dim mute icon");
    mark();
    send(VOCAB_EV_LONG, VOCAB_KEY_DOWN);
    CHECK(!all_cmd(VOCAB_CMD_SET_VOLUME) && strcmp(F.view.hint.text, VOCAB_STR_TOAST_NO_AUDIO) == 0,
          "audio unavailable toast");
}

static void test_battery(void)
{
    setup(16, true, true);
    const struct {
        int pct;
        vocab_batt_icon_t icon;
        bool red;
        bool warn;
    } rows[] = {
        { 100, VOCAB_BATT_ICON_FULL, false, false }, { 80, VOCAB_BATT_ICON_FULL, false, false },
        { 79, VOCAB_BATT_ICON_3, false, false },     { 55, VOCAB_BATT_ICON_3, false, false },
        { 54, VOCAB_BATT_ICON_2, false, false },     { 30, VOCAB_BATT_ICON_2, false, false },
        { 29, VOCAB_BATT_ICON_1, false, false },     { 16, VOCAB_BATT_ICON_1, false, false },
        { 15, VOCAB_BATT_ICON_1, true, false },      { 11, VOCAB_BATT_ICON_1, true, false },
        { 10, VOCAB_BATT_ICON_1, true, true },       { 9, VOCAB_BATT_ICON_EMPTY, true, true },
        { 0, VOCAB_BATT_ICON_EMPTY, true, true },    { -1, VOCAB_BATT_ICON_UNKNOWN, false, false },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        vocab_session_set_battery(&F.s, rows[i].pct, &F.view);
        CHECK(F.view.status.battery == rows[i].icon && F.view.status.battery_low == rows[i].red,
              "battery %d icon/red (AC-50)", rows[i].pct);
        CHECK(F.view.home.warn.visible == rows[i].warn, "battery %d HOME warning (AC-50)", rows[i].pct);
    }
    CHECK(strcmp(F.view.status.batt_pct.text, VOCAB_STR_BATT_PCT_UNKNOWN) == 0, "unknown -> --%%");
    vocab_session_set_battery(&F.s, 87, &F.view);
    CHECK(strcmp(F.view.status.batt_pct.text, "87%") == 0, "percentage on HOME");
    vocab_session_set_storage_warning(&F.s, true, &F.view);
    vocab_session_set_battery(&F.s, 5, &F.view);
    CHECK(strcmp(F.view.home.warn.text, VOCAB_STR_WARN_SAVE) == 0, "storage warning has priority");
    start();
    CHECK(!F.view.status.batt_pct.visible, "sessions show the icon only");
    mark();
    answer(true);
    int i = vocab_outbox_find(&F.all, VOCAB_CMD_SAVE, 0);
    CHECK(i >= 0 && F.all.cmd[i].value == VOCAB_SAVE_LOW_BATTERY, "<= 5 %%: save at card boundaries");
}

static void test_bank_error(void)
{
    static const vocab_entry_t dup[2] = {
        { .uid = 5, .text = "a", .zh = "x", .example = "a b" },
        { .uid = 5, .text = "b", .zh = "y", .example = "b c" },
    };
    const vocab_bank_t dup_bank = { dup, 2 };
    setup_bank(&dup_bank, 17, false, false);
    CHECK(F.view.screen == VOCAB_SCREEN_BANK_ERROR, "duplicate uids -> BANK_ERROR");
    const vocab_bank_t empty = { NULL, 0 };
    setup_bank(&empty, 17, false, false);
    CHECK(F.view.screen == VOCAB_SCREEN_BANK_ERROR &&
              strcmp(F.view.card.banner.text, VOCAB_STR_BANK_ERROR_BANNER) == 0, "empty bank");
    click(VOCAB_KEY_OK);
    CHECK(F.view.screen == VOCAB_SCREEN_BANK_ERROR, "no way out of BANK_ERROR");
}

static void test_queue_bound(void)
{
    // AC-9: insertions never exceed 10 cards; at most one inserted card per word.
    for (uint32_t seed = 100; seed < 160; seed++) {
        setup(seed, false, false);
        all_due((uint8_t)(seed % 3));
        for (vocab_index_t i = 0; i < BANK->count; i += 2) F.records[i].mastery = 0;
        init_session(10, 0, false, 0);
        start();
        vocab_rng_t pick;
        vocab_rng_seed(&pick, seed);
        for (int steps = 0; steps < 40 && F.s.screen != VOCAB_SCREEN_SUMMARY; steps++) {
            settle();
            if (F.s.screen != VOCAB_SCREEN_CARD) continue;
            if (vocab_rng_below(&pick, 4) == 0 && F.s.cards[F.s.cur].purpose == VOCAB_PURPOSE_LEARN) {
                send(VOCAB_EV_LONG, VOCAB_KEY_UP);
                continue;
            }
            answer(vocab_rng_bit(&pick));
            CHECK(F.s.card_count <= VOCAB_MAX_CARDS, "queue bound");
        }
        CHECK(F.s.screen == VOCAB_SCREEN_SUMMARY, "seed %u completes", (unsigned)seed);
        for (uint8_t a = 0; a < F.s.card_count; a++) {
            if (F.s.cards[a].purpose == VOCAB_PURPOSE_LEARN) continue;
            for (uint8_t b = (uint8_t)(a + 1); b < F.s.card_count; b++) {
                CHECK(F.s.cards[b].purpose == VOCAB_PURPOSE_LEARN || F.s.cards[b].index != F.s.cards[a].index,
                      "one inserted card per word (seed %u)", (unsigned)seed);
            }
        }
        for (vocab_index_t i = 0; i < BANK->count; i++) CHECK(F.records[i].mastery <= 5, "m in 0..5");
    }
}

// Review / QA fix round: the HOME toast replaces the start line (QA-1), "n/N" counts an
// inserted card in the same update (QA-3), SUMMARY -> HOME stops playback (QA-2).
static void test_fix_round(void)
{
    setup(41, true, true);
    send(VOCAB_EV_LONG, VOCAB_KEY_DOWN);
    CHECK(F.view.hint.toast != VOCAB_TOAST_NONE && F.view.hint.text[0] != '\0' && !F.view.home.go.visible,
          "HOME: the volume toast replaces the start line (QA-1)");
    wait_ms(VOCAB_TOAST_MS);
    CHECK(F.view.hint.toast == VOCAB_TOAST_NONE && F.view.home.go.visible &&
              strcmp(F.view.home.go.text, VOCAB_STR_HOME_GO) == 0, "the start line returns after the toast");

    setup(42, false, false);
    all_due(1);
    init_session(10, 0, false, 0);
    start();
    settle();
    CHECK(F.s.stage_format == VOCAB_FORMAT_S2 && strcmp(F.view.status.progress.text, "1/5") == 0,
          "Stage 2 card 1/5");
    flip();
    click(VOCAB_KEY_DOWN);  // forgot: TEST_LATER inserted
    CHECK(F.s.card_count == 6 && vocab_session_locked(&F.s) && strcmp(F.view.status.progress.text, "1/6") == 0,
          "n/N counts the inserted card during the acknowledgement (QA-3)");
    finish_session_cards();
    CHECK(F.view.screen == VOCAB_SCREEN_SUMMARY, "session with the insertion completes");
    settle();
    mark();
    click(VOCAB_KEY_UP);
    CHECK(F.view.screen == VOCAB_SCREEN_HOME && all_cmd(VOCAB_CMD_STOP_AUDIO),
          "SUMMARY -> HOME stops playback (QA-2, spec 8.3)");
}

// Timers fire at their deadline: an explicit early timer event is ignored (Stage 1 request).
static void test_timer_event_deadline(void)
{
    setup(31, true, true);
    send(VOCAB_EV_LONG, VOCAB_KEY_DOWN);  // volume toast with its own timer
    CHECK(F.view.hint.toast != VOCAB_TOAST_NONE, "volume toast shown");
    const vocab_ms_t shown = F.now;
    const vocab_event_t ev = { VOCAB_EV_TIMER, VOCAB_KEY_OK, VOCAB_TIMER_TOAST };
    vocab_outbox_clear(&F.out);
    vocab_session_handle(&F.s, &ev, shown + 1, &F.view, &F.out);
    CHECK(F.view.hint.toast != VOCAB_TOAST_NONE, "an early timer event is ignored");
    vocab_session_handle(&F.s, &ev, shown + VOCAB_TOAST_MS, &F.view, &F.out);
    CHECK(F.view.hint.toast == VOCAB_TOAST_NONE, "a due timer event fires");
    vocab_outbox_clear(&F.out);
    vocab_session_handle(&F.s, &ev, shown + 5000, &F.view, &F.out);
    CHECK(F.view.hint.toast == VOCAB_TOAST_NONE && F.out.count == 0, "a stale timer event does nothing");
}

int main(void)
{
    test_home_to_summary();
    test_pause();
    test_dispatch();
    test_forgot_inserts_test_later();
    test_spot_check(true);
    test_spot_check(false);
    test_streak_reset();
    test_too_easy();
    test_volume_and_mute();
    test_battery();
    test_bank_error();
    test_queue_bound();
    test_timer_event_deadline();
    test_fix_round();
    return vocab_test_report("vocab session tests");
}
