// tests/test_vocab_qa_power.c - QA system tests: idle/power timeline, wake, deep sleep and
// persistence through the real store (fake NVS), incl. failures and format guards.
//
// - AC-47: dim at 20 s (keys act normally), off at 30 s after STOP + SAVE, deep sleep 120 s
//   later; any PRESS restarts the timeline; no battery polling while the screen is off;
// - AC-30: in SCREEN_OFF a short press and a 1 s hold only wake (delayed CLICK / LONG
//   swallowed) on HOME, on a card, on the pause card and on SUMMARY; a new press 450 ms
//   after the release acts;
// - AC-48: deep sleep is refused while a key is held and retried every 5 s;
// - AC-46 (host part), AC-39: progress, tick, XP, volume and statistics survive deep sleep
//   and power cycles bit-exactly; a power loss loses only what was not saved yet;
// - spec 10.4: a power cut between the chunk write and the meta write leaves consistent data;
// - AC-41, AC-44, spec 10.5: corrupt / truncated / missing blobs, a newer format version
//   (never overwritten, volatile mode, HOME warning), NVS init failure (no erase, warning),
//   failing writes (warning until a save succeeds), audio busy (save deferred, AC-42);
// - AC-40: a bank update keeps progress by uid, drops unknown uids, rewrites and erases
//   surplus chunk keys; AC-50: battery icon, HOME line and the <= 5 % card-boundary saves.
//
// Owner: QA engineer.
#include "test_vocab_qa_harness.h"

static void wait_for_screen(vocab_screen_t screen, uint32_t max_ms)
{
    for (uint32_t t = 0; t < max_ms && D.view.screen != screen; t += 10) qa_wait(10);
}

static void answer(bool good)
{
    qa_settle();
    if (D.view.screen != VOCAB_SCREEN_CARD) return;
    if (qa_s()->stage_format == VOCAB_FORMAT_S2) {
        qa_wait(VOCAB_S2_FRONT_MIN_MS + 50);
        qa_tap(VOCAB_KEY_OK, 80);
        qa_wait(300);
        qa_tap(good ? VOCAB_KEY_UP : VOCAB_KEY_DOWN, 80);
        qa_settle();
        return;
    }
    const uint8_t cur = qa_s()->cur;
    qa_wait(300);
    const vocab_key_t right = qa_correct_key();
    qa_tap(good ? right : qa_other_key(right), 80);
    for (int i = 0; i < 4 && D.view.screen == VOCAB_SCREEN_CARD && qa_s()->cur == cur; i++) {
        qa_settle();
        if (D.view.screen != VOCAB_SCREEN_CARD || qa_s()->cur != cur) break;
        qa_wait(300);
        // Stage 3 LEARN retry: miss again when `good` is false, else answer; holds: right key.
        const bool both = D.view.panel[0].visible && D.view.panel[1].visible;
        qa_tap(both && !good ? qa_other_key(qa_correct_key()) : qa_correct_key(), 80);
    }
    qa_settle();
}

static void play_session(unsigned pattern)
{
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    for (unsigned i = 0; i < 12 && D.view.screen != VOCAB_SCREEN_SUMMARY; i++) answer(((pattern >> i) & 1u) == 0);
    wait_for_screen(VOCAB_SCREEN_SUMMARY, 3000);
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "session completed");
    qa_wait(VOCAB_SUMMARY_LOCK_MS + 200);
    qa_tap(VOCAB_KEY_UP, 80);
}

