// tests/test_vocab_qa_harness.h - QA system-test harness for Word Buddy (header only).
//
// A fake device around the pure engine (main/vocab_engine.h) that mirrors main/vocab_app.c:
// - boot: the real persistence store (vocab_store + vocab_progress) over a fake NVS, the
//   engine configured from the loaded meta exactly like app_task();
// - command execution in order: audio requests are checked against mute, audio
//   availability and the (modeled) clip availability; SAVE runs save_point() (persist ->
//   meta, audio stop,
//   vocab_store_save with the audio save gate); NEW_EPOCH drains the key queue; DEEP_SLEEP
//   is refused while a key is held, otherwise the chip "sleeps" and a key wake reboots from
//   storage (main.c key_guard swallows the waking press);
// - raw BSP key events with the BSP timing (CLICK about 180 ms after the release, LONG at
//   500 ms of hold, DOUBLE for two quick taps);
// - after every step an independent English-safety scan of the whole view model (AC-27)
//   and system invariants (AC-32, AC-34, AC-50 view rules, power/backlight consistency).
//
// Every helper is `static inline` so a test file that does not use one gets no warning.
// Former product defects (QA-1..QA-4, fixed in the lead's fix round) are checked through
// qa_known_defect(): always enforced, so a regression fails the static gate.
//
// Owner: QA engineer. Used by tests/test_vocab_qa_*.c only.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_engine.h"
#include "vocab_pet.h"
#include "vocab_progress.h"
#include "vocab_store.h"
#include "vocab_strings.h"
#include "vocab_test.h"

#define QA_MAX_PENDING 128
#define QA_MAX_BLOBS   48
#define QA_LOG_CAP     256
#define QA_SLEEP_MS    60000u  // time spent in deep sleep before the waking key press

// ---------------------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------------------

typedef struct {
    char key[16];
    uint8_t data[VOCAB_CHUNK_MAX_BYTES + 16];
    size_t len;
    bool used;
} qa_blob_t;

typedef struct {
    vocab_cmd_kind_t kind;
    vocab_cue_t cue;
    vocab_uid_t uid;
    uint32_t value;
    vocab_ms_t t;
} qa_logged_t;

typedef struct {
    // App task state (main/vocab_app.c statics).
    vocab_engine_t eng;
    vocab_view_t view;
    vocab_outbox_t out;
    vocab_record_t records[VOCAB_RECORDS_MAX];
    vocab_store_t store;
    vocab_meta_t meta;
    const vocab_bank_t *bank;
    vocab_ms_t now;
    uint32_t seed;               // RNG seed of the next boot (device: esp_random())
    // Device configuration.
    bool nvs_ok;                 // nvs_flash_init() succeeded at boot
    bool audio_ok;               // vocab_audio_start() succeeded at boot
    bool clips;                  // the clip pack is usable (models vocab_audio_has_clip)
    vocab_uid_t missing_uid;     // one uid without a clip (0 = none)
    int battery;                 // bsp_battery_soc() result (-1 = unavailable)
    bool audio_stuck;            // the save gate never sees an idle audio worker
    // Device state.
    uint8_t backlight;
    uint8_t volume_pct;          // worker volume (boot value, then SET_VOLUME)
    bool key_held[VOCAB_KEY_COUNT];
    int battery_reported;        // last value given to vocab_engine_battery()
    // Fake NVS (persists across reboots).
    qa_blob_t blobs[QA_MAX_BLOBS];
    unsigned kv_sets, kv_erases, kv_commits;
    unsigned fail_set_at;        // 1-based set() call that fails (0 = never)
    bool fail_commit;
    bool fail_reads;
    bool cut_power_on_meta;      // power cut right before `meta` is written
    bool power_cut;              // set when the cut happened (the harness reboots)
    // Statistics.
    unsigned boots, saves, saves_ok, saves_deferred, saves_failed, saves_volatile;
    unsigned sleeps, refused, words, cues, steps, raw, epochs;
    unsigned screens_seen, formats_seen;
    // Command log since the last qa_mark().
    qa_logged_t log[QA_LOG_CAP];
    unsigned log_len;
    // Pending raw events in time order (the BSP key queue).
    vocab_raw_event_t pending[QA_MAX_PENDING];
    int npending;
    // Invariant bookkeeping.
    vocab_tick_t max_tick;       // highest tick seen in this boot
    bool scan;                   // run the view scanner after every step
    vocab_screen_t last_screen;  // screen after the previous step
    vocab_ms_t screen_since;     // time the current screen appeared
    uint8_t last_cur;            // session cursor after the previous step
    vocab_ms_t card_since;       // time the current card appeared
    bool last_locked;
    vocab_ms_t lock_since;       // time the current lock window started
    vocab_ms_t unlock_since;     // time the last lock window ended
    // Persistence round trip: what storage holds after the last successful save.
    bool check_restore;          // compare a reboot's loaded state with the snapshot
    bool saved_valid;
    vocab_record_t saved_records[VOCAB_RECORDS_MAX];
    vocab_session_persist_t saved_persist;
    unsigned restores_checked;
    // Hooks for oracles.
    void (*on_saved)(void);      // after a save that left storage equal to RAM (OK/CLEAN)
    void (*on_boot)(bool loaded);
    // Known defect counters (reported once at the end).
    unsigned home_toasts;        // HOME views with a toast in the hint bar
    unsigned home_toast_overlap; // QA-1
    unsigned progress_stale;     // QA-3: "n/N" not yet counting a just-inserted card
    unsigned progress_stale_waiting;  // ... while a card waits for input (would be worse)
    unsigned insertions;         // inserted cards observed (QA-3 exercised)
    uint8_t last_card_count;
} qa_dev_t;

static qa_dev_t D;

static inline void qa_execute(void);
static inline void qa_after_step(void);

// Records tracked for the bank (a bank larger than the record array shows BANK_ERROR).
static inline unsigned qa_record_count(void)
{
    return D.bank->count <= VOCAB_RECORDS_MAX ? D.bank->count : 0u;
}

// ---------------------------------------------------------------------------------------
// Former defects QA-1..QA-4 (see reports/qa.md), now enforced regression checks.
// ---------------------------------------------------------------------------------------

// Regression check for a former product defect: `fixed` must hold (strict, no lenient mode).
static inline void qa_known_defect(const char *id, bool fixed, const char *what)
{
    printf("regression check %s (%s): %s\n", id, what, fixed ? "not present" : "PRESENT");
    CHECK(fixed, "%s regressed: %s", id, what);
}

