// tests/test_vocab_qa_edges.c - QA system edge cases through raw keys.
//
// - AC-56 / spec 4.9, 11.5: HOME level, title, XP line, XP bar and pet stage for XP values
//   around every level threshold, Lv10 ("full level") and XP saturation; level-up SUMMARY
//   (CUE_LEVEL_UP instead of CUE_CHEER, level-up line, pet level_up);
// - spec 4.2: the session tick and every due saturate at 65535 without breaking sessions;
// - AC-8 at system level: a 3-entry and a 1-entry bank (no clips) run whole sessions and
//   persist; AC-12: the cloze-sentence front only with playable word audio;
// - spec 11 / 3.2 BANK_ERROR: an invalid bank (duplicate uid, more than VOCAB_RECORDS_MAX
//   entries) never leaves the error screen, keeps the volume key, sleeps and wakes safely;
// - spec 8.3: leaving SUMMARY for HOME stops playback (former defect QA-2, regression check).
//
// Owner: QA engineer.
#include "test_vocab_qa_harness.h"

static vocab_record_t R[VOCAB_RECORDS_MAX];

static void wait_for_screen(vocab_screen_t screen, uint32_t max_ms)
{
    for (uint32_t t = 0; t < max_ms && D.view.screen != screen; t += 10) qa_wait(10);
}

static void answer_ok(void)
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
    qa_settle();
}

static void finish_session(void)
{
    for (int i = 0; i < 20 && D.view.screen != VOCAB_SCREEN_SUMMARY; i++) answer_ok();
    wait_for_screen(VOCAB_SCREEN_SUMMARY, 3000);
}

// ---------------------------------------------------------------------------------------
// AC-56: levels, XP line, pet
// ---------------------------------------------------------------------------------------

static void test_home_levels(void)
{
    static const uint32_t xps[] = { 0, 29, 30, 45, 89, 90, 179, 180, 299, 300, 449, 450, 629, 630,
                                    839, 840, 1079, 1080, 1349, 1350, 1520, 4294967290u };
    static const uint32_t mins[10] = { 0, 30, 90, 180, 300, 450, 630, 840, 1080, 1350 };
    static const char *const titles[10] = VOCAB_STR_TITLES_INIT;
    static const vocab_pet_stage_t pets[10] = {
        VOCAB_PET_EGG, VOCAB_PET_HATCHLING, VOCAB_PET_HATCHLING, VOCAB_PET_CHILD, VOCAB_PET_CHILD,
        VOCAB_PET_CHILD, VOCAB_PET_GROWN, VOCAB_PET_GROWN, VOCAB_PET_GROWN, VOCAB_PET_CROWN,
    };
    for (size_t k = 0; k < sizeof(xps) / sizeof(xps[0]); k++) {
        qa_power_on_fresh(60);
        qa_fresh_records(R);
        qa_seed_storage(R, 0, xps[k], 0);
        qa_power_cycle(500);
        const uint32_t xp = xps[k];
        unsigned level = 1;
        for (unsigned i = 1; i < 10; i++) {
            if (xp >= mins[i]) level = i + 1;
        }
        char want[64];
        snprintf(want, sizeof(want), "Lv.%u", level);
        CHECK(strcmp(D.view.status.progress.text, want) == 0, "XP %u: '%s' (want %s)", (unsigned)xp,
              D.view.status.progress.text, want);
        CHECK(strcmp(D.view.home.title.text, titles[level - 1]) == 0, "XP %u: title", (unsigned)xp);
        CHECK(D.view.pet.stage == pets[level - 1], "XP %u: pet stage %d", (unsigned)xp, (int)D.view.pet.stage);
        if (level == 10) {
            snprintf(want, sizeof(want), VOCAB_STR_XP_MAX_FMT, (unsigned)xp);
            CHECK(D.view.xpbar.value == D.view.xpbar.max, "Lv10: full XP bar");
        } else {
            snprintf(want, sizeof(want), VOCAB_STR_XP_FMT, (unsigned)xp, (unsigned)mins[level]);
            CHECK(D.view.xpbar.max == mins[level] - mins[level - 1] && D.view.xpbar.value == xp - mins[level - 1],
                  "XP %u: bar %u/%u", (unsigned)xp, (unsigned)D.view.xpbar.value, (unsigned)D.view.xpbar.max);
        }
        CHECK(strcmp(D.view.home.xp.text, want) == 0, "XP %u: '%s' (want '%s')", (unsigned)xp, D.view.home.xp.text,
              want);
    }
    // XP saturates (uint32) at the next SUMMARY.
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    finish_session();
    CHECK(vocab_engine_persist(&D.eng).xp == UINT32_MAX, "XP saturates at 4294967295 (spec 4.9)");
    qa_wait(VOCAB_SUMMARY_LOCK_MS + 200);
    qa_tap(VOCAB_KEY_UP, 80);
    char want[64];
    snprintf(want, sizeof(want), VOCAB_STR_XP_MAX_FMT, (unsigned)UINT32_MAX);
    CHECK(strcmp(D.view.home.xp.text, want) == 0, "HOME after saturation: '%s'", D.view.home.xp.text);
}

