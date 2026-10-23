// main/vocab_session.h - Word Buddy learning flow: HOME -> cards -> SUMMARY (spec 4, 11.5).
//
// Pure state machine (no ESP-IDF/LVGL). It composes the session (vocab_srs), decides each
// card's format (spec 4.6), runs the stage modules ONLY through the vocab_stage.h contract,
// applies outcomes (O1-O14, incl. the anti-cheat streak through vocab_anticheat.h), inserts
// TEST_LATER / SPOT_CHECK / TOO_EASY_CHECK cards (spec 4.5: at most VOCAB_MAX_CARDS, one
// insertion per word), shows the intro / acknowledgement banners, the pause card and the
// SUMMARY, owns the named timers, translates stage audio requests (WORD_PULSE substitution
// when a clip cannot play), handles the global keys (DOWN LONG volume, OK LONG pause) and
// fills the session-owned parts of the view model.
//
// Caller: the engine (vocab_engine.c) on the app task, or host tests. Every call is
// non-blocking. `out` is appended to (the caller clears it); `view` is updated in place.
// Commands emitted: PLAY_* / STOP_AUDIO / NEW_EPOCH (translated from stages), SET_VOLUME,
// SAVE (value VOCAB_SAVE_POINT or VOCAB_SAVE_LOW_BATTERY; the app runs the same spec 10.4 gate
// for both: STOP, wait up to 2 s for audio idle, write). Owner: lead developer
// (tests/test_vocab_session.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_anticheat.h"
#include "vocab_rng.h"
#include "vocab_stage1.h"
#include "vocab_stage2.h"
#include "vocab_stage3.h"
#include "vocab_types.h"
#include "vocab_view.h"

// Returns true when the word clip of `uid` exists (device: vocab_audio_has_clip).
typedef bool (*vocab_has_clip_fn)(void *ctx, vocab_uid_t uid);

#define VOCAB_SAVE_POINT          0u  // SAVE command value: a save point of spec 10.4
#define VOCAB_SAVE_LOW_BATTERY    1u  // SAVE command value: card boundary at <= 5 % (spec 9)

typedef struct {
    const vocab_bank_t *bank;     // borrowed; must outlive the session
    vocab_record_t *records;      // borrowed; bank->count records parallel to the bank
    vocab_rng_t *rng;             // borrowed
    bool audio_ok;                // audio worker available
    vocab_has_clip_fn has_clip;   // NULL = no clips
    void *clip_ctx;
    vocab_tick_t tick;            // persisted session tick
    uint32_t xp;                  // persisted cumulative XP
    uint8_t volume_index;         // index into VOCAB_VOL_LEVELS_INIT
    uint32_t objective_total;     // persisted lifetime statistics
    uint32_t first_try_total;
    bool storage_warning;         // HOME shows the "progress cannot be saved" warning
} vocab_session_config_t;

// Values the app persists in `meta`.
typedef struct {
    vocab_tick_t tick;
    uint32_t xp;
    uint8_t volume_index;
    uint32_t objective_total;
    uint32_t first_try_total;
} vocab_session_persist_t;

typedef enum {
    VOCAB_BANNER_NONE = 0,
    VOCAB_BANNER_SPOT_INTRO,      // before a SPOT_CHECK card
    VOCAB_BANNER_TOO_EASY_ACK,    // after UP LONG "too easy" ended a card
    VOCAB_BANNER_TOO_EASY_INTRO,  // before a TOO_EASY_CHECK card
} vocab_banner_t;

typedef struct {
    vocab_index_t index;          // bank index of the word
    vocab_purpose_t purpose;
    vocab_format_t format;        // decided when the card is shown
    vocab_index_t lure;           // lure used (S1), or the word's previous lure to avoid
    bool spawned;                 // this word already spawned its inserted card
    bool was_seen;                // SEEN flag before this card was shown
    uint8_t claim_slot;           // SPOT_CHECK: target slot in the streak
    uint8_t pre_mastery;          // SPOT_CHECK: target's mastery before its claim
} vocab_card_t;

typedef struct {
    vocab_session_config_t cfg;
    vocab_screen_t screen;
    vocab_banner_t banner;
    vocab_card_t cards[VOCAB_MAX_CARDS];
    uint8_t card_count;
    uint8_t cur;                  // index of the current / next card
    uint8_t base_count;           // words composed for the session
    vocab_index_t base_index[VOCAB_SESSION_WORDS];
    bool base_ever[VOCAB_SESSION_WORDS];  // EVER_MASTERED at session start
    vocab_format_t stage_format;  // stage currently running (NONE between cards)
    union {
        vocab_stage1_t s1;
        vocab_stage2_t s2;
        vocab_stage3_t s3;
    } stage;
    vocab_streak_t streak;
    bool stage_locked;            // last stage result reported a lock window
    bool session_locked;          // banner / summary lock
    bool outcome_seen;            // the current card already reported its outcome
    bool pending_ack;             // show the too-easy acknowledgement after this card
    bool timer_armed[VOCAB_TIMER_COUNT];
    vocab_ms_t timer_deadline[VOCAB_TIMER_COUNT];
    uint8_t objective;            // objective answers this session
    uint8_t first_try;            // first-try-correct objective answers this session
    int battery_pct;              // last battery reading (-1 unknown)
    vocab_badge_t last_badge;     // badge of the card that just ended (too-easy ack)
    // Stage region saved while the pause card is shown.
    vocab_view_card_t pause_card;
    vocab_view_panel_t pause_panel[2];
    vocab_badge_t pause_badge;
    vocab_hint_t pause_hint;
} vocab_session_t;

// Initializes the session and shows HOME (or BANK_ERROR for an empty/invalid bank).
void vocab_session_init(vocab_session_t *s, const vocab_session_config_t *cfg, vocab_ms_t now,
                        vocab_view_t *view, vocab_outbox_t *out);

// Delivers one accepted intent (CLICK / LONG). Timers normally fire through
// vocab_session_poll(); an explicit VOCAB_EV_TIMER event is honored only when that timer is
// armed and its deadline has been reached (an early or stale one is ignored and the timer
// stays armed), so stages never see a timer before its deadline.
void vocab_session_handle(vocab_session_t *s, const vocab_event_t *ev, vocab_ms_t now,
                          vocab_view_t *view, vocab_outbox_t *out);

// Fires every timer whose deadline has been reached (in deadline order).
void vocab_session_poll(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view,
                        vocab_outbox_t *out);

// Earliest armed timer deadline; false when no timer is armed.
bool vocab_session_next_deadline(const vocab_session_t *s, vocab_ms_t *deadline);

// True while every intent except DOWN LONG must be dropped (spec 3.3 R5).
bool vocab_session_locked(const vocab_session_t *s);

// Updates the battery reading (-1 = unavailable) shown in the status bar and on HOME.
void vocab_session_set_battery(vocab_session_t *s, int pct, vocab_view_t *view);

// Updates the HOME storage warning (store state changed after a save).
void vocab_session_set_storage_warning(vocab_session_t *s, bool warning, vocab_view_t *view);

// Values to persist (tick, XP, volume, statistics).
vocab_session_persist_t vocab_session_persist(const vocab_session_t *s);

// Compatibility accessors.
vocab_tick_t vocab_session_tick(const vocab_session_t *s);
uint32_t vocab_session_xp_total(const vocab_session_t *s);
uint8_t vocab_session_volume_index(const vocab_session_t *s);