// ---------------------------------------------------------------------------------------
// Fake NVS (vocab_kv_ops_t)
// ---------------------------------------------------------------------------------------

static inline qa_blob_t *qa_blob(const char *key)
{
    for (int i = 0; i < QA_MAX_BLOBS; i++) {
        if (D.blobs[i].used && strcmp(D.blobs[i].key, key) == 0) return &D.blobs[i];
    }
    return NULL;
}

static inline vocab_kv_status_t qa_kv_get(void *ctx, const char *key, void *buf, size_t *len)
{
    (void)ctx;
    if (D.fail_reads) return VOCAB_KV_ERROR;
    const qa_blob_t *b = qa_blob(key);
    if (!b) return VOCAB_KV_MISSING;
    if (b->len > *len) return VOCAB_KV_ERROR;  // nvs_get_blob: ESP_ERR_NVS_INVALID_LENGTH
    memcpy(buf, b->data, b->len);
    *len = b->len;
    return VOCAB_KV_OK;
}

static inline bool qa_kv_put(const char *key, const void *buf, size_t len)
{
    qa_blob_t *b = qa_blob(key);
    for (int i = 0; !b && i < QA_MAX_BLOBS; i++) {
        if (!D.blobs[i].used) b = &D.blobs[i];
    }
    if (!b || len > sizeof(b->data)) return false;
    b->used = true;
    snprintf(b->key, sizeof(b->key), "%s", key);
    memcpy(b->data, buf, len);
    b->len = len;
    return true;
}

static inline bool qa_kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    if (D.power_cut) return false;
    D.kv_sets++;
    if (D.fail_set_at && D.kv_sets == D.fail_set_at) return false;
    if (D.cut_power_on_meta && strcmp(key, "meta") == 0) {
        D.power_cut = true;  // the chip dies before meta reaches the flash
        return false;
    }
    return qa_kv_put(key, buf, len);
}

static inline bool qa_kv_erase(void *ctx, const char *key)
{
    (void)ctx;
    if (D.power_cut) return false;
    D.kv_erases++;
    qa_blob_t *b = qa_blob(key);
    if (b) b->used = false;
    return true;
}

static inline bool qa_kv_commit(void *ctx)
{
    (void)ctx;
    if (D.power_cut) return false;
    D.kv_commits++;
    return !D.fail_commit;
}

static const vocab_kv_ops_t QA_KV_OPS = { qa_kv_get, qa_kv_set, qa_kv_erase, qa_kv_commit };

static inline void qa_kv_wipe(void)
{
    memset(D.blobs, 0, sizeof(D.blobs));
}

// ---------------------------------------------------------------------------------------
// Audio model
// ---------------------------------------------------------------------------------------

// Clip availability is modeled (D.clips / D.missing_uid) instead of reading the clip index:
// host builds carry no clip pack (amendment A6: the index is parsed from the embedded blob at
// runtime), so the device answer of vocab_audio_has_clip() is simulated here.
static inline bool qa_has_clip(void *ctx, vocab_uid_t uid)
{
    (void)ctx;
    return D.clips && uid != VOCAB_UID_NONE && uid != D.missing_uid;
}

static inline uint8_t qa_level_pct(uint8_t index)
{
    static const uint8_t levels[VOCAB_VOL_LEVEL_COUNT] = VOCAB_VOL_LEVELS_INIT;
    return levels[index < VOCAB_VOL_LEVEL_COUNT ? index : VOCAB_VOL_DEFAULT_INDEX];
}

static inline uint8_t qa_volume_index(void)
{
    return vocab_engine_persist(&D.eng).volume_index;
}

// The session's own condition for a playable word clip (spec 8.4).
static inline bool qa_word_audio(vocab_uid_t uid)
{
    return D.audio_ok && qa_level_pct(qa_volume_index()) > 0 && qa_has_clip(NULL, uid);
}

static inline bool qa_gate_idle(void *ctx)
{
    (void)ctx;
    return !D.audio_stuck;
}