static void test_level_up_summary(void)
{
    qa_power_on_fresh(61);
    qa_fresh_records(R);
    qa_seed_storage(R, 3, 25, 0);
    qa_power_cycle(500);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    qa_mark();
    finish_session();
    const int lvl = qa_find(VOCAB_CMD_PLAY_CUE, 0);
    int cue = -1;
    for (int i = 0; i < (int)D.log_len; i++) {
        if (D.log[i].kind == VOCAB_CMD_PLAY_CUE && (D.log[i].cue == VOCAB_CUE_LEVEL_UP || D.log[i].cue == VOCAB_CUE_CHEER)) {
            cue = i;
        }
    }
    (void)lvl;
    static const char *const titles[10] = VOCAB_STR_TITLES_INIT;
    char want[64];
    snprintf(want, sizeof(want), VOCAB_STR_SUMMARY_LEVELUP_FMT, titles[1]);
    CHECK(vocab_engine_persist(&D.eng).xp == 35 && D.view.summary.levelup.visible &&
              strcmp(D.view.summary.levelup.text, want) == 0 && D.view.pet.level_up && D.view.pet.on_summary,
          "Lv1 -> Lv2 at SUMMARY: level-up line and pet level-up (AC-56)");
    CHECK(cue >= 0 && D.log[cue].cue == VOCAB_CUE_LEVEL_UP, "CUE_LEVEL_UP replaces CUE_CHEER (spec 8.2)");
    qa_wait(VOCAB_SUMMARY_LOCK_MS + 200);
    qa_tap(VOCAB_KEY_DOWN, 80);
    CHECK(strcmp(D.view.status.progress.text, "Lv.2") == 0 && D.view.pet.stage == VOCAB_PET_HATCHLING,
          "HOME shows Lv.2 and the hatchling");
}

// ---------------------------------------------------------------------------------------
// Spec 4.2: tick saturation
// ---------------------------------------------------------------------------------------

static void test_tick_saturation(void)
{
    qa_power_on_fresh(62);
    qa_fresh_records(R);
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        R[i].flags = VOCAB_REC_SEEN;
        R[i].mastery = (uint8_t)(1 + i % 5);
        R[i].due = (vocab_tick_t)(65520u + i % 15u);
    }
    qa_seed_storage(R, 65533u, 500, 0);
    qa_power_cycle(500);
    for (int s = 0; s < 4; s++) {
        qa_wait(300);
        qa_tap(VOCAB_KEY_OK, 80);
        CHECK(D.view.screen == VOCAB_SCREEN_CARD && qa_s()->card_count >= 5, "session at tick %u",
              (unsigned)vocab_engine_persist(&D.eng).tick);
        finish_session();
        qa_wait(VOCAB_SUMMARY_LOCK_MS + 200);
        qa_tap(VOCAB_KEY_UP, 80);
    }
    CHECK(vocab_engine_persist(&D.eng).tick == 65535u, "tick saturates at 65535 (spec 4.2)");
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        CHECK(D.records[i].due >= 65520u, "due arithmetic saturates instead of wrapping (word %u due %u)",
              (unsigned)D.records[i].uid, (unsigned)D.records[i].due);
    }
    qa_power_cycle(500);
    CHECK(vocab_engine_persist(&D.eng).tick == 65535u, "saturated tick persists");
}

// ---------------------------------------------------------------------------------------
// AC-8: small banks at system level
// ---------------------------------------------------------------------------------------

