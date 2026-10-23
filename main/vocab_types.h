// main/vocab_types.h - Word Buddy shared plain-data types (pure; no ESP-IDF/LVGL).
//
// Shared by the session, the three stage modules, the input filter, persistence, the app
// task and the audio worker. Everything here is plain data plus tiny static inline helpers,
// so it compiles under `cc -std=c11 -Wall -Wextra -Werror` on the host.
//
// Owner: lead developer. After Milestone 1 only additive, backwards-compatible changes are
// allowed (amendments A1); report them to the orchestrator.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_config.h"

// =======================================================================================
// Time, identifiers, ticks
// =======================================================================================

// Monotonic milliseconds supplied by the caller (device: esp_timer_get_time() / 1000).
// Wraps after about 49.7 days; always compare with vocab_ms_reached(), never with < / >.
typedef uint32_t vocab_ms_t;

static inline bool vocab_ms_reached(vocab_ms_t now, vocab_ms_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

typedef uint16_t vocab_uid_t;    // stable word id (spec 12.1); 0 is never a valid uid
typedef uint16_t vocab_index_t;  // index into a bank's entry array
typedef uint16_t vocab_tick_t;   // logical session tick (spec 4.2)

#define VOCAB_UID_NONE   0u
#define VOCAB_INDEX_NONE 0xFFFFu

// Saturating tick arithmetic (spec 4.2: due arithmetic saturates at 65535).
static inline vocab_tick_t vocab_tick_add(vocab_tick_t tick, uint32_t delta)
{
    const uint32_t sum = (uint32_t)tick + delta;
    return sum > VOCAB_TICK_MAX ? (vocab_tick_t)VOCAB_TICK_MAX : (vocab_tick_t)sum;
}

// =======================================================================================
// Keys and input (spec 3)
// =======================================================================================

// Key identity. Numeric values equal bsp_btn_t (UP = 0, DOWN = 1, OK = 2); main.c asserts it.
typedef enum {
    VOCAB_KEY_UP = 0,
    VOCAB_KEY_DOWN = 1,
    VOCAB_KEY_OK = 2,
} vocab_key_t;
#define VOCAB_KEY_COUNT 3u

// Raw BSP event kinds as captured by main.c on_key(). Numeric values equal bsp_btn_ev_t.
// Only the input filter (vocab_input) consumes these; stages never see raw events.
typedef enum {
    VOCAB_RAW_PRESS = 0,
    VOCAB_RAW_CLICK = 1,
    VOCAB_RAW_DOUBLE = 2,   // treated as one CLICK (spec 3.1)
    VOCAB_RAW_LONG = 3,     // fires once at 500 ms of hold; no CLICK follows
    VOCAB_RAW_RELEASE = 4,  // bookkeeping only, never a command
} vocab_raw_kind_t;

typedef struct {
    vocab_key_t key;
    vocab_raw_kind_t kind;
    vocab_ms_t t_ms;        // capture time in the BSP callback
} vocab_raw_event_t;

// =======================================================================================
// Timers (requested through the outbox, delivered as VOCAB_EV_TIMER)
// =======================================================================================

// Named one-shot timers. The two STAGE timers belong to the active stage; the session
// cancels both when the card ends, before a pause card and when leaving the session.
// A stage may only start/cancel VOCAB_TIMER_STAGE and VOCAB_TIMER_STAGE_AUX.
typedef enum {
    VOCAB_TIMER_STAGE = 0,      // stage timer #1 (locks, feedback, front minimum, ...)
    VOCAB_TIMER_STAGE_AUX = 1,  // stage timer #2 (spare; independent of #1)
    VOCAB_TIMER_SESSION = 2,    // session: banners, acks, summary lock
    VOCAB_TIMER_TOAST = 3,      // session: hint-bar toast
    VOCAB_TIMER_COUNT = 4,
} vocab_timer_id_t;
#define VOCAB_STAGE_TIMER_COUNT 2u

static inline bool vocab_timer_is_stage(vocab_timer_id_t id)
{
    return id == VOCAB_TIMER_STAGE || id == VOCAB_TIMER_STAGE_AUX;
}

// =======================================================================================
// Events delivered to the session and to stages
// =======================================================================================

typedef enum {
    VOCAB_EV_NONE = 0,
    VOCAB_EV_CLICK,   // accepted click intent (BSP CLICK or DOUBLE); `key` is valid
    VOCAB_EV_LONG,    // accepted long-press intent; `key` is valid
    VOCAB_EV_TIMER,   // a timer armed with VOCAB_CMD_TIMER_START expired; `timer` is valid
} vocab_event_kind_t;

typedef struct {
    vocab_event_kind_t kind;
    vocab_key_t key;
    vocab_timer_id_t timer;
} vocab_event_t;

static inline vocab_event_t vocab_event_click(vocab_key_t key)
{
    const vocab_event_t ev = { VOCAB_EV_CLICK, key, VOCAB_TIMER_STAGE };
    return ev;
}

static inline vocab_event_t vocab_event_long(vocab_key_t key)
{
    const vocab_event_t ev = { VOCAB_EV_LONG, key, VOCAB_TIMER_STAGE };
    return ev;
}

static inline vocab_event_t vocab_event_timer(vocab_timer_id_t timer)
{
    const vocab_event_t ev = { VOCAB_EV_TIMER, VOCAB_KEY_OK, timer };
    return ev;
}

// =======================================================================================
// Cards (spec 4.5, 4.6)
// =======================================================================================

typedef enum {
    VOCAB_PURPOSE_LEARN = 0,       // the session's own word
    VOCAB_PURPOSE_TEST_LATER,      // inserted after Stage 2 "forgot" (Stage 1 format)
    VOCAB_PURPOSE_SPOT_CHECK,      // inserted after 3 "I know" claims (Stage 1 or 3)
    VOCAB_PURPOSE_TOO_EASY_CHECK,  // inserted after UP LONG "too easy" (Stage 3 or 1)
} vocab_purpose_t;

// Card format = which stage module runs the card. Decided by the session when the card is
// shown (spec 4.6). Stage 2 is only used for LEARN cards.
typedef enum {
    VOCAB_FORMAT_NONE = 0,  // not decided / invalid
    VOCAB_FORMAT_S1 = 1,  // Stage 1: two-choice meaning
    VOCAB_FORMAT_S2 = 2,  // Stage 2: flashcard + self-rating (word or cloze-sentence front)
    VOCAB_FORMAT_S3 = 3,  // Stage 3: phonics cloze
} vocab_format_t;

// Option side. Also the index of the option panel in vocab_view_t.panel[] (0 = UP).
typedef enum {
    VOCAB_SIDE_UP = 0,
    VOCAB_SIDE_DOWN = 1,
} vocab_side_t;

static inline vocab_side_t vocab_side_other(vocab_side_t side)
{
    return side == VOCAB_SIDE_UP ? VOCAB_SIDE_DOWN : VOCAB_SIDE_UP;
}

// The key that selects a side (UP key -> UP panel, DOWN key -> DOWN panel).
static inline vocab_key_t vocab_side_key(vocab_side_t side)
{
    return side == VOCAB_SIDE_UP ? VOCAB_KEY_UP : VOCAB_KEY_DOWN;
}

// =======================================================================================
// Outcomes reported by stages (spec 4.3). The session maps (purpose, format, result) to the
// O1..O14 rows; stages never touch records.
// =======================================================================================

typedef enum {
    VOCAB_RESULT_NONE = 0,
    VOCAB_RESULT_CORRECT_FIRST,   // objective card, first press correct: O1 O5 O7 O10 O13
    VOCAB_RESULT_CORRECT_SECOND,  // Stage 3 LEARN, correct on the second press: O8
    VOCAB_RESULT_WRONG,           // objective card, final miss: O2 O6 O9 O11 O14
    VOCAB_RESULT_KNOW,            // Stage 2 back, UP "I know": O3
    VOCAB_RESULT_FORGOT,          // Stage 2 back, DOWN "forgot, test me later": O4
    VOCAB_RESULT_TOO_EASY,        // UP LONG on a LEARN Stage 1 question / Stage 2 front: O12
} vocab_result_t;

// =======================================================================================
// Audio cues (spec 8.2). Shared by the session, stages, the tone synthesizer and the worker.
// =======================================================================================

typedef enum {
    VOCAB_CUE_NONE = 0,
    VOCAB_CUE_CORRECT,   // 1319 Hz 70 ms + 1760 Hz 110 ms
    VOCAB_CUE_WRONG,     // 392 Hz 90 ms + 330 Hz 150 ms (soft, never a buzzer)
    VOCAB_CUE_FLIP,      // 600 -> 1200 Hz sweep, 60 ms
    VOCAB_CUE_TICK,      // 1000 Hz, 40 ms
    VOCAB_CUE_CHEER,     // 1047, 1319, 1568 Hz x 80 ms + 2093 Hz 160 ms
    VOCAB_CUE_LEVEL_UP,  // 1047, 1319, 1568, 2093, 1568 Hz x 80 ms + 2093 Hz 300 ms
    VOCAB_CUE_COUNT,
} vocab_cue_t;

// =======================================================================================
// Commands (outbox). Pure modules never call hardware: they append commands and the caller
// executes them in order. "Who may emit" is part of the contract.
// =======================================================================================

typedef enum {
    VOCAB_CMD_NONE = 0,
    // --- may be emitted by stages ---
    VOCAB_CMD_PLAY_WORD,           // uid: play the word clip (session substitutes WORD_PULSE
                                   //      when the clip cannot play: mute, missing, no audio)
    VOCAB_CMD_PLAY_CUE,            // cue: play a synthesized cue (skipped when muted)
    VOCAB_CMD_PLAY_CUE_THEN_WORD,  // cue, uid: cue, then the word clip (one request)
    VOCAB_CMD_STOP_AUDIO,          // stop playback at the next chunk boundary
    VOCAB_CMD_TIMER_START,         // timer, value = delay in ms (>= 1); re-arming replaces
    VOCAB_CMD_TIMER_CANCEL,        // timer
    VOCAB_CMD_NEW_EPOCH,           // input epoch (spec 3.3 R3/R4/R9): flush queued input,
                                   // drop presses begun earlier, restart the 250 ms cooldown
    // --- session / engine only (stages must not emit these) ---
    VOCAB_CMD_SET_VOLUME,          // value = percent (80 / 30 / 0)
    VOCAB_CMD_SAVE,                // a save point (spec 10.4); the app runs the save gate
    VOCAB_CMD_BACKLIGHT,           // value = percent
    VOCAB_CMD_READ_BATTERY,        // the app reads bsp_battery_soc() and reports it back
    VOCAB_CMD_DEEP_SLEEP,          // run the deep-sleep sequence (spec 9)
} vocab_cmd_kind_t;

typedef struct {
    vocab_cmd_kind_t kind;
    vocab_cue_t cue;          // PLAY_CUE, PLAY_CUE_THEN_WORD
    vocab_timer_id_t timer;   // TIMER_START, TIMER_CANCEL
    vocab_uid_t uid;          // PLAY_WORD, PLAY_CUE_THEN_WORD
    uint32_t value;           // TIMER_START: ms; SET_VOLUME / BACKLIGHT: percent
} vocab_cmd_t;

#define VOCAB_OUTBOX_CAP 16u

// Ordered command list filled by one call into a pure module. The caller clears it before
// the call (vocab_outbox_clear) and executes the commands in order afterwards. A full outbox
// drops the command and sets `overflow` (a bug: tests assert it never happens).
typedef struct {
    uint8_t count;
    bool overflow;
    vocab_cmd_t cmd[VOCAB_OUTBOX_CAP];
} vocab_outbox_t;

static inline void vocab_outbox_clear(vocab_outbox_t *out)
{
    out->count = 0;
    out->overflow = false;
}

static inline bool vocab_outbox_push(vocab_outbox_t *out, vocab_cmd_kind_t kind, vocab_cue_t cue,
                                     vocab_timer_id_t timer, vocab_uid_t uid, uint32_t value)
{
    if (out->count >= VOCAB_OUTBOX_CAP) {
        out->overflow = true;
        return false;
    }
    vocab_cmd_t *cmd = &out->cmd[out->count++];
    cmd->kind = kind;
    cmd->cue = cue;
    cmd->timer = timer;
    cmd->uid = uid;
    cmd->value = value;
    return true;
}

static inline bool vocab_out_play_word(vocab_outbox_t *out, vocab_uid_t uid)
{
    return vocab_outbox_push(out, VOCAB_CMD_PLAY_WORD, VOCAB_CUE_NONE, VOCAB_TIMER_STAGE, uid, 0);
}

static inline bool vocab_out_play_cue(vocab_outbox_t *out, vocab_cue_t cue)
{
    return vocab_outbox_push(out, VOCAB_CMD_PLAY_CUE, cue, VOCAB_TIMER_STAGE, VOCAB_UID_NONE, 0);
}

static inline bool vocab_out_play_cue_then_word(vocab_outbox_t *out, vocab_cue_t cue, vocab_uid_t uid)
{
    return vocab_outbox_push(out, VOCAB_CMD_PLAY_CUE_THEN_WORD, cue, VOCAB_TIMER_STAGE, uid, 0);
}

static inline bool vocab_out_stop_audio(vocab_outbox_t *out)
{
    return vocab_outbox_push(out, VOCAB_CMD_STOP_AUDIO, VOCAB_CUE_NONE, VOCAB_TIMER_STAGE,
                             VOCAB_UID_NONE, 0);
}

static inline bool vocab_out_timer_start(vocab_outbox_t *out, vocab_timer_id_t timer, uint32_t ms)
{
    return vocab_outbox_push(out, VOCAB_CMD_TIMER_START, VOCAB_CUE_NONE, timer, VOCAB_UID_NONE,
                             ms == 0 ? 1u : ms);
}

static inline bool vocab_out_timer_cancel(vocab_outbox_t *out, vocab_timer_id_t timer)
{
    return vocab_outbox_push(out, VOCAB_CMD_TIMER_CANCEL, VOCAB_CUE_NONE, timer, VOCAB_UID_NONE, 0);
}

static inline bool vocab_out_new_epoch(vocab_outbox_t *out)
{
    return vocab_outbox_push(out, VOCAB_CMD_NEW_EPOCH, VOCAB_CUE_NONE, VOCAB_TIMER_STAGE,
                             VOCAB_UID_NONE, 0);
}

// Returns the index of the first command of `kind` at or after `from`, or -1. Test helper.
static inline int vocab_outbox_find(const vocab_outbox_t *out, vocab_cmd_kind_t kind, int from)
{
    for (int i = from < 0 ? 0 : from; i < (int)out->count; i++) {
        if (out->cmd[i].kind == kind) return i;
    }
    return -1;
}

// =======================================================================================
// Word bank entry (spec 12.1 data contract; also the target of the deferred pipeline)
// =======================================================================================

typedef enum {
    VOCAB_KIND_WORD = 0,
    VOCAB_KIND_PHRASE = 1,  // text contains a space
} vocab_kind_t;

// One bank entry, stored as const data in flash. Field rules (validated by
// tests/test_vocab_bank.c): uid unique and stable forever; text ASCII, 1..20 chars;
// ipa UTF-8 British broad transcription between slashes, or NULL (sentence-like phrases);
// zh UTF-8, 1..6 code points; category 1-based; cloze_right/cloze_wrong both NULL or both
// set (1..3 lowercase letters, equal length, text[cloze_pos..] starts with cloze_right);
// example ASCII <= 30 chars containing text exactly once; example_cloze = example with text
// replaced by "____", or NULL; example_zh reserved (always NULL in iteration 1).
typedef struct {
    vocab_uid_t uid;
    uint8_t category;
    uint8_t cloze_pos;          // byte offset of the blank in `text` (valid with cloze data)
    const char *text;
    const char *ipa;
    const char *zh;
    const char *cloze_right;
    const char *cloze_wrong;
    const char *example;
    const char *example_cloze;
    const char *example_zh;
} vocab_entry_t;

// A bank is an array of entries in curriculum order plus its length. Pure functions take a
// `const vocab_bank_t *` so tests can pass small custom banks (1 entry, one category, ...).
typedef struct {
    const vocab_entry_t *entries;
    vocab_index_t count;
} vocab_bank_t;

// =======================================================================================
// Per-word learning record (spec 4.1). RAM array parallel to the bank: records[i] belongs to
// bank->entries[i]. 8 bytes per word.
// =======================================================================================

#define VOCAB_REC_SEEN           0x01u  // set when the word is first shown
#define VOCAB_REC_EVER_MASTERED  0x02u  // set the first time mastery reaches 5 (XP bonus)

typedef struct {
    vocab_uid_t uid;
    vocab_tick_t due;      // tick at which the word is due again (unseen: 0)
    uint8_t mastery;       // 0..VOCAB_MASTERY_MAX
    uint8_t flags;         // VOCAB_REC_*
    uint8_t lapses;        // saturating failure count (statistics only)
    uint8_t reserved;      // 0
} vocab_record_t;

static inline bool vocab_record_seen(const vocab_record_t *r)
{
    return (r->flags & VOCAB_REC_SEEN) != 0;
}