static inline void qa_gate_wait(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

// ---------------------------------------------------------------------------------------
// Command log
// ---------------------------------------------------------------------------------------

static inline void qa_mark(void)
{
    D.log_len = 0;
}

static inline void qa_log(const vocab_cmd_t *c)
{
    if (D.log_len >= QA_LOG_CAP) return;
    qa_logged_t *l = &D.log[D.log_len++];
    l->kind = c->kind;
    l->cue = c->cue;
    l->uid = c->uid;
    l->value = c->value;
    l->t = D.now;
}

// Index of the first logged command of `kind` at or after `from`, or -1.
static inline int qa_find(vocab_cmd_kind_t kind, int from)
{
    for (int i = from < 0 ? 0 : from; i < (int)D.log_len; i++) {
        if (D.log[i].kind == kind) return i;
    }
    return -1;
}

static inline unsigned qa_count(vocab_cmd_kind_t kind)
{
    unsigned n = 0;
    for (unsigned i = 0; i < D.log_len; i++) n += D.log[i].kind == kind;
    return n;
}

// ---------------------------------------------------------------------------------------
// Boot (mirrors app_task() in main/vocab_app.c)
// ---------------------------------------------------------------------------------------

static inline void qa_boot(void)
{
    D.boots++;
    D.power_cut = false;
    vocab_store_init(&D.store, D.nvs_ok ? &QA_KV_OPS : NULL, NULL);
    const bool loaded = vocab_store_load(&D.store, D.bank, D.records, &D.meta);
    D.volume_pct = qa_level_pct(D.meta.volume_index);
    D.backlight = 0;
    vocab_engine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session.bank = D.bank;
    cfg.session.records = D.bank->count <= VOCAB_RECORDS_MAX ? D.records : NULL;
    cfg.session.rng = NULL;
    cfg.session.audio_ok = D.audio_ok;
    cfg.session.has_clip = D.audio_ok ? qa_has_clip : NULL;
    cfg.session.clip_ctx = NULL;
    cfg.session.tick = D.meta.tick;
    cfg.session.xp = D.meta.xp;
    cfg.session.volume_index = D.meta.volume_index;
    cfg.session.objective_total = D.meta.objective_answers;
    cfg.session.first_try_total = D.meta.first_try_correct;
    cfg.session.storage_warning = vocab_store_warning(&D.store);
    cfg.rng_seed = D.seed++;
    vocab_view_init(&D.view);
    vocab_outbox_clear(&D.out);
    vocab_engine_init(&D.eng, &cfg, D.now, &D.view, &D.out);
    memset(D.key_held, 0, sizeof(D.key_held));
    D.npending = 0;
    D.max_tick = D.meta.tick;
    D.last_screen = D.view.screen;
    D.screen_since = D.now;
    if (loaded && D.check_restore && D.saved_valid && !D.store.volatile_mode) {
        // Spec 10 / AC-46 host part: a reboot restores exactly what the last save stored.
        D.restores_checked++;
        const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
        CHECK(memcmp(D.records, D.saved_records, qa_record_count() * sizeof(D.records[0])) == 0,
              "reboot: records differ from the last saved state (round trip)");
        CHECK(p.tick == D.saved_persist.tick && p.xp == D.saved_persist.xp &&
                  p.volume_index == D.saved_persist.volume_index &&
                  p.objective_total == D.saved_persist.objective_total &&
                  p.first_try_total == D.saved_persist.first_try_total,
              "reboot: tick %u xp %u vol %u stats %u/%u differ from the saved %u %u %u %u/%u", (unsigned)p.tick,
              (unsigned)p.xp, (unsigned)p.volume_index, (unsigned)p.objective_total, (unsigned)p.first_try_total,
              (unsigned)D.saved_persist.tick, (unsigned)D.saved_persist.xp, (unsigned)D.saved_persist.volume_index,
              (unsigned)D.saved_persist.objective_total, (unsigned)D.saved_persist.first_try_total);
    }
    if (D.on_boot) D.on_boot(loaded);
    qa_execute();
    qa_after_step();
}

// Writes `records` (bank-parallel) and meta values into the fake NVS through the real store
// (as if an earlier run had saved them); the next boot loads them.
static inline void qa_seed_storage(const vocab_record_t *records, vocab_tick_t tick, uint32_t xp,
                                   uint8_t volume_index)
{
    vocab_store_t st;
    vocab_meta_t meta;
    static vocab_record_t tmp[VOCAB_RECORDS_MAX];
    vocab_store_init(&st, &QA_KV_OPS, NULL);
    vocab_store_load(&st, D.bank, tmp, &meta);
    memcpy(tmp, records, D.bank->count * sizeof(tmp[0]));
    meta.tick = tick;
    meta.xp = xp;
    meta.volume_index = volume_index;
    const vocab_save_result_t res = vocab_store_save(&st, D.bank, tmp, &meta, NULL, 0);
    CHECK(res == VOCAB_SAVE_OK || res == VOCAB_SAVE_CLEAN, "seeding storage failed (%d)", (int)res);
    memcpy(D.saved_records, tmp, D.bank->count * sizeof(tmp[0]));
    D.saved_persist.tick = tick;
    D.saved_persist.xp = xp;
    D.saved_persist.volume_index = volume_index;
    D.saved_persist.objective_total = meta.objective_answers;
    D.saved_persist.first_try_total = meta.first_try_correct;
    D.saved_valid = true;
}

// Bank-parallel records of a fresh device (uids only).
static inline void qa_fresh_records(vocab_record_t *records)
{
    memset(records, 0, D.bank->count * sizeof(records[0]));
    for (vocab_index_t i = 0; i < D.bank->count; i++) records[i].uid = D.bank->entries[i].uid;
}

static inline void qa_set_record(vocab_record_t *records, vocab_uid_t uid, uint8_t mastery, vocab_tick_t due,
                                 uint8_t flags)
{
    vocab_index_t i = VOCAB_INDEX_NONE;
    CHECK(vocab_bank_find(D.bank, uid, &i), "uid %u not in the bank", (unsigned)uid);
    if (i == VOCAB_INDEX_NONE) return;
    records[i].mastery = mastery;
    records[i].due = due;
    records[i].flags = flags;
}

static inline vocab_index_t qa_index_of(vocab_uid_t uid)
{
    vocab_index_t i = VOCAB_INDEX_NONE;
    CHECK(vocab_bank_find(D.bank, uid, &i), "uid %u not in the bank", (unsigned)uid);
    return i;
}

// A fresh device (empty NVS) powered on at t = 3 s with the given boot seed.
static inline void qa_power_on_fresh(uint32_t seed)
{
    void (*saved_hook)(void) = D.on_saved;
    void (*boot_hook)(bool) = D.on_boot;
    const unsigned toasts = D.home_toasts;
    const unsigned overlap = D.home_toast_overlap;
    const unsigned stale = D.progress_stale;
    const unsigned stale_waiting = D.progress_stale_waiting;
    const unsigned insertions = D.insertions;
    memset(&D, 0, sizeof(D));
    D.on_saved = saved_hook;
    D.on_boot = boot_hook;
    D.home_toasts = toasts;
    D.home_toast_overlap = overlap;
    D.insertions = insertions;
    D.progress_stale = stale;
    D.progress_stale_waiting = stale_waiting;
    D.bank = vocab_bank_builtin();
    D.seed = seed;
    D.nvs_ok = true;
    D.audio_ok = true;
    D.clips = true;
    D.battery = 80;
    D.battery_reported = -1;
    D.scan = true;
    D.check_restore = true;
    D.now = 3000;
    qa_boot();
}

// Power cycle without a save (battery pulled, reset): boots from what storage holds.
static inline void qa_power_cycle(uint32_t off_ms)
{
    D.now += off_ms;
    qa_boot();
}

// ---------------------------------------------------------------------------------------
// Save point (mirrors save_point() in main/vocab_app.c)
// ---------------------------------------------------------------------------------------

static inline void qa_save_point(void)
{
    const vocab_session_persist_t p = vocab_engine_persist(&D.eng);
    D.meta.tick = p.tick;
    D.meta.xp = p.xp;
    D.meta.volume_index = p.volume_index;
    D.meta.objective_answers = p.objective_total;
    D.meta.first_try_correct = p.first_try_total;
    D.saves++;
    const vocab_audio_gate_t gate = { qa_gate_idle, qa_gate_wait, NULL };
    const vocab_save_result_t res =
        vocab_store_save(&D.store, D.bank, D.records, &D.meta, &gate, VOCAB_SAVE_AUDIO_WAIT_MS);
    switch (res) {
    case VOCAB_SAVE_OK:
    case VOCAB_SAVE_CLEAN:
        D.saves_ok++;
        memcpy(D.saved_records, D.records, qa_record_count() * sizeof(D.records[0]));
        D.saved_persist = p;
        D.saved_valid = true;
        if (D.on_saved) D.on_saved();
        break;
    case VOCAB_SAVE_DEFERRED: D.saves_deferred++; break;
    case VOCAB_SAVE_FAILED: D.saves_failed++; break;
    case VOCAB_SAVE_VOLATILE:
    default: D.saves_volatile++; break;
    }
    vocab_engine_storage_warning(&D.eng, vocab_store_warning(&D.store), &D.view);
}

// ---------------------------------------------------------------------------------------
// Key queue and time
// ---------------------------------------------------------------------------------------

static inline void qa_track_held(const vocab_raw_event_t *raw)
{
    if ((unsigned)raw->key >= VOCAB_KEY_COUNT) return;  // malformed event (fuzz)
    if (raw->kind == VOCAB_RAW_PRESS) D.key_held[raw->key] = true;
    if (raw->kind == VOCAB_RAW_RELEASE) D.key_held[raw->key] = false;
}

static inline vocab_raw_event_t qa_pop(void)
{
    const vocab_raw_event_t raw = D.pending[0];
    memmove(&D.pending[0], &D.pending[1], (size_t)(D.npending - 1) * sizeof(D.pending[0]));
    D.npending--;
    return raw;
}

static inline void qa_push(vocab_key_t key, vocab_raw_kind_t kind, vocab_ms_t t)
{
    if (D.npending >= QA_MAX_PENDING) return;  // full BSP queue: the new event is dropped
    int i = D.npending++;
    while (i > 0 && (int32_t)(D.pending[i - 1].t_ms - t) > 0) {
        D.pending[i] = D.pending[i - 1];
        i--;
    }
    D.pending[i].key = key;
    D.pending[i].kind = kind;
    D.pending[i].t_ms = t;
}

// NEW_EPOCH: events already in the queue are drained (bookkeeping only).
static inline void qa_drain(void)
{
    while (D.npending > 0 && vocab_ms_reached(D.now, D.pending[0].t_ms)) {
        const vocab_raw_event_t raw = qa_pop();
        qa_track_held(&raw);
        vocab_engine_key_drained(&D.eng, &raw, D.now);
    }
}

static inline bool qa_any_key_held(void)
{
    return D.key_held[0] || D.key_held[1] || D.key_held[2];
}

static inline void qa_execute(void)
{
    CHECK(!D.out.overflow, "engine outbox overflow (AC-32)");
    const vocab_outbox_t cmds = D.out;
    vocab_outbox_clear(&D.out);
    for (uint8_t i = 0; i < cmds.count; i++) {
        const vocab_cmd_t *c = &cmds.cmd[i];
        qa_log(c);
        const vocab_power_t power = vocab_engine_power(&D.eng);
        switch (c->kind) {
        case VOCAB_CMD_PLAY_WORD:
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            CHECK(D.audio_ok && D.volume_pct > 0 && qa_has_clip(NULL, c->uid),
                  "word clip %u requested although it cannot play (AC-34)", (unsigned)c->uid);
            CHECK(power == VOCAB_POWER_ACTIVE || power == VOCAB_POWER_DIM, "audio while the screen is off");
            D.words++;
            break;
        case VOCAB_CMD_PLAY_CUE:
            CHECK(D.audio_ok && D.volume_pct > 0, "cue %d requested while muted/unavailable (AC-34)", (int)c->cue);
            CHECK(power == VOCAB_POWER_ACTIVE || power == VOCAB_POWER_DIM, "cue while the screen is off");
            D.cues++;
            break;
        case VOCAB_CMD_STOP_AUDIO:
            break;
        case VOCAB_CMD_NEW_EPOCH:
            D.epochs++;
            qa_drain();
            break;
        case VOCAB_CMD_SET_VOLUME:
            CHECK(D.audio_ok, "SET_VOLUME without audio");
            CHECK(c->value == 80u || c->value == 30u || c->value == 0u, "volume %u", (unsigned)c->value);
            D.volume_pct = (uint8_t)c->value;
            break;
        case VOCAB_CMD_SAVE:
            qa_save_point();
            if (D.power_cut) {
                // The chip died in the middle of the save: reboot from what reached flash.
                D.now += 2000;
                qa_boot();
                return;
            }
            break;
        case VOCAB_CMD_BACKLIGHT:
            CHECK(c->value == VOCAB_BACKLIGHT_ON || c->value == VOCAB_BACKLIGHT_DIM || c->value == 0u,
                  "backlight %u", (unsigned)c->value);
            D.backlight = (uint8_t)c->value;
            break;
        case VOCAB_CMD_READ_BATTERY:
            D.battery_reported = D.battery;
            vocab_engine_battery(&D.eng, D.battery, &D.view);
            break;
        case VOCAB_CMD_DEEP_SLEEP:
            if (qa_any_key_held()) {
                D.refused++;
                vocab_engine_sleep_refused(&D.eng, D.now);
            } else {
                D.sleeps++;
                D.now += QA_SLEEP_MS;  // asleep until a key wakes the chip (key_guard eats it)
                qa_boot();
                return;
            }
            break;
        case VOCAB_CMD_TIMER_START:
        case VOCAB_CMD_TIMER_CANCEL:
        case VOCAB_CMD_NONE:
        default:
            CHECK(false, "the engine leaked internal command %d to the app", (int)c->kind);
            break;
        }
    }
}

static inline void qa_poll_once(void)
{
    vocab_engine_poll(&D.eng, D.now, &D.view, &D.out);
    const unsigned boots = D.boots;
    qa_execute();
    qa_after_step();
    vocab_ms_t d;
    if (boots == D.boots && vocab_engine_next_deadline(&D.eng, &d)) {
        CHECK(!vocab_ms_reached(D.now, d), "deadline %u not in the future after poll(%u): the app task "
              "would spin", (unsigned)d, (unsigned)D.now);
    }
}

// Runs every engine deadline up to time `t` (inclusive) and sets the clock to `t`.
static inline void qa_advance_to(vocab_ms_t t)
{
    for (int guard = 0; guard < 20000; guard++) {
        vocab_ms_t d;
        if (!vocab_engine_next_deadline(&D.eng, &d) || !vocab_ms_reached(t, d)) break;
        if (!vocab_ms_reached(d, D.now)) d = D.now;
        D.now = d;
        qa_poll_once();
    }
    if (vocab_ms_reached(t, D.now)) D.now = t;
}

// Delivers queued raw events up to time `t` (inclusive), running deadlines in between.
static inline void qa_run_until(vocab_ms_t t)
{
    while (D.npending > 0 && vocab_ms_reached(t, D.pending[0].t_ms)) {
        const vocab_raw_event_t raw = qa_pop();
        const unsigned boots = D.boots;
        qa_advance_to(raw.t_ms);
        if (boots != D.boots) continue;  // the chip slept: the event belonged to the old run
        qa_track_held(&raw);
        D.raw++;
        vocab_engine_key(&D.eng, &raw, D.now, &D.view, &D.out);
        qa_execute();
        qa_after_step();
        // The app task polls after every key event.
        if (boots == D.boots) qa_poll_once();
    }
    qa_advance_to(t);
}

static inline void qa_wait(uint32_t ms)
{
    qa_run_until(D.now + ms);
}

// Tap: PRESS at now + 1, RELEASE after `ms`, CLICK 180 ms later. Returns at the CLICK.
static inline void qa_tap(vocab_key_t key, uint32_t ms)
{
    const vocab_ms_t t = D.now + 1;
    qa_push(key, VOCAB_RAW_PRESS, t);
    qa_push(key, VOCAB_RAW_RELEASE, t + ms);
    qa_push(key, VOCAB_RAW_CLICK, t + ms + 180);
    qa_run_until(t + ms + 180);
}

// Tap whose CLICK arrives exactly at time `click_at` (press `ms` long).
static inline void qa_tap_click_at(vocab_key_t key, uint32_t ms, vocab_ms_t click_at)
{
    const vocab_ms_t press = click_at - 180u - ms;
    qa_push(key, VOCAB_RAW_PRESS, press);
    qa_push(key, VOCAB_RAW_RELEASE, press + ms);
    qa_push(key, VOCAB_RAW_CLICK, click_at);
    qa_run_until(click_at);
}

// Long hold: PRESS at now + 1, LONG at 500 ms, RELEASE after `ms` (no CLICK).
static inline void qa_hold(vocab_key_t key, uint32_t ms)
{
    const vocab_ms_t t = D.now + 1;
    qa_push(key, VOCAB_RAW_PRESS, t);
    if (ms >= 500) qa_push(key, VOCAB_RAW_LONG, t + 500);
    qa_push(key, VOCAB_RAW_RELEASE, t + ms);
    qa_run_until(t + ms);
}

// Two quick taps: one DOUBLE, no CLICK (BSP behavior).
static inline void qa_double(vocab_key_t key)
{
    const vocab_ms_t t = D.now + 1;
    qa_push(key, VOCAB_RAW_PRESS, t);
    qa_push(key, VOCAB_RAW_RELEASE, t + 60);
    qa_push(key, VOCAB_RAW_PRESS, t + 130);
    qa_push(key, VOCAB_RAW_RELEASE, t + 190);
    qa_push(key, VOCAB_RAW_DOUBLE, t + 370);
    qa_run_until(t + 370);
}

static inline bool qa_locked(void)
{
    return vocab_session_locked(&D.eng.session);
}

// Waits out lock windows and banners.
static inline void qa_settle(void)
{
    for (int i = 0; i < 200 && qa_locked(); i++) qa_wait(50);
}

// ---------------------------------------------------------------------------------------
// Card helpers (the current card through the session; answers chosen from the view)
// ---------------------------------------------------------------------------------------

static inline const vocab_session_t *qa_s(void)
{
    return &D.eng.session;
}

static inline const vocab_card_t *qa_card(void)
{
    const vocab_session_t *s = qa_s();
    if (s->screen != VOCAB_SCREEN_CARD && s->screen != VOCAB_SCREEN_BANNER) return NULL;
    return s->cur < s->card_count ? &s->cards[s->cur] : NULL;
}

static inline const vocab_entry_t *qa_entry(void)
{
    const vocab_card_t *c = qa_card();
    return c ? vocab_bank_at(D.bank, c->index) : NULL;
}

// The key that answers the visible Stage 1 / Stage 3 card correctly (from the view).
static inline vocab_key_t qa_correct_key(void)
{
    const vocab_entry_t *e = qa_entry();
    if (!e) return VOCAB_KEY_UP;
    const char *want = D.view.card.skeleton.visible ? e->cloze_right : e->zh;
    if (D.view.panel[1].visible && strcmp(D.view.panel[1].text, want) == 0) return VOCAB_KEY_DOWN;
    return VOCAB_KEY_UP;
}

static inline vocab_key_t qa_other_key(vocab_key_t k)
{
    return k == VOCAB_KEY_UP ? VOCAB_KEY_DOWN : VOCAB_KEY_UP;
}

static inline int qa_panel_of(vocab_key_t k)
{
    return k == VOCAB_KEY_UP ? 0 : 1;
}

// ---------------------------------------------------------------------------------------
// English-safety scanner (AC-27), independent of tests/test_vocab_sim.c
// ---------------------------------------------------------------------------------------

static inline bool qa_utf8_ok(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        const int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3
                    : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0) return false;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += n;
    }
    return true;
}