static void test_small_banks(void)
{
    static const vocab_entry_t three[3] = {
        { .uid = 9001, .category = 1, .text = "kiwi", .ipa = "/ki/", .zh = "\xe7\x8c\xab", .example = "A kiwi." },
        { .uid = 9002, .category = 2, .text = "fox", .zh = "\xe7\x8b\x97", .example = "A fox." },
        { .uid = 9003, .category = 3, .text = "van", .zh = "\xe9\xb1\xbc", .example = "A van." },
    };
    static const vocab_bank_t three_bank = { three, 3 };
    qa_power_on_fresh(63);
    D.bank = &three_bank;
    D.clips = false;  // a custom bank has no clip pack: every word pulses
    qa_kv_wipe();
    D.saved_valid = false;
    qa_power_cycle(500);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_s()->card_count == 3 && qa_s()->stage_format == VOCAB_FORMAT_S1 &&
              strcmp(D.view.hint.text, VOCAB_STR_HINT_LOOK) == 0, "3-entry bank: 3 Stage 1 cards, no clips (AC-8)");
    finish_session();
    char want[48];
    snprintf(want, sizeof(want), VOCAB_STR_SUMMARY_SCORE_FMT, 3u, 3u);
    CHECK(strcmp(D.view.summary.score.text, want) == 0 && vocab_engine_persist(&D.eng).tick == 1, "3-card SUMMARY");
    qa_power_cycle(500);  // round trip checked by the harness
    CHECK(qa_count_seen() == 3, "small bank progress persists");
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_s()->card_count == 3 && qa_s()->stage_format == VOCAB_FORMAT_S2, "second session: Stage 2 reviews");
    finish_session();

    static const vocab_entry_t one[1] = {
        { .uid = 9100, .category = 4, .text = "owl", .zh = "\xe9\xb8\x9f", .example = "An owl." },
    };
    static const vocab_bank_t one_bank = { one, 1 };
    qa_power_on_fresh(64);
    D.bank = &one_bank;
    D.clips = false;
    qa_kv_wipe();
    D.saved_valid = false;
    qa_power_cycle(500);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(qa_s()->card_count == 1 && qa_s()->stage_format == VOCAB_FORMAT_S2, "1-entry bank: Stage 2 (AC-8)");
    qa_wait(300);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_CARD, "too easy ignored when no check can be served (no lure, no cloze)");
    finish_session();
    snprintf(want, sizeof(want), VOCAB_STR_SUMMARY_LEARNED_FMT, 1u);
    CHECK(strcmp(D.view.summary.score.text, want) == 0, "no objective card: 'learned 1 word'");
    D.bank = vocab_bank_builtin();
}

// ---------------------------------------------------------------------------------------
// AC-12: cloze-sentence front only with playable word audio
// ---------------------------------------------------------------------------------------

static void cloze_front_case(bool audio_ok, uint8_t volume_index, bool missing, bool expect)
{
    qa_power_on_fresh(65);
    D.audio_ok = audio_ok;
    qa_fresh_records(R);
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        R[i].flags = VOCAB_REC_SEEN;
        R[i].mastery = 2;
        R[i].due = 3;
    }
    qa_seed_storage(R, 10, 0, volume_index);
    qa_power_cycle(500);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    if (missing) {
        // The current word's clip disappears before the next card is drawn: check card 2.
        const vocab_index_t next = qa_s()->cards[1].index;
        D.missing_uid = D.bank->entries[next].uid;
        answer_ok();
    }
    const vocab_entry_t *e = qa_entry();
    CHECK(qa_s()->stage_format == VOCAB_FORMAT_S2, "m=2: Stage 2");
    if (vocab_entry_has_example_cloze(e)) {
        CHECK(D.view.card.sentence.visible == expect && D.view.card.word.visible == !expect,
              "'%s' cloze front %d (audio %d, volume %u, clip missing %d) (AC-12)", e->text,
              (int)D.view.card.sentence.visible, (int)audio_ok, (unsigned)volume_index, (int)missing);
        if (expect) CHECK(strcmp(D.view.card.prompt.text, VOCAB_STR_S2_FRONT_CLOZE) == 0, "cloze-front prompt");
    }
    D.missing_uid = 0;
}

static void test_cloze_front_rule(void)
{
    cloze_front_case(true, 0, false, true);
    cloze_front_case(true, 1, false, true);
    cloze_front_case(true, 2, false, false);
    cloze_front_case(false, 0, false, false);
    cloze_front_case(true, 0, true, false);
}

