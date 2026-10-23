// main/vocab_config.h - Word Buddy compile-time constants (spec Appendix A + amendments).
//
// Single source of truth for every tunable number of the vocabulary trainer. Pure header:
// no ESP-IDF or LVGL includes, usable by host tests. Names follow the spec (prefixed with
// VOCAB_); values may change only with PM sign-off (spec Appendix A). Amendment A3 lowered
// VOCAB_MIN_PRESS_MS from 50 to 30.
//
// Owner: lead developer. After Milestone 1 other roles may only request additions.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------------------
// Session composition and card queue (spec 4.4, 4.5, 4.8)
// ---------------------------------------------------------------------------------------
#define VOCAB_SESSION_WORDS          5u   // words per micro-session (session size = min(5, bank))
#define VOCAB_MAX_NEW_PER_SESSION    3u
#define VOCAB_MIN_NEW_PER_SESSION    1u
#define VOCAB_MAX_CARDS              (2u * VOCAB_SESSION_WORDS)  // queue bound incl. insertions
#define VOCAB_INSERT_GAP             2u   // TEST_LATER / TOO_EASY_CHECK land at cur + 1 + gap
#define VOCAB_SPOT_CHECK_THRESHOLD   3u   // "I know" claims in a row that trigger a spot check

// ---------------------------------------------------------------------------------------
// Mastery and spaced repetition on logical session ticks (spec 4.1 - 4.3)
// ---------------------------------------------------------------------------------------
#define VOCAB_MASTERY_MAX            5u
#define VOCAB_TOO_EASY_INTERVAL      30u  // due = T + 30 after a passed "too easy" check (O13)
#define VOCAB_TICK_MAX               0xFFFFu  // due arithmetic saturates here (spec 4.2)
// INTERVAL[m] in ticks for m = 0..5 (index 0 is unused: failures always use due = T + 1).
#define VOCAB_INTERVALS_INIT         { 0u, 1u, 2u, 5u, 10u, 20u }

// ---------------------------------------------------------------------------------------
// Stage timing (spec 5.3, 6, 7.4, 11.5, 11.6). All values in milliseconds.
// ---------------------------------------------------------------------------------------
#define VOCAB_ERROR_HOLD_MS          1500u  // generic error-hold lock (Stage 1 wrong, Stage 3 reveal)
#define VOCAB_S3_RETRY_LOCK_MS       600u   // Stage 3 miss 1 of 2 (LEARN)
#define VOCAB_S3_MAX_MISSES_LEARN    2u
#define VOCAB_S3_MAX_MISSES_CHECK    1u     // SPOT_CHECK / TOO_EASY_CHECK cards: first press decides
#define VOCAB_CORRECT_FEEDBACK_MS    600u   // Stage 1 / check correct feedback, then next card
#define VOCAB_SUCCESS_HOLD_MS        1200u  // Stage 3 success hold, then next card
#define VOCAB_S2_CONFIRM_MS          400u   // Stage 2 "I know" confirm flash
#define VOCAB_S2_FORGOT_ACK_MS       600u   // Stage 2 "forgot" acknowledgement
#define VOCAB_S2_FRONT_MIN_MS        1000u  // Stage 2 front: clicks ignored this long
#define VOCAB_BANNER_MS              800u   // spot-check intro, too-easy ack and intro
#define VOCAB_SUMMARY_LOCK_MS        800u   // SUMMARY ignores keys this long

// ---------------------------------------------------------------------------------------
// Input hygiene (spec 3.3; amendment A3)
// ---------------------------------------------------------------------------------------
#define VOCAB_CARD_COOLDOWN_MS       250u   // R4: UP/DOWN clicks dropped this long after an epoch
#define VOCAB_WAKE_TAIL_MS           400u   // R2: intents dropped until this long after the wake RELEASE
#define VOCAB_WAKE_GUARD_MAX_MS      10000u // R2/R6 safety disarm for a lost RELEASE
#define VOCAB_MIN_PRESS_MS           30u    // R7 (A3: 30, not 50)
#define VOCAB_LONG_REJOIN_MS         120u   // R8
#define VOCAB_INPUT_QUEUE_DEPTH      16u    // raw key events; overflow drops the new event
// Mirror of BSP_BTN_SHORT_PRESS_MS (bsp_pins.h; checked by a static assert in vocab_app.c):
// the button driver merges a PRESS that starts within this time after the same key's
// RELEASE into one DOUBLE, delivered about this time after the second RELEASE.
#define VOCAB_BTN_SHORT_PRESS_MS     180u
// R1-R4/R6-R8 for DOUBLE: a PRESS starting within this time after the same key's RELEASE
// (not after a LONG, sequence not yet closed by a CLICK/DOUBLE/LONG) continues the press
// sequence that the driver may report as one DOUBLE. Driver window + 5 ms tick + margin.
#define VOCAB_DOUBLE_WINDOW_MS       (VOCAB_BTN_SHORT_PRESS_MS + 40u)

// ---------------------------------------------------------------------------------------
// Feedback and animation (spec 8.4, 11.6)
// ---------------------------------------------------------------------------------------
#define VOCAB_TOAST_MS               1200u
#define VOCAB_WORD_PULSE_MS          600u
#define VOCAB_BLINK_PERIOD_MS        500u
#define VOCAB_SHAKE_STEP_MS          60u    // SHAKE: x offsets +6, -6, +6, -6, 0 px
#define VOCAB_SHAKE_OFFSET_PX        6
#define VOCAB_NUDGE_MS               300u   // NUDGE: border 2 -> 4 -> 2 px
#define VOCAB_NUDGE_PERIOD_MS        1000u  // repeated NUDGE in the error-hold wait part
#define VOCAB_PET_IDLE_PERIOD_MS     1600u
#define VOCAB_PET_BLINK_PERIOD_MS    4000u