static inline bool qa_is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool qa_has_letter(const char *s)
{
    for (; *s; s++) {
        if (qa_is_letter(*s)) return true;
    }
    return false;
}

static inline char qa_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// The token (letters and apostrophes around position `pos`) of `text` with `chunk`
// substituted for the cloze chunk, lowercased: e.g. "Good morning" / or -> ar = "marning".
static inline void qa_wrong_token(const vocab_entry_t *e, char *out, size_t cap)
{
    char full[64];
    snprintf(full, sizeof(full), "%.*s%s%s", (int)e->cloze_pos, e->text, e->cloze_wrong,
             e->text + e->cloze_pos + strlen(e->cloze_right));
    size_t start = e->cloze_pos;
    while (start > 0 && (qa_is_letter(full[start - 1]) || full[start - 1] == '\'')) start--;
    size_t end = e->cloze_pos;
    while (full[end] && (qa_is_letter(full[end]) || full[end] == '\'')) end++;
    size_t n = 0;
    for (size_t i = start; i < end && n + 1 < cap; i++) out[n++] = qa_lower(full[i]);
    out[n] = '\0';
}

// True when `s` contains, as a whole word (case-insensitive), the wrong-chunk spelling of
// any bank entry (spec 7.3: a wrong spelling is never displayed).
static inline bool qa_contains_misspelling(const char *s, char *which, size_t cap)
{
    static const vocab_bank_t *cached_bank;
    static char bad[VOCAB_RECORDS_MAX][32];
    static unsigned nbad;
    if (cached_bank != D.bank) {
        cached_bank = D.bank;
        nbad = 0;
        for (vocab_index_t i = 0; i < D.bank->count && nbad < VOCAB_RECORDS_MAX; i++) {
            const vocab_entry_t *e = &D.bank->entries[i];
            if (vocab_entry_has_cloze(e)) qa_wrong_token(e, bad[nbad++], sizeof(bad[0]));
        }
    }
    char token[64];
    size_t n = 0;
    for (const char *p = s;; p++) {
        if (*p && (qa_is_letter(*p) || *p == '\'')) {
            if (n + 1 < sizeof(token)) token[n++] = qa_lower(*p);
            continue;
        }
        if (n > 0) {
            token[n] = '\0';
            for (unsigned i = 0; i < nbad; i++) {
                if (strcmp(token, bad[i]) == 0) {
                    snprintf(which, cap, "%s", bad[i]);
                    return true;
                }
            }
            n = 0;
        }
        if (!*p) break;
    }
    return false;
}