static bool blobs_equal(const qa_blob_t *a, const qa_blob_t *b)
{
    for (int i = 0; i < QA_MAX_BLOBS; i++) {
        if (a[i].used != b[i].used) return false;
        if (!a[i].used) continue;
        if (strcmp(a[i].key, b[i].key) != 0 || a[i].len != b[i].len || memcmp(a[i].data, b[i].data, a[i].len) != 0) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// AC-47: idle timeline
// ---------------------------------------------------------------------------------------

static void test_idle_timeline(void)
{
    qa_power_on_fresh(31);
    qa_wait(1000);
    qa_tap(VOCAB_KEY_UP, 80);  // ignored on HOME, but a PRESS: the timeline restarts here
    const vocab_ms_t t0 = D.now - 180u - 80u;
    qa_run_until(t0 + VOCAB_IDLE_DIM_MS - 1);
    CHECK(vocab_engine_power(&D.eng) == VOCAB_POWER_ACTIVE && D.backlight == VOCAB_BACKLIGHT_ON, "active at 19.999 s");
    qa_run_until(t0 + VOCAB_IDLE_DIM_MS);
    CHECK(vocab_engine_power(&D.eng) == VOCAB_POWER_DIM && D.backlight == VOCAB_BACKLIGHT_DIM, "dim at 20 s (AC-47)");
    // Keys act normally while dim and restore full brightness.
    qa_wait(1000);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_CARD && D.backlight == VOCAB_BACKLIGHT_ON,
          "a key while dim acts (OK starts the session) and restores 80 %% (AC-47)");
    // Mid-card: off 30 s after the last PRESS, after stopping audio and saving.
    const uint8_t cur = qa_s()->cur;
    const vocab_ms_t tp = D.now - 180u - 80u;
    qa_mark();
    qa_run_until(tp + VOCAB_IDLE_OFF_MS - 1);
    CHECK(D.backlight == VOCAB_BACKLIGHT_DIM && qa_count(VOCAB_CMD_SAVE) == 0, "dim, not yet off at 29.999 s");
    qa_run_until(tp + VOCAB_IDLE_OFF_MS);
    const int stop = qa_find(VOCAB_CMD_STOP_AUDIO, 0);
    const int save = qa_find(VOCAB_CMD_SAVE, 0);
    const int dark = qa_find(VOCAB_CMD_BACKLIGHT, save < 0 ? 0 : save);
    CHECK(stop >= 0 && save > stop && dark > save && D.log[dark].value == 0 && D.log[dark].t == tp + VOCAB_IDLE_OFF_MS,
          "off at 30 s: stop audio, save, backlight 0 (AC-47)");
    CHECK(vocab_engine_power(&D.eng) == VOCAB_POWER_OFF && D.view.screen == VOCAB_SCREEN_CARD && qa_s()->cur == cur,
          "SCREEN_OFF keeps the screen and state");
    qa_mark();
    qa_run_until(tp + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS - 1);
    CHECK(D.sleeps == 0 && qa_count(VOCAB_CMD_READ_BATTERY) == 0 && qa_count(VOCAB_CMD_PLAY_WORD) == 0 &&
              qa_count(VOCAB_CMD_PLAY_CUE) == 0, "screen off: no battery polling, no audio, no sleep yet");
    const unsigned boots = D.boots;
    qa_run_until(tp + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS);
    CHECK(D.sleeps == 1 && D.boots == boots + 1u && D.view.screen == VOCAB_SCREEN_HOME,
          "deep sleep 120 s after off (150 s), key wake reboots to HOME (AC-47, AC-3 logic)");
    const int s2 = qa_find(VOCAB_CMD_SAVE, 0);
    const int sl = qa_find(VOCAB_CMD_DEEP_SLEEP, 0);
    CHECK(s2 >= 0 && sl > s2, "save before deep sleep (AC-43)");
    // Any PRESS restarts the timeline: presses every 19 s keep the screen bright.
    for (int i = 0; i < 5; i++) {
        qa_wait(19000);
        qa_tap(VOCAB_KEY_UP, 80);
        CHECK(D.backlight == VOCAB_BACKLIGHT_ON, "a PRESS every 19 s keeps the screen on");
    }
}

// ---------------------------------------------------------------------------------------
// AC-30: wake-only first key
// ---------------------------------------------------------------------------------------

static void go_dark(void)
{
    qa_wait(VOCAB_IDLE_OFF_MS + 100);
    CHECK(D.backlight == 0 && vocab_engine_power(&D.eng) == VOCAB_POWER_OFF, "screen off");
}

static void test_wake_only(void)
{
    qa_power_on_fresh(32);
    go_dark();
    qa_mark();
    qa_tap(VOCAB_KEY_OK, 80);  // short press while off
    qa_wait(50);
    CHECK(D.backlight == VOCAB_BACKLIGHT_ON && D.view.screen == VOCAB_SCREEN_HOME && qa_count(VOCAB_CMD_READ_BATTERY) == 1,
          "short press only wakes (its delayed CLICK is swallowed), fresh battery reading (AC-30)");
    go_dark();
    const uint8_t vol = qa_volume_index();
    qa_hold(VOCAB_KEY_DOWN, 1000);  // 1 s hold while off
    CHECK(qa_volume_index() == vol && D.backlight == VOCAB_BACKLIGHT_ON, "a 1 s hold only wakes (LONG swallowed)");
    qa_wait(450);
    qa_tap(VOCAB_KEY_OK, 80);  // a new press 450 ms after the release acts
    CHECK(D.view.screen == VOCAB_SCREEN_CARD, "a new press 450 ms after the release acts (AC-30)");
    // On a card: the waking press of the correct key does not answer, and a second tap
    // inside the 400 ms tail (the BSP reports the pair as one DOUBLE) is swallowed too.
    const uint8_t cur = qa_s()->cur;
    go_dark();
    const vocab_key_t right = qa_correct_key();
    const vocab_ms_t a = D.now + 1;
    qa_push(right, VOCAB_RAW_PRESS, a);
    qa_push(right, VOCAB_RAW_RELEASE, a + 80);
    qa_push(right, VOCAB_RAW_PRESS, a + 140);
    qa_push(right, VOCAB_RAW_RELEASE, a + 200);
    qa_push(right, VOCAB_RAW_DOUBLE, a + 380);
    qa_run_until(a + 450);
    CHECK(qa_s()->cur == cur && !qa_s()->outcome_seen && D.view.screen == VOCAB_SCREEN_CARD &&
              D.backlight == VOCAB_BACKLIGHT_ON, "the waking press (and its DOUBLE) never answers the card (AC-30)");
    qa_wait(500);
    qa_tap(right, 80);
    CHECK(qa_s()->outcome_seen, "afterwards the key acts");
    qa_settle();
    // Pause card and SUMMARY keep their screen through screen-off and wake.
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    CHECK(D.view.screen == VOCAB_SCREEN_PAUSE, "paused");
    go_dark();
    qa_tap(VOCAB_KEY_DOWN, 80);  // DOWN would leave to HOME, but it only wakes
    qa_wait(50);
    CHECK(D.view.screen == VOCAB_SCREEN_PAUSE, "wake keeps the pause card");
    qa_wait(500);
    qa_tap(VOCAB_KEY_UP, 80);
    for (int i = 0; i < 12 && D.view.screen != VOCAB_SCREEN_SUMMARY; i++) answer(true);
    wait_for_screen(VOCAB_SCREEN_SUMMARY, 3000);
    go_dark();
    qa_tap(VOCAB_KEY_OK, 80);  // OK would start a session
    qa_wait(50);
    CHECK(D.view.screen == VOCAB_SCREEN_SUMMARY, "wake keeps SUMMARY");
    qa_wait(500);
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(D.view.screen == VOCAB_SCREEN_CARD, "then OK starts the next session");
}

// ---------------------------------------------------------------------------------------
// AC-48: deep sleep refused while a key is held
// ---------------------------------------------------------------------------------------

static void test_sleep_refused_while_held(void)
{
    qa_power_on_fresh(33);
    qa_wait(500);
    const vocab_ms_t t0 = D.now + 1;
    qa_push(VOCAB_KEY_UP, VOCAB_RAW_PRESS, t0);       // a key stays down (stuck / held)
    qa_push(VOCAB_KEY_UP, VOCAB_RAW_LONG, t0 + 500);  // UP LONG is ignored on HOME
    qa_run_until(t0 + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS);
    CHECK(D.refused == 1 && D.sleeps == 0 && vocab_engine_power(&D.eng) == VOCAB_POWER_OFF,
          "deep sleep refused while the key is held (AC-48)");
    qa_run_until(t0 + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + VOCAB_SLEEP_RETRY_MS);
    CHECK(D.refused == 2 && D.sleeps == 0, "retried after 5 s, still held");
    qa_push(VOCAB_KEY_UP, VOCAB_RAW_RELEASE, t0 + 157000);
    qa_run_until(t0 + 157000);
    CHECK(D.backlight == 0 && D.sleeps == 0, "a RELEASE while off neither wakes nor sleeps");
    const unsigned boots = D.boots;
    qa_run_until(t0 + VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + 2u * VOCAB_SLEEP_RETRY_MS);
    CHECK(D.sleeps == 1 && D.refused == 2 && D.boots == boots + 1u && D.view.screen == VOCAB_SCREEN_HOME,
          "next retry after the release sleeps (AC-48)");
}

// ---------------------------------------------------------------------------------------
// AC-46 (host part), AC-39: persistence round trips
// ---------------------------------------------------------------------------------------

static void test_round_trip(void)
{
    qa_power_on_fresh(34);
    play_session(0x00u);
    play_session(0x05u);
    qa_wait(300);
    qa_hold(VOCAB_KEY_DOWN, 700);  // volume 30 %
    qa_wait(1500);
    play_session(0x12u);
    const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
    static vocab_record_t before[VOCAB_RECORDS_MAX];
    memcpy(before, D.records, sizeof(before));
    CHECK(p.tick == 3 && p.volume_index == 1 && p.xp > 15 && p.objective_total > 0, "three sessions played");
    // Walk away: deep sleep and key wake (the harness compares with the last saved state).
    const unsigned checked = D.restores_checked;
    qa_wait(VOCAB_IDLE_OFF_MS + VOCAB_DEEP_SLEEP_AFTER_OFF_MS + 100);
    const vocab_session_persist_t q = vocab_engine_persist(&D.eng);
    CHECK(D.sleeps == 1 && D.restores_checked == checked + 1u, "slept and rebooted from storage");
    CHECK(memcmp(before, D.records, D.bank->count * sizeof(before[0])) == 0 && q.tick == p.tick && q.xp == p.xp &&
              q.volume_index == 1 && q.objective_total == p.objective_total && q.first_try_total == p.first_try_total,
          "deep sleep keeps progress, tick, XP, volume and statistics bit-exactly (AC-46 host part)");
    CHECK(D.view.status.volume == VOCAB_VOL_ICON_LOW && D.volume_pct == 30, "volume restored at boot");
    // A power loss mid-session loses only what was not saved.
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    const vocab_index_t w = qa_card()->index;
    answer(true);
    answer(true);
    CHECK(memcmp(&before[w], &D.records[w], sizeof(before[w])) != 0, "two cards changed RAM");
    qa_power_cycle(3000);
    CHECK(memcmp(before, D.records, D.bank->count * sizeof(before[0])) == 0 && vocab_engine_persist(&D.eng).tick == 3,
          "power loss: back to the last saved state");
    CHECK(D.view.screen == VOCAB_SCREEN_HOME, "boot after a power loss: HOME");
}

static void test_power_cut_between_chunk_and_meta(void)
{
    qa_power_on_fresh(35);
    play_session(0x00u);
    const vocab_session_persist_t p1 = vocab_engine_persist(&D.eng);
    D.check_restore = false;  // storage holds the new chunk with the old meta by design
    D.cut_power_on_meta = true;
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    static vocab_record_t ram[VOCAB_RECORDS_MAX];
    for (unsigned i = 0; i < 12 && D.boots == 1 && D.view.screen != VOCAB_SCREEN_SUMMARY; i++) {
        memcpy(ram, D.records, sizeof(ram));
        answer(true);
    }
    for (unsigned i = 0; i < 40 && D.boots == 1; i++) qa_wait(100);
    CHECK(D.boots == 2 && D.view.screen == VOCAB_SCREEN_HOME, "power cut during the SUMMARY save -> reboot");
    D.cut_power_on_meta = false;
    const vocab_session_persist_t p2 = vocab_engine_persist(&D.eng);
    CHECK(p2.tick == p1.tick && p2.xp == p1.xp, "meta not written: tick %u and XP %u of the previous save", p2.tick,
          p2.xp);
    unsigned newer = 0;
    for (vocab_index_t i = 0; i < D.bank->count; i++) {
        const vocab_record_t *r = &D.records[i];
        if (vocab_record_seen(r)) CHECK(r->due <= p2.tick + VOCAB_TOO_EASY_INTERVAL, "consistent due after the cut");
        newer += r->mastery >= 1 && r->due > p2.tick;
    }
    CHECK(newer >= 5, "the chunk with the session's outcomes reached flash (spec 10.4: dues relative to the old tick)");
    D.check_restore = true;
    play_session(0x00u);
    CHECK(vocab_engine_persist(&D.eng).tick == p1.tick + 1u, "the device carries on normally");
    (void)ram;
}

// ---------------------------------------------------------------------------------------
// AC-41, AC-44, spec 10.5: corrupt storage, newer format, NVS failures
// ---------------------------------------------------------------------------------------

static void test_corrupt_storage(void)
{
    qa_power_on_fresh(36);
    play_session(0x00u);
    play_session(0x00u);
    const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
    CHECK(qa_count_seen() > 0, "progress exists");
    D.check_restore = false;
    // Corrupt chunk: its words restart fresh, meta (tick, XP) is kept, no warning.
    qa_blob("p00")->data[12] ^= 0x5Au;
    qa_power_cycle(1000);
    CHECK(qa_count_seen() == 0 && vocab_engine_persist(&D.eng).tick == p.tick && vocab_engine_persist(&D.eng).xp == p.xp,
          "corrupt chunk: its words restart, tick and XP kept (AC-41)");
    CHECK(!D.view.home.warn.visible, "a corrupt chunk is repaired silently (no warning)");
    play_session(0x00u);
    const vocab_session_persist_t p2 = vocab_engine_persist(&D.eng);
    // Corrupt meta: defaults (XP 0, volume 80 %), tick = tick at write of the chunks.
    qa_blob("meta")->data[9] ^= 0x01u;
    const unsigned seen = qa_count_seen();
    qa_power_cycle(1000);
    CHECK(vocab_engine_persist(&D.eng).xp == 0 && qa_volume_index() == 0 && vocab_engine_persist(&D.eng).tick == p2.tick &&
              qa_count_seen() == seen, "corrupt meta: defaults with tick = max(tick at write) (AC-41)");
    // Missing meta: the same.
    play_session(0x00u);
    const vocab_session_persist_t p3 = vocab_engine_persist(&D.eng);
    qa_blob("meta")->used = false;
    qa_power_cycle(1000);
    CHECK(vocab_engine_persist(&D.eng).tick == p3.tick && vocab_engine_persist(&D.eng).xp == 0, "missing meta");
    // Truncated chunk and a stray surplus chunk key.
    play_session(0x00u);
    qa_blob("p00")->len -= 1;
    qa_kv_put("p03", "junk", 4);
    qa_power_cycle(1000);
    CHECK(qa_count_seen() == 0, "truncated chunk: words restart");
    play_session(0x00u);
    CHECK(qa_blob("p03") == NULL && qa_blob("p00") != NULL && qa_blob("meta") != NULL,
          "the next save rewrites the chunk and erases the surplus key");
    D.check_restore = true;
    qa_power_cycle(1000);  // harness: round trip of the repaired storage
    CHECK(qa_count_seen() == 5, "repaired storage loads");
}

static void test_newer_format(bool chunk)
{
    qa_power_on_fresh(37);
    play_session(0x00u);
    // Storage written by a future firmware (version 2), as far as this firmware can tell.
    if (chunk) {
        qa_blob_t *b = qa_blob("p00");
        b->data[2] = (uint8_t)(VOCAB_FORMAT_VERSION + 1u);
    } else {
        vocab_meta_t m;
        vocab_meta_defaults(&m);
        m.version = (uint8_t)(VOCAB_FORMAT_VERSION + 1u);
        m.tick = 9;
        m.xp = 999;
        uint8_t buf[VOCAB_META_BYTES];
        vocab_meta_encode(&m, buf, sizeof(buf));
        qa_kv_put("meta", buf, sizeof(buf));
    }
    static qa_blob_t snapshot[QA_MAX_BLOBS];
    memcpy(snapshot, D.blobs, sizeof(snapshot));
    D.check_restore = false;
    qa_power_cycle(1000);
    CHECK(D.store.volatile_mode && D.view.home.warn.visible && strcmp(D.view.home.warn.text, VOCAB_STR_WARN_SAVE) == 0,
          "newer %s version: volatile mode, HOME warning (AC-41, spec 10.3)", chunk ? "chunk" : "meta");
    const unsigned sets = D.kv_sets, erases = D.kv_erases, commits = D.kv_commits;
    play_session(0x00u);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    qa_wait(300);
    qa_hold(VOCAB_KEY_OK, 700);
    qa_wait(300);
    qa_tap(VOCAB_KEY_DOWN, 80);  // pause -> HOME save point
    qa_wait(VOCAB_IDLE_OFF_MS + 500);  // screen-off save point
    CHECK(D.saves >= 3 && D.saves_volatile >= 3 && D.kv_sets == sets && D.kv_erases == erases && D.kv_commits == commits,
          "no write, erase or commit in volatile mode");
    CHECK(blobs_equal(snapshot, D.blobs), "the newer data is never overwritten (AC-41)");
    qa_tap(VOCAB_KEY_UP, 80);
    qa_wait(500);
    CHECK(D.view.home.warn.visible && strcmp(D.view.home.warn.text, VOCAB_STR_WARN_SAVE) == 0,
          "the warning stays on HOME");
    D.check_restore = true;
}

static void test_nvs_failures(void)
{
    // nvs_flash_init() failure: volatile mode, no key-value call at all, HOME warning (AC-44).
    qa_power_on_fresh(38);
    D.nvs_ok = false;
    qa_power_cycle(500);
    CHECK(D.store.volatile_mode && strcmp(D.view.home.warn.text, VOCAB_STR_WARN_SAVE) == 0, "NVS init failure: "
          "volatile mode + HOME warning (AC-44)");
    play_session(0x00u);
    CHECK(D.kv_sets == 0 && D.kv_erases == 0 && D.kv_commits == 0 && D.saves_volatile >= 1, "nothing written");
    D.nvs_ok = true;

    // A failing write: warning until a later save succeeds; the dirty state is written then.
    qa_power_on_fresh(39);
    play_session(0x00u);
    D.check_restore = false;
    D.fail_set_at = D.kv_sets + 1u;  // the next set() fails
    play_session(0x00u);
    CHECK(D.saves_failed == 1 && vocab_store_warning(&D.store) && D.view.home.warn.visible,
          "failed SUMMARY save: HOME warning (spec 10.5)");
    const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
    static vocab_record_t ram[VOCAB_RECORDS_MAX];
    memcpy(ram, D.records, sizeof(ram));
    D.fail_set_at = 0;
    qa_wait(VOCAB_IDLE_OFF_MS + 500);  // screen-off save point retries
    CHECK(!vocab_store_warning(&D.store), "a successful save clears the warning");
    qa_tap(VOCAB_KEY_UP, 80);
    qa_wait(500);
    CHECK(!D.view.home.warn.visible, "HOME warning gone after the successful save");
    D.check_restore = true;
    qa_power_cycle(1000);
    CHECK(memcmp(ram, D.records, D.bank->count * sizeof(ram[0])) == 0 && vocab_engine_persist(&D.eng).tick == p.tick,
          "the retried save stored everything");

    // Audio never idle: saves are deferred with nothing written (AC-42); a power loss then
    // returns to the last successful save.
    qa_power_on_fresh(40);
    play_session(0x00u);
    const vocab_session_persist_t before = vocab_engine_persist(&D.eng);
    const unsigned sets = D.kv_sets;
    D.audio_stuck = true;
    play_session(0x00u);
    CHECK(D.saves_deferred >= 1 && D.kv_sets == sets, "audio busy: save deferred, nothing written (AC-42)");
    qa_power_cycle(1000);
    CHECK(vocab_engine_persist(&D.eng).tick == before.tick, "a deferred save is lost on power loss (expected)");
    D.audio_stuck = false;
    play_session(0x00u);
    qa_power_cycle(1000);
    CHECK(vocab_engine_persist(&D.eng).tick == before.tick + 1u, "the next save point writes again");
}

// ---------------------------------------------------------------------------------------
// AC-40: bank update
// ---------------------------------------------------------------------------------------

static void test_bank_update(void)
{
    const vocab_bank_t *builtin = vocab_bank_builtin();
    // The "previous firmware" bank: the first 45 builtin entries + 25 retired uids (2 chunks).
    static vocab_entry_t old_entries[70];
    for (int i = 0; i < 45; i++) old_entries[i] = builtin->entries[i];
    for (int i = 45; i < 70; i++) {
        old_entries[i] = builtin->entries[i % 40];
        old_entries[i].uid = (vocab_uid_t)(6000 + i);
    }
    static const vocab_bank_t old_bank = { old_entries, 70 };
    qa_power_on_fresh(41);
    D.check_restore = false;
    D.bank = &old_bank;
    static vocab_record_t old[VOCAB_RECORDS_MAX];
    qa_fresh_records(old);
    for (int i = 0; i < 70; i++) {
        old[i].mastery = (uint8_t)(1 + i % 5);
        old[i].due = (vocab_tick_t)(10 + i % 7);
        old[i].flags = VOCAB_REC_SEEN;
        old[i].lapses = (uint8_t)(i % 3);
    }
    qa_seed_storage(old, 12, 345, 1);
    CHECK(qa_blob("p01") != NULL, "old bank used two chunks");
    D.bank = builtin;
    qa_power_cycle(1000);
    CHECK(vocab_engine_persist(&D.eng).tick == 12 && vocab_engine_persist(&D.eng).xp == 345 && qa_volume_index() == 1,
          "meta survives the bank update");
    for (vocab_index_t i = 0; i < builtin->count; i++) {
        const vocab_record_t *r = &D.records[i];
        if (i < 45) {
            CHECK(r->mastery == old[i].mastery && r->due == old[i].due && r->lapses == old[i].lapses &&
                      vocab_record_seen(r), "progress of uid %u kept by uid (AC-40)", (unsigned)r->uid);
        } else {
            CHECK(!vocab_record_seen(r) && r->mastery == 0, "new uid %u starts fresh (AC-40)", (unsigned)r->uid);
        }
    }
    CHECK(D.store.force_all, "fingerprint change forces a full rewrite");
    play_session(0x00u);
    CHECK(qa_blob("p01") == NULL && qa_blob("p00") != NULL, "surplus chunk key erased at the next save (AC-40)");
    D.check_restore = true;
    qa_power_cycle(1000);
    CHECK(vocab_engine_persist(&D.eng).tick == 13, "the rewritten storage loads");
}

// ---------------------------------------------------------------------------------------
// AC-50: battery
// ---------------------------------------------------------------------------------------

static void test_battery(void)
{
    qa_power_on_fresh(42);
    D.battery = -1;
    qa_power_cycle(500);
    CHECK(D.view.status.battery == VOCAB_BATT_ICON_UNKNOWN && strcmp(D.view.status.batt_pct.text, "--%") == 0 &&
              !D.view.home.warn.visible, "unavailable gauge: dim empty icon, '--%%', no warning (AC-50)");
    D.battery = 87;
    qa_wait(15000);
    qa_tap(VOCAB_KEY_UP, 80);
    qa_wait(15100);
    CHECK(D.view.status.battery == VOCAB_BATT_ICON_FULL && strcmp(D.view.status.batt_pct.text, "87%") == 0,
          "polled within 30 s: 87 %%");
    D.battery = 9;
    qa_wait(15000);
    qa_tap(VOCAB_KEY_UP, 80);
    qa_wait(15100);
    CHECK(D.view.status.battery == VOCAB_BATT_ICON_EMPTY && D.view.status.battery_low &&
              strcmp(D.view.home.warn.text, VOCAB_STR_WARN_BATTERY) == 0, "9 %%: empty red icon, HOME warning");
    // <= 5 %: a save at every card boundary.
    D.battery = 5;
    qa_wait(15000);
    qa_tap(VOCAB_KEY_UP, 80);
    qa_wait(15100);
    qa_wait(300);
    qa_mark();
    qa_tap(VOCAB_KEY_OK, 80);
    CHECK(!D.view.status.batt_pct.visible, "sessions show the icon only");
    answer(true);
    answer(true);
    unsigned low = 0;
    for (unsigned i = 0; i < D.log_len; i++) low += D.log[i].kind == VOCAB_CMD_SAVE && D.log[i].value == VOCAB_SAVE_LOW_BATTERY;
    CHECK(low == 2, "<= 5 %%: one save per card boundary (got %u) (spec 9)", low);
    // Wake reads the battery at once.
    D.battery = 50;
    qa_wait(VOCAB_IDLE_OFF_MS + 100);
    qa_tap(VOCAB_KEY_UP, 80);
    CHECK(D.view.status.battery == VOCAB_BATT_ICON_2 && !D.view.status.battery_low, "reading on wake");
}

// ---------------------------------------------------------------------------------------
// QA-4 (= reviewer R-1): a realistic double tap while the screen is off
// ---------------------------------------------------------------------------------------

// iot_button 4.2.0 reports two presses as one DOUBLE when the second press starts within
// 180 ms of the first release, about 185 ms after the second release. 100 ms taps with a
// 150 ms gap are an ordinary double tap of a child.
static void double_tap_slow(vocab_key_t key, vocab_ms_t t)
{
    qa_push(key, VOCAB_RAW_PRESS, t);
    qa_push(key, VOCAB_RAW_RELEASE, t + 100);
    qa_push(key, VOCAB_RAW_PRESS, t + 250);
    qa_push(key, VOCAB_RAW_RELEASE, t + 350);
    qa_push(key, VOCAB_RAW_DOUBLE, t + 535);
    qa_run_until(t + 600);
}

static void test_wake_by_double_tap(void)
{
    // On a Stage 1 card: the waking double tap must not answer (spec 3.3 R2, AC-30).
    qa_power_on_fresh(43);
    qa_wait(300);
    qa_tap(VOCAB_KEY_OK, 80);
    go_dark();
    const uint8_t cur = qa_s()->cur;
    double_tap_slow(qa_other_key(qa_correct_key()), D.now + 1);
    const bool card_ok = D.view.screen == VOCAB_SCREEN_CARD && qa_s()->cur == cur && !qa_s()->outcome_seen;
    // On HOME: the waking double tap of OK must not start a session.
    qa_power_on_fresh(44);
    go_dark();
    double_tap_slow(VOCAB_KEY_OK, D.now + 1);
    const bool home_ok = D.view.screen == VOCAB_SCREEN_HOME;
    CHECK(D.backlight == VOCAB_BACKLIGHT_ON, "the double tap woke the screen");
    qa_known_defect("QA-4", card_ok && home_ok,
                    "a waking double tap (100 ms taps, 150 ms gap) acts: answers the card / starts a session "
                    "(reviewer R-1; spec 3.3 R2, AC-30)");
}

int main(void)
{
    test_idle_timeline();
    test_wake_only();
    test_wake_by_double_tap();
    test_sleep_refused_while_held();
    test_round_trip();
    test_power_cut_between_chunk_and_meta();
    test_corrupt_storage();
    test_newer_format(false);
    test_newer_format(true);
    test_nvs_failures();
    test_bank_update();
    test_battery();
    qa_report_known_defects();
    return vocab_test_report("vocab QA power and persistence tests");
}