// ---------------------------------------------------------------------------------------
// Power and battery (spec 9)
// ---------------------------------------------------------------------------------------
#define VOCAB_IDLE_DIM_MS            20000u
#define VOCAB_IDLE_OFF_MS            30000u
#define VOCAB_DEEP_SLEEP_AFTER_OFF_MS 120000u  // 150 s after the last PRESS in total
#define VOCAB_SLEEP_RETRY_MS         5000u     // deep sleep postponed while a key is held
#define VOCAB_BACKLIGHT_ON           80u       // percent
#define VOCAB_BACKLIGHT_DIM          20u       // percent
#define VOCAB_BATTERY_POLL_MS        30000u
#define VOCAB_BATTERY_FULL_PCT       80   // >= : LV_SYMBOL_BATTERY_FULL
#define VOCAB_BATTERY_3_PCT          55   // >= : LV_SYMBOL_BATTERY_3
#define VOCAB_BATTERY_2_PCT          30   // >= : LV_SYMBOL_BATTERY_2
#define VOCAB_BATTERY_1_PCT          10   // >= : LV_SYMBOL_BATTERY_1, else EMPTY
#define VOCAB_BATTERY_RED_PCT        15   // <= : icon red
#define VOCAB_BATTERY_WARN_PCT       10   // <= : HOME warning line
#define VOCAB_BATTERY_CARD_SAVE_PCT  5    // <= : save at every card boundary while audio is idle

// ---------------------------------------------------------------------------------------
// Audio (spec 8)
// ---------------------------------------------------------------------------------------
#define VOCAB_VOL_LEVELS_INIT        { 80u, 30u, 0u }  // percent; index 2 = mute
#define VOCAB_VOL_LEVEL_COUNT        3u
#define VOCAB_VOL_DEFAULT_INDEX      0u
#define VOCAB_AUDIO_RATE_HZ          16000u
#define VOCAB_AUDIO_BITS             16u
#define VOCAB_AUDIO_CHANNELS         1u
#define VOCAB_AUDIO_CHUNK_SAMPLES    256u   // 16 ms; a new request preempts at a chunk boundary
#define VOCAB_AUDIO_QUEUE_DEPTH      4u
#define VOCAB_CLIP_MAX_WORD_MS       1600u
#define VOCAB_CLIP_MAX_PHRASE_MS     2500u
#define VOCAB_CLIP_BLOB_MAX_BYTES    (600u * 1024u)

// ---------------------------------------------------------------------------------------
// Persistence (spec 10)
// ---------------------------------------------------------------------------------------
#define VOCAB_NVS_NAMESPACE          "vocab"
#define VOCAB_NVS_RECORDS_PER_CHUNK  64u
#define VOCAB_SAVE_AUDIO_WAIT_MS     2000u
#define VOCAB_FORMAT_VERSION         1u
#define VOCAB_META_MAGIC             0x564Du
#define VOCAB_CHUNK_MAGIC            0x5650u
#define VOCAB_META_BYTES             40u
#define VOCAB_CHUNK_HEADER_BYTES     8u
#define VOCAB_CHUNK_RECORD_BYTES     6u
#define VOCAB_CHUNK_CRC_BYTES        4u

// ---------------------------------------------------------------------------------------
// XP, levels, pet (spec 4.9)
// ---------------------------------------------------------------------------------------
#define VOCAB_XP_PER_SESSION         5u
#define VOCAB_XP_FIRST_TRY           1u
#define VOCAB_XP_NEW_MASTERED        2u
#define VOCAB_LEVEL_MAX              10u
#define VOCAB_LEVEL_XP_INIT          { 0u, 30u, 90u, 180u, 300u, 450u, 630u, 840u, 1080u, 1350u }

// ---------------------------------------------------------------------------------------
// Layout (spec 11.3, 11.4, 7.3)
// ---------------------------------------------------------------------------------------
#define VOCAB_SCREEN_W               240
#define VOCAB_SCREEN_H               320
#define VOCAB_WORD_FIT_PX            196   // word fits on one line at this width
#define VOCAB_WORD_WRAP_PX           200   // F_EN_S wrap width, at most 2 lines
#define VOCAB_BLANK_PAD_PX           8     // blank box = max(width(right), width(wrong)) + 8
#define VOCAB_CORNER_SAFE_X_MIN      16    // text with y < 30 or y > 290 stays within x 16..224
#define VOCAB_CORNER_SAFE_X_MAX      224

// ---------------------------------------------------------------------------------------
// Bank data contract limits (spec 12.1)
// ---------------------------------------------------------------------------------------
#define VOCAB_RECORDS_MAX            256u  // static per-word records (8 B each); a larger bank
                                           // shows BANK_ERROR until this is raised
#define VOCAB_TEXT_MAX_CHARS         20u   // ASCII English word/phrase
#define VOCAB_ZH_MAX_CHARS           6u    // Chinese meaning, code points
#define VOCAB_EXAMPLE_MAX_CHARS      30u   // ASCII example sentence
#define VOCAB_CLOZE_CHUNK_MAX        3u    // cloze chunks are 1..3 lowercase ASCII letters
#define VOCAB_CATEGORY_MAX           10u   // categories are 1-based (1..10 in the test bank)
#define VOCAB_CLOZE_BLANK            "____" // cloze sentences always use four underscores