static inline void qa_check_text(const char *s, const char *what)
{
    char bad[64];
    CHECK(qa_utf8_ok(s), "%s: invalid UTF-8", what);
    CHECK(!qa_contains_misspelling(s, bad, sizeof(bad)), "%s shows the misspelling '%s' in '%s' (AC-27)",
          what, bad, s);
}

// A label that must hold Chinese UI text only (no English letters at all).
static inline void qa_zh_label(const vocab_view_label_t *l, const char *what)
{
    if (!l->visible) return;
    qa_check_text(l->text, what);
    CHECK(!qa_has_letter(l->text), "%s '%s' contains English letters (AC-27)", what, l->text);
}

// An English label: must be exactly `want` (bank data of the current card).
static inline void qa_en_label(const vocab_view_label_t *l, const char *want, const char *what)
{
    if (!l->visible) return;
    qa_check_text(l->text, what);
    CHECK(want != NULL && strcmp(l->text, want) == 0, "%s '%s' is not the current card's bank text '%s' "
          "(AC-27)", what, l->text, want ? want : "(none)");
}

static inline void qa_scan_view(void)
{
    const vocab_view_t *v = &D.view;
    const vocab_session_t *s = qa_s();
    const bool card_screen = v->screen == VOCAB_SCREEN_CARD;
    const vocab_entry_t *e = card_screen ? qa_entry() : NULL;
    if (card_screen) CHECK(e != NULL, "CARD screen without a current card");

    // English elements exist only on card screens and only as the current card's data.
    if (!card_screen) {
        CHECK(!v->card.word.visible && !v->card.ipa.visible && !v->card.sentence.visible &&
                  !v->card.example.visible && !v->card.skeleton.visible,
              "English card elements visible on screen %d", (int)v->screen);
    }
    qa_en_label(&v->card.word, e ? e->text : NULL, "word");
    qa_en_label(&v->card.ipa, e ? e->ipa : NULL, "ipa");
    qa_en_label(&v->card.sentence, e ? e->example_cloze : NULL, "cloze sentence");
    qa_en_label(&v->card.example, e ? e->example : NULL, "example");
    if (v->card.sentence.visible) {
        const char *blank = strstr(v->card.sentence.text, VOCAB_CLOZE_BLANK);
        CHECK(blank && !strstr(blank + 1, VOCAB_CLOZE_BLANK) && !v->card.word.visible,
              "cloze front: one ____ and the word hidden");
    }
    if (v->card.ipa.visible) CHECK(v->card.word.visible, "IPA shown without its word");
    if (v->card.meaning.visible) CHECK(!v->card.ipa.visible, "IPA shown on the Stage 2 back (spec 6.2)");
    if (v->card.meaning.visible) CHECK(e && strcmp(v->card.meaning.text, e->zh) == 0, "meaning of another word");
    if (v->card.s3_meaning.visible) CHECK(e && strcmp(v->card.s3_meaning.text, e->zh) == 0, "S3 meaning");
    qa_zh_label(&v->card.meaning, "meaning");
    qa_zh_label(&v->card.s3_meaning, "s3 meaning");
    qa_zh_label(&v->card.prompt, "prompt");
    qa_zh_label(&v->card.banner, "banner");
    qa_zh_label(&v->home.title, "home title");
    qa_zh_label(&v->home.xp, "home xp");
    qa_zh_label(&v->home.stats, "home stats");
    qa_zh_label(&v->home.warn, "home warning");
    qa_zh_label(&v->home.go, "home go");
    qa_zh_label(&v->summary.score, "summary score");
    qa_zh_label(&v->summary.xp, "summary xp");
    qa_zh_label(&v->summary.levelup, "summary level-up");
    qa_check_text(v->hint.text, "hint");
    CHECK(!qa_has_letter(v->hint.text), "hint '%s' contains English letters", v->hint.text);
    if (v->status.progress.visible) {
        const char *p = v->status.progress.text;
        if (v->screen == VOCAB_SCREEN_HOME) {
            char want[16];
            snprintf(want, sizeof(want), "Lv.%u", (unsigned)vocab_level_for_xp(vocab_engine_persist(&D.eng).xp));
            CHECK(strcmp(p, want) == 0, "HOME progress '%s' (want %s)", p, want);
        } else {
            unsigned n = 0, total = 0;
            CHECK(sscanf(p, "%u/%u", &n, &total) == 2 && n >= 1 && n <= total && total <= VOCAB_MAX_CARDS &&
                      total <= s->card_count, "progress '%s' (queue %u)", p, (unsigned)s->card_count);
            // Observation: N lags behind an insertion until the next card is drawn.
            if (total != s->card_count) {
                D.progress_stale++;
                if (v->screen == VOCAB_SCREEN_CARD && !vocab_session_locked(s)) D.progress_stale_waiting++;
            }
        }
    }
    CHECK(!v->status.batt_pct.visible || !qa_has_letter(v->status.batt_pct.text), "battery percentage text");

    // Stage 3 skeleton: prefix + (nothing | right chunk) + suffix of the current entry.
    const vocab_view_skeleton_t *sk = &v->card.skeleton;
    if (sk->visible) {
        CHECK(e && vocab_entry_has_cloze(e), "skeleton without cloze data");
        if (e && vocab_entry_has_cloze(e)) {
            const size_t pos = e->cloze_pos;
            const size_t len = strlen(e->cloze_right);
            CHECK(strlen(sk->pre) == pos && strncmp(sk->pre, e->text, pos) == 0 &&
                      strcmp(sk->suf, e->text + pos + len) == 0 && strcmp(sk->right, e->cloze_right) == 0 &&
                      strcmp(sk->other, e->cloze_wrong) == 0,
                  "skeleton '%s|%s|%s' does not match '%s' (AC-27)", sk->pre, sk->right, sk->suf, e->text);
        }
        char shown[64];
        snprintf(shown, sizeof(shown), "%s%s%s", sk->pre, sk->filled ? sk->right : "", sk->suf);
        qa_check_text(shown, "skeleton");
        if (sk->filled && e) CHECK(strcmp(shown, e->text) == 0, "filled skeleton '%s' != '%s'", shown, e->text);
    }
    // Option panels: Chinese meanings, or an isolated chunk of the current entry.
    for (int i = 0; i < 2; i++) {
        const vocab_view_panel_t *p = &v->panel[i];
        if (!p->visible) continue;
        qa_check_text(p->text, "panel");
        if (p->text_en) {
            CHECK(card_screen && sk->visible && e && vocab_entry_has_cloze(e) &&
                      (strcmp(p->text, e->cloze_right) == 0 || strcmp(p->text, e->cloze_wrong) == 0),
                  "panel chunk '%s' is not an isolated chunk of the current word (AC-27)", p->text);
        } else {
            CHECK(!qa_has_letter(p->text), "panel '%s' contains English letters (AC-27)", p->text);
        }
    }
    if (v->panel[0].visible && v->panel[1].visible) {
        CHECK(strcmp(v->panel[0].text, v->panel[1].text) != 0, "both panels show '%s'", v->panel[0].text);
    }
    // Stage 1 meaning panels: one is the word's meaning, the lure is not a substring of it.
    if (card_screen && e && s->stage_format == VOCAB_FORMAT_S1 && v->panel[0].visible && v->panel[1].visible) {
        const bool up = strcmp(v->panel[0].text, e->zh) == 0;
        const bool down = strcmp(v->panel[1].text, e->zh) == 0;
        CHECK(up != down, "Stage 1: exactly one panel holds the meaning '%s'", e->zh);
        const char *lure = up ? v->panel[1].text : v->panel[0].text;
        CHECK(!strstr(lure, e->zh) && !strstr(e->zh, lure), "Stage 1 lure '%s' vs '%s' (spec 5.2)", lure, e->zh);
    }
}