// ---------------------------------------------------------------------------------------
// BANK_ERROR
// ---------------------------------------------------------------------------------------

static void bank_error_case(const vocab_bank_t *bank, const char *what)
{
    qa_power_on_fresh(66);
    D.bank = bank;
    qa_kv_wipe();
    D.saved_valid = false;
    qa_power_cycle(500);
    CHECK(D.view.screen == VOCAB_SCREEN_BANK_ERROR && strcmp(D.view.card.banner.text, VOCAB_STR_BANK_ERROR_BANNER) == 0 &&
              strcmp(D.view.card.prompt.text, VOCAB_STR_BANK_ERROR_PROMPT) == 0 && !D.view.panel[0].visible,
          "%s: BANK_ERROR screen", what);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    qa_tap(VOCAB_KEY_UP, 80);
    qa_tap(VOCAB_KEY_DOWN, 80);
    qa_hold(VOCAB_KEY_OK, 700);
    qa_hold(VOCAB_KEY_UP, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_BANK_ERROR, "%s: no key leaves BANK_ERROR", what);
    const uint8_t vol = qa_volume_index();
    qa_wait(300);
    qa_hold(VOCAB_KEY_DOWN, 700);
    CHECK(qa_volume_index() != vol && strcmp(D.view.hint.text, VOCAB_STR_TOAST_VOL_LOW) == 0,
          "%s: DOWN LONG = volume (spec 3.2)", what);
    qa_wait(VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + 500);
    CHECK(D.sleeps == 1 && D.view.screen == VOCAB_SCREEN_BANK_ERROR, "%s: sleeps and wakes into BANK_ERROR", what);
    D.bank = vocab_bank_builtin();
}

static void test_bank_error(void)
{
    static vocab_entry_t dup[6];
    for (int i = 0; i < 6; i++) dup[i] = vocab_bank_builtin()->entries[i];
    dup[5].uid = dup[2].uid;
    static const vocab_bank_t dup_bank = { dup, 6 };
    bank_error_case(&dup_bank, "duplicate uid");
    static vocab_entry_t big[VOCAB_RECORDS_MAX + 44];
    for (unsigned i = 0; i < VOCAB_RECORDS_MAX + 44; i++) {
        big[i] = vocab_bank_builtin()->entries[i % 48];
        big[i].uid = (vocab_uid_t)(20000 + i);
    }
    static const vocab_bank_t big_bank = { big, VOCAB_RECORDS_MAX + 44 };
    bank_error_case(&big_bank, "bank larger than VOCAB_RECORDS_MAX");
}

// ---------------------------------------------------------------------------------------
// Spec 8.3: HOME stops playback (QA-2)
// ---------------------------------------------------------------------------------------

static void test_summary_to_home_audio(void)
{
    unsigned stops = 0, homes = 0;
    for (int k = 0; k < 2; k++) {
        qa_power_on_fresh(67);
        qa_fresh_records(R);
        qa_seed_storage(R, 3, 25, 0);  // the session ends with a level up: the longest cue
        qa_power_cycle(500);
        qa_wait(300);
        qa_tap(VOCAB_KEY_OK, 80);
        finish_session();
        // The child leaves as soon as keys act again (lock + UP/DOWN cooldown), while a
        // level-up cue started after the save may still be playing.
        qa_run_until(D.screen_since + VOCAB_SUMMARY_LOCK_MS + VOCAB_CARD_COOLDOWN_MS);
        qa_mark();
        qa_tap(k ? VOCAB_KEY_UP : VOCAB_KEY_DOWN, 60);
        homes += D.view.screen == VOCAB_SCREEN_HOME;
        stops += qa_count(VOCAB_CMD_STOP_AUDIO) > 0;
    }
    CHECK(homes == 2, "UP/DOWN on SUMMARY -> HOME");
    qa_known_defect("QA-2", stops == homes, "SUMMARY -> HOME does not request STOP_AUDIO (spec 8.3: HOME stops "
                    "playback)");
}

int main(void)
{
    test_home_levels();
    test_level_up_summary();
    test_tick_saturation();
    test_small_banks();
    test_cloze_front_rule();
    test_bank_error();
    test_summary_to_home_audio();
    qa_report_known_defects();
    return vocab_test_report("vocab QA edge-case system tests");
}