// ---------------------------------------------------------------------------------------
// System invariants
// ---------------------------------------------------------------------------------------

static inline unsigned qa_count_seen(void)
{
    unsigned n = 0;
    for (vocab_index_t i = 0; i < qa_record_count(); i++) n += vocab_record_seen(&D.records[i]);
    return n;
}

static inline unsigned qa_count_mastered(void)
{
    unsigned n = 0;
    for (vocab_index_t i = 0; i < qa_record_count(); i++) n += D.records[i].mastery >= VOCAB_MASTERY_MAX;
    return n;
}

static inline vocab_batt_icon_t qa_battery_icon(int pct)
{
    if (pct < 0) return VOCAB_BATT_ICON_UNKNOWN;
    if (pct >= 80) return VOCAB_BATT_ICON_FULL;
    if (pct >= 55) return VOCAB_BATT_ICON_3;
    if (pct >= 30) return VOCAB_BATT_ICON_2;
    if (pct >= 10) return VOCAB_BATT_ICON_1;
    return VOCAB_BATT_ICON_EMPTY;
}

static inline void qa_check_status_and_home(void)
{
    const vocab_view_t *v = &D.view;
    // Volume icon (spec 8.4 / 8.7) and the worker volume agree with the session.
    const uint8_t idx = qa_volume_index();
    const vocab_vol_icon_t want_vol = !D.audio_ok ? VOCAB_VOL_ICON_NO_AUDIO
                                    : qa_level_pct(idx) == 0 ? VOCAB_VOL_ICON_MUTE
                                    : qa_level_pct(idx) == 30 ? VOCAB_VOL_ICON_LOW : VOCAB_VOL_ICON_HIGH;
    CHECK(v->status.volume == want_vol, "volume icon %d (want %d)", (int)v->status.volume, (int)want_vol);
    if (D.audio_ok) CHECK(D.volume_pct == qa_level_pct(idx), "worker volume %u vs session index %u",
                          (unsigned)D.volume_pct, (unsigned)idx);
    // Battery icon (spec 9, AC-50) from the last reading handed to the engine.
    const int pct = D.battery_reported < 0 ? -1 : (D.battery_reported > 100 ? 100 : D.battery_reported);
    CHECK(v->status.battery == qa_battery_icon(pct), "battery icon %d for %d %%", (int)v->status.battery, pct);
    CHECK(v->status.battery_low == (pct >= 0 && pct <= 15), "battery red flag for %d %%", pct);
    if (v->screen == VOCAB_SCREEN_HOME) {
        char want[48];
        if (pct < 0) {
            snprintf(want, sizeof(want), "%s", VOCAB_STR_BATT_PCT_UNKNOWN);
        } else {
            snprintf(want, sizeof(want), VOCAB_STR_BATT_PCT_FMT, pct);
        }
        CHECK(v->status.batt_pct.visible && strcmp(v->status.batt_pct.text, want) == 0, "HOME battery '%s' "
              "(want '%s')", v->status.batt_pct.text, want);
        const bool storage = vocab_store_warning(&D.store);
        const char *warn = storage ? VOCAB_STR_WARN_SAVE : (pct >= 0 && pct <= 10) ? VOCAB_STR_WARN_BATTERY : NULL;
        if (warn) {
            CHECK(v->home.warn.visible && strcmp(v->home.warn.text, warn) == 0, "HOME warning '%s' (want '%s')",
                  v->home.warn.text, warn);
        } else {
            CHECK(!v->home.warn.visible, "HOME warning '%s' without a reason", v->home.warn.text);
        }
        char stats[64];
        snprintf(stats, sizeof(stats), VOCAB_STR_STATS_FMT, qa_count_seen(), qa_count_mastered());
        CHECK(strcmp(v->home.stats.text, stats) == 0, "HOME stats '%s' (want '%s')", v->home.stats.text, stats);
        const uint32_t xp = vocab_engine_persist(&D.eng).xp;
        CHECK(strcmp(v->home.title.text, vocab_level_title(vocab_level_for_xp(xp))) == 0, "HOME title");
        // The start line may give way to a toast (the QA-1 fix); otherwise it is always shown.
        CHECK((v->home.go.visible && strcmp(v->home.go.text, VOCAB_STR_HOME_GO) == 0) ||
                  (!v->home.go.visible && v->hint.toast != VOCAB_TOAST_NONE), "HOME go line");
        CHECK(v->pet.visible && v->pet.idle && !v->pet.on_summary &&
                  v->pet.stage == vocab_pet_stage_for_level(vocab_level_for_xp(xp)), "HOME pet");
        // QA-1: a hint-bar toast and the start line share the bottom row on HOME.
        if (v->hint.toast != VOCAB_TOAST_NONE) {
            D.home_toasts++;
            if (v->hint.text[0] != '\0' && v->home.go.visible) D.home_toast_overlap++;
        }
    } else {
        CHECK(!v->status.batt_pct.visible, "battery percentage outside HOME");
    }
    // REPLAY hint follows the word-audio availability (spec 8.4, AC-34).
    if (v->hint.toast == VOCAB_TOAST_NONE && v->hint.kind == VOCAB_HINT_REPLAY) {
        const vocab_entry_t *e = qa_entry();
        const bool audio = e && v->screen == VOCAB_SCREEN_CARD && qa_word_audio(e->uid);
        CHECK(strcmp(v->hint.text, audio ? VOCAB_STR_HINT_LISTEN : VOCAB_STR_HINT_LOOK) == 0,
              "replay hint '%s' (word audio %d)", v->hint.text, (int)audio);
    }
}

static inline void qa_invariants(void)
{
    const vocab_session_t *s = qa_s();
    CHECK(s->card_count <= VOCAB_MAX_CARDS && s->cur <= s->card_count, "queue %u cursor %u (AC-9/AC-32)",
          (unsigned)s->card_count, (unsigned)s->cur);
    for (uint8_t a = 0; a < s->card_count; a++) {
        if (s->cards[a].purpose == VOCAB_PURPOSE_LEARN) continue;
        for (uint8_t b = (uint8_t)(a + 1u); b < s->card_count; b++) {
            CHECK(s->cards[b].purpose == VOCAB_PURPOSE_LEARN || s->cards[b].index != s->cards[a].index,
                  "two inserted cards for one word (AC-9)");
        }
    }
    const vocab_tick_t tick = vocab_engine_persist(&D.eng).tick;
    CHECK(tick >= D.max_tick, "tick went back within one boot (AC-32)");
    D.max_tick = tick;
    for (vocab_index_t i = 0; i < qa_record_count(); i++) {
        const vocab_record_t *r = &D.records[i];
        CHECK(r->mastery <= VOCAB_MASTERY_MAX, "mastery %u (AC-32)", (unsigned)r->mastery);
        CHECK((r->flags & (uint8_t)~(VOCAB_REC_SEEN | VOCAB_REC_EVER_MASTERED)) == 0, "stray record flags");
        CHECK(r->uid == D.bank->entries[i].uid, "record %u uid %u", (unsigned)i, (unsigned)r->uid);
        if (!vocab_record_seen(r)) {
            CHECK(r->mastery == 0 && r->due == 0, "unseen word %u has m %u due %u", (unsigned)r->uid,
                  (unsigned)r->mastery, (unsigned)r->due);
        } else if (tick < 60000u) {
            CHECK(r->due <= tick + VOCAB_TOO_EASY_INTERVAL, "word %u due %u > tick %u + 30", (unsigned)r->uid,
                  (unsigned)r->due, (unsigned)tick);
        }
    }
    CHECK(D.view.screen == s->screen, "view screen %d vs session %d", (int)D.view.screen, (int)s->screen);
    if (s->screen == VOCAB_SCREEN_CARD) CHECK(s->stage_format != VOCAB_FORMAT_NONE, "card without a stage");
    const vocab_power_t power = vocab_engine_power(&D.eng);
    const uint8_t want_bl = power == VOCAB_POWER_ACTIVE ? VOCAB_BACKLIGHT_ON
                          : power == VOCAB_POWER_DIM ? VOCAB_BACKLIGHT_DIM : 0u;
    CHECK(D.backlight == want_bl, "backlight %u in power state %d", (unsigned)D.backlight, (int)power);
    qa_check_status_and_home();
}

static inline void qa_after_step(void)
{
    D.steps++;
    const bool screen_changed = D.view.screen != D.last_screen;
    if (screen_changed) {
        D.last_screen = D.view.screen;
        D.screen_since = D.now;
    }
    if (qa_s()->screen == VOCAB_SCREEN_CARD && (screen_changed || qa_s()->cur != D.last_cur)) {
        D.card_since = D.now;
    }
    D.last_cur = qa_s()->cur;
    if (qa_s()->card_count > D.last_card_count && D.last_card_count > 0 &&
        (qa_s()->screen == VOCAB_SCREEN_CARD || qa_s()->screen == VOCAB_SCREEN_BANNER)) {
        D.insertions++;
    }
    D.last_card_count = qa_s()->card_count;
    const bool locked = qa_locked();
    if (locked != D.last_locked) {
        D.last_locked = locked;
        if (locked) {
            D.lock_since = D.now;
        } else {
            D.unlock_since = D.now;
        }
    }
    D.screens_seen |= 1u << (unsigned)D.view.screen;
    if (qa_s()->screen == VOCAB_SCREEN_CARD) D.formats_seen |= 1u << (unsigned)qa_s()->stage_format;
    if (D.scan) qa_scan_view();
    qa_invariants();
}

// Liveness (AC-32 "never stuck"): from any state a short key sequence reaches HOME.
static inline void qa_recover_to_home(void)
{
    for (int attempt = 0; attempt < 12 && D.view.screen != VOCAB_SCREEN_HOME; attempt++) {
        D.npending = 0;
        memset(D.key_held, 0, sizeof(D.key_held));
        if (vocab_engine_power(&D.eng) != VOCAB_POWER_ACTIVE) {
            qa_tap(VOCAB_KEY_OK, 80);  // wakes (only)
            qa_wait(500);
        }
        qa_settle();
        qa_wait(300);
        switch (D.view.screen) {
        case VOCAB_SCREEN_CARD: qa_hold(VOCAB_KEY_OK, 700); break;
        case VOCAB_SCREEN_PAUSE: qa_tap(VOCAB_KEY_DOWN, 80); break;
        case VOCAB_SCREEN_SUMMARY: qa_tap(VOCAB_KEY_UP, 80); break;
        default: qa_wait(1000); break;
        }
    }
    CHECK(D.view.screen == VOCAB_SCREEN_HOME, "never stuck: HOME reachable (AC-32), screen %d", (int)D.view.screen);
}

// Checks the former defects observed by the shared checks (only when exercised).
static inline void qa_report_known_defects(void)
{
    if (D.home_toasts > 0) {
        qa_known_defect("QA-1", D.home_toast_overlap == 0,
                        "HOME: a volume toast and the start line are both visible in the bottom row");
    }
    if (D.insertions > 0) {
        qa_known_defect("QA-3", D.progress_stale == 0,
                        "status n/N keeps the old N for 400-800 ms after a card is inserted");
    }
    CHECK(D.progress_stale_waiting == 0, "a card waiting for input shows a stale n/N (spec 4.5)");
}
