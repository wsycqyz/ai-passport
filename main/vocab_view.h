// main/vocab_view.h - Word Buddy view model: a plain-data description of the whole screen.
//
// The pure core (session + stages) writes a vocab_view_t; the LVGL renderer (vocab_ui.c)
// draws it. Host tests read it to check exact visible texts per state (spec 11.1, AC-27).
// No ESP-IDF or LVGL includes.
//
// Conventions
// - Texts are copied into fixed buffers (VOCAB_VIEW_TEXT_CAP bytes incl. NUL) with
//   vocab_text_copy(), which never splits a UTF-8 sequence. A label is drawn only when
//   `visible` is true; hidden labels keep no meaning.
// - Colors are 0xRRGGBB values from the palette macros below (spec 11.2).
// - Fonts are fixed per element (spec 11.3/11.4) and chosen by the renderer: word ->
//   F_EN_L/M/S by the word-size rule; ipa -> F_IPA; example -> F_EN_XS; sentence -> F_EN_S;
//   meaning/s3_meaning/banner/panel text -> F_ZH_L (panel text F_EN_M when `text_en`);
//   prompt/hint/badge/progress/home lines -> F_ZH_S; status icons -> Montserrat 14.
// - One-shot animations are sequence counters: incrementing a counter (wrap allowed)
//   starts the animation once; the renderer compares with the last value it saw. Counters
//   are never reset (vocab_view_reset_card keeps them). Continuous animations are flags.
//
// Field ownership while a card is shown (the "stage region"):
// - Stages write: card.word/ipa/meaning/sentence/example/s3_meaning/skeleton/prompt and
//   card.frame, panel[0..1], badge, hint.kind, anim.shake_seq, anim.nudge_seq[],
//   anim.confirm_seq[]. Stages leave card.banner hidden.
// - The session writes everything else: screen, status, hint.toast and the resolved
//   hint.text/hint.color (vocab_view_resolve), card.banner, home, summary, pet, xpbar,
//   anim.pulse_seq (WORD_PULSE substitution), anim.pet_seq.
// The session calls vocab_view_reset_card() before a stage's begin(), so begin() starts
// from an empty stage region (everything hidden, badge NONE, hint NONE).
//
// Owner: lead developer. After Milestone 1 only additive, backwards-compatible changes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_types.h"

#define VOCAB_VIEW_TEXT_CAP   48u  // bytes incl. NUL; longest UI string is about 32 bytes
#define VOCAB_VIEW_PART_CAP   24u  // skeleton prefix/suffix (text <= 20 chars)
#define VOCAB_VIEW_CHUNK_CAP  4u   // cloze chunk (<= 3 letters)

// ---------------------------------------------------------------------------------------
// Palette (spec 11.2)
// ---------------------------------------------------------------------------------------
#define VOCAB_RGB_SCREEN_BG     0x121826u
#define VOCAB_RGB_CARD_BG       0x1E293Bu
#define VOCAB_RGB_CARD_BORDER   0x334155u
#define VOCAB_RGB_PANEL_BORDER  0x475569u  // neutral option panel border; dim mute icon
#define VOCAB_RGB_TEXT          0xF8FAFCu  // primary text (word, banner, toasts)
#define VOCAB_RGB_DIM           0x94A3B8u  // IPA, progress, Stage 1/3 prompt
#define VOCAB_RGB_SOFT          0xCBD5E1u  // examples, stats
#define VOCAB_RGB_YELLOW        0xFDE047u  // meanings, highlight, WORD_PULSE
#define VOCAB_RGB_HINT          0x38BDF8u  // hint bar, Lv.n, XP
#define VOCAB_RGB_S1            0x3B82F6u
#define VOCAB_RGB_S1_BG         0x1E3A8Au
#define VOCAB_RGB_S3            0x8B5CF6u
#define VOCAB_RGB_S3_BG         0x3B0764u
#define VOCAB_RGB_KNOW          0x10B981u
#define VOCAB_RGB_KNOW_BG       0x064E3Bu
#define VOCAB_RGB_FORGOT        0xF97316u
#define VOCAB_RGB_FORGOT_BG     0x7C2D12u
#define VOCAB_RGB_SUCCESS       0x34D399u  // correct panel border, success texts
#define VOCAB_RGB_CORRECT_BG    0x065F46u
#define VOCAB_RGB_SPOT          0xFBBF24u
#define VOCAB_RGB_SPOT_BG       0x78350Fu
#define VOCAB_RGB_PINK          0xF472B6u
#define VOCAB_RGB_PINK_BG       0x831843u
#define VOCAB_RGB_WARN          0xF97316u  // warnings, retry prompt, mute icon
#define VOCAB_RGB_LOW_BATTERY   0xEF4444u
#define VOCAB_RGB_BLANK_BG      0x312E81u
#define VOCAB_RGB_BLANK_LINE    0xA78BFAu
#define VOCAB_RGB_GOLD          0xFACC15u
#define VOCAB_RGB_PAUSE_BG      0x334155u

// ---------------------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------------------

// Which top-level group is shown. CARD covers Stage 1/2/3 cards incl. holds and feedback;
// BANNER is a card frame with a banner + prompt (spot-check intro, too-easy ack/intro).
typedef enum {
    VOCAB_SCREEN_BOOT = 0,   // nothing drawn yet (keys ignored)
    VOCAB_SCREEN_HOME,
    VOCAB_SCREEN_CARD,
    VOCAB_SCREEN_BANNER,
    VOCAB_SCREEN_PAUSE,
    VOCAB_SCREEN_SUMMARY,
    VOCAB_SCREEN_BANK_ERROR,
} vocab_screen_t;

// Status-bar badge (text + accent/background via vocab_view_badge_*). NONE = hidden.
typedef enum {
    VOCAB_BADGE_NONE = 0,
    VOCAB_BADGE_NEW,         // Stage 1 LEARN              (S1 colors)
    VOCAB_BADGE_RECALL,      // Stage 2 front              (know colors)
    VOCAB_BADGE_SELF_RATE,   // Stage 2 back               (know colors)
    VOCAB_BADGE_SPELL,       // Stage 3 LEARN              (S3 colors)
    VOCAB_BADGE_TEST_AGAIN,  // TEST_LATER card            (S1 colors)
    VOCAB_BADGE_SPOT_CHECK,  // spot-check intro and card  (amber)
    VOCAB_BADGE_TOO_EASY,    // too-easy intro and card    (pink)
    VOCAB_BADGE_REMEMBER,    // error hold / reveal hold   (correct colors)
    VOCAB_BADGE_PAUSE,       // pause card                 (bg 0x334155)
    VOCAB_BADGE_DONE,        // SUMMARY                    (correct colors)
} vocab_badge_t;

// Hint-bar content requested by a stage or the session. REPLAY resolves to "listen again"
// when the current word clip can play and to "look again" otherwise (spec 8.4).
typedef enum {
    VOCAB_HINT_NONE = 0,     // empty (lock parts, banners, pause)
    VOCAB_HINT_REPLAY,
    VOCAB_HINT_THINK,        // Stage 2 front, first S2_FRONT_MIN_MS
    VOCAB_HINT_FLIP,         // Stage 2 front afterwards
    VOCAB_HINT_PRESS_UP,     // error-hold wait part, correct side UP
    VOCAB_HINT_PRESS_DOWN,   // error-hold wait part, correct side DOWN
    VOCAB_HINT_SUMMARY,      // SUMMARY after the lock
} vocab_hint_t;

// Temporary hint-bar override (session-owned, VOCAB_TOAST_MS).
typedef enum {
    VOCAB_TOAST_NONE = 0,
    VOCAB_TOAST_VOL_HIGH,
    VOCAB_TOAST_VOL_LOW,
    VOCAB_TOAST_MUTE,
    VOCAB_TOAST_NO_AUDIO,
} vocab_toast_t;

// Option panel styles (bg / border via vocab_view_panel_colors).
typedef enum {
    VOCAB_PANEL_NEUTRAL = 0,  // 0x1E293B / 0x475569
    VOCAB_PANEL_S1,           // 0x1E293B / 0x3B82F6
    VOCAB_PANEL_S3,           // 0x1E293B / 0x8B5CF6
    VOCAB_PANEL_KNOW,         // 0x064E3B / 0x10B981
    VOCAB_PANEL_FORGOT,       // 0x7C2D12 / 0xF97316
    VOCAB_PANEL_CORRECT,      // 0x065F46 / 0x34D399
} vocab_panel_style_t;

typedef enum {
    VOCAB_VOL_ICON_HIGH = 0,  // LV_SYMBOL_VOLUME_MAX (80 %)
    VOCAB_VOL_ICON_LOW,       // LV_SYMBOL_VOLUME_MID (30 %)
    VOCAB_VOL_ICON_MUTE,      // LV_SYMBOL_MUTE in 0xF97316 (0 %)
    VOCAB_VOL_ICON_NO_AUDIO,  // LV_SYMBOL_MUTE in 0x475569 (audio init failed)
} vocab_vol_icon_t;

typedef enum {
    VOCAB_BATT_ICON_FULL = 0,  // >= 80
    VOCAB_BATT_ICON_3,         // >= 55
    VOCAB_BATT_ICON_2,         // >= 30
    VOCAB_BATT_ICON_1,         // >= 10
    VOCAB_BATT_ICON_EMPTY,     // < 10
    VOCAB_BATT_ICON_UNKNOWN,   // reading -1: EMPTY symbol in 0x475569
} vocab_batt_icon_t;

typedef enum {
    VOCAB_PET_EGG = 0,     // Lv1
    VOCAB_PET_HATCHLING,   // Lv2-3
    VOCAB_PET_CHILD,       // Lv4-6
    VOCAB_PET_GROWN,       // Lv7-9
    VOCAB_PET_CROWN,       // Lv10
} vocab_pet_stage_t;

// ---------------------------------------------------------------------------------------
// Elements
// ---------------------------------------------------------------------------------------

typedef struct {
    bool visible;
    uint32_t color;                  // text color 0xRRGGBB
    char text[VOCAB_VIEW_TEXT_CAP];
} vocab_view_label_t;

// Stage 3 skeleton row: prefix label + blank box + suffix label, centered as one group
// (spec 7.3). The renderer sizes the font from pre + right + suf (the full correct text) and
// the box from max(width(right), width(other)) + 8 px, so neither leaks the answer and
// success never re-flows the row. `other` is used ONLY for that measurement and is never
// drawn in the skeleton. The blank shows nothing, or `right` (in 0x34D399) when `filled`.
typedef struct {
    bool visible;
    char pre[VOCAB_VIEW_PART_CAP];
    char suf[VOCAB_VIEW_PART_CAP];
    char right[VOCAB_VIEW_CHUNK_CAP];
    char other[VOCAB_VIEW_CHUNK_CAP];
    bool filled;   // success / reveal: the full correct word is visible
    bool blink;    // BLANK_BLINK while waiting for input (continuous)
} vocab_view_skeleton_t;

// Option panel. panel[0] = UP (glyph U+25B2), panel[1] = DOWN (glyph U+25BC). The glyph is
// drawn by the renderer; `text` excludes it.
typedef struct {
    bool visible;
    vocab_panel_style_t style;
    bool text_en;                    // text is a Stage 3 chunk: F_EN_M white (else F_ZH_L)
    bool nudge_repeat;               // NUDGE every 1000 ms (error-hold wait part)
    char text[VOCAB_VIEW_TEXT_CAP];
} vocab_view_panel_t;

// Main card area (spec 11.4 widget map). Labels that never appear together share space.
typedef struct {
    bool frame;                      // card rectangle 10,34 220x120
    vocab_view_label_t word;         // English word/phrase, y 40 (2 lines at F_EN_S: y 38)
    vocab_view_label_t ipa;          // y 92, F_IPA, VOCAB_RGB_DIM
    vocab_view_label_t meaning;      // y 90, F_ZH_L, yellow (Stage 2 back)
    vocab_view_label_t sentence;     // y 44, F_EN_S, 2 lines max (Stage 2 cloze front)
    vocab_view_label_t example;      // y 120, F_EN_XS, VOCAB_RGB_SOFT (Stage 2 back)
    vocab_view_label_t s3_meaning;   // y 40, F_ZH_L, yellow (Stage 3)
    vocab_view_skeleton_t skeleton;  // y 76 (Stage 3)
    vocab_view_label_t prompt;       // y 124, F_ZH_S (overlaps `example`: show one of them)
    vocab_view_label_t banner;       // y 56, F_ZH_L (session-owned)
} vocab_view_card_t;

typedef struct {
    vocab_hint_t kind;               // requested content (stage or session)
    vocab_toast_t toast;             // session: temporary override
    char text[VOCAB_VIEW_TEXT_CAP];  // resolved by vocab_view_resolve(); "" = empty bar
    uint32_t color;
} vocab_view_hint_t;

typedef struct {
    vocab_view_label_t progress;     // "n/N" (dim) in sessions, "Lv.n" (hint blue) on HOME
    vocab_view_label_t batt_pct;     // HOME only: "87%" or "--%" (Montserrat 14)
    vocab_vol_icon_t volume;
    vocab_batt_icon_t battery;
    bool battery_low;                // icon in VOCAB_RGB_LOW_BATTERY (<= 15 %)
} vocab_view_status_t;

typedef struct {
    vocab_view_label_t title;        // level title (F_ZH_L white)
    vocab_view_label_t xp;           // "XP n/m" line (F_ZH_S dim)
    vocab_view_label_t stats;        // learned / mastered line (F_ZH_S soft)
    vocab_view_label_t warn;         // storage or battery warning (F_ZH_S orange)
    vocab_view_label_t go;           // start hint (F_ZH_L hint blue)
} vocab_view_home_t;

typedef struct {
    vocab_view_label_t score;        // y 92, F_ZH_L white
    vocab_view_label_t xp;           // y 124, F_ZH_S hint blue
    vocab_view_label_t levelup;      // y 266, F_ZH_S gold
} vocab_view_summary_t;

typedef struct {
    bool visible;
    vocab_pet_stage_t stage;
    bool on_summary;                 // box at 80,162 (SUMMARY) instead of 80,44 (HOME)
    bool idle;                       // PET_IDLE runs (HOME)
    bool level_up;                   // PET_EAT ends with recolor + three jumps
} vocab_view_pet_t;

typedef struct {
    bool visible;
    bool on_summary;                 // y 252 (SUMMARY) instead of y 170 (HOME)
    uint16_t value;                  // XP inside the current level
    uint16_t max;                    // XP span of the current level (> 0)
} vocab_view_xpbar_t;

typedef struct {
    uint8_t shake_seq;               // SHAKE of the Stage 3 blank box (stage)
    uint8_t pulse_seq;               // WORD_PULSE of the visible English element (session)
    uint8_t nudge_seq[2];            // one NUDGE of panel i (stage)
    uint8_t confirm_seq[2];          // CONFIRM_FLASH of panel i (stage)
    uint8_t pet_seq;                 // PET_EAT on SUMMARY (session)
} vocab_view_anim_t;

typedef struct {
    vocab_screen_t screen;
    vocab_view_status_t status;
    vocab_badge_t badge;
    vocab_view_card_t card;
    vocab_view_panel_t panel[2];
    vocab_view_hint_t hint;
    vocab_view_home_t home;
    vocab_view_summary_t summary;
    vocab_view_pet_t pet;
    vocab_view_xpbar_t xpbar;
    vocab_view_anim_t anim;
} vocab_view_t;

// ---------------------------------------------------------------------------------------
// Helpers (vocab_view.c; pure, any task, no allocation)
// ---------------------------------------------------------------------------------------

// Copies `src` (NULL = "") into `dst` of `cap` bytes, truncating at a UTF-8 character
// boundary; always NUL-terminates when cap > 0. Returns the number of bytes copied.
size_t vocab_text_copy(char *dst, size_t cap, const char *src);

// Number of UTF-8 code points in `s` (NULL = 0). Continuation bytes are not counted.
size_t vocab_utf8_count(const char *s);

// Everything hidden/empty, screen BOOT, counters 0. Call once at startup.
void vocab_view_init(vocab_view_t *view);

// Clears the stage region (see the ownership note above): card labels, skeleton and frame
// hidden, banner hidden, panels hidden with nudge_repeat off, badge NONE, hint.kind NONE.
// Keeps anim counters, status, toast, home, summary, pet and xpbar untouched.
void vocab_view_reset_card(vocab_view_t *view);

// Shows a label with `text` (copied) and `color`.
void vocab_view_show(vocab_view_label_t *label, const char *text, uint32_t color);

// Hides a label and clears its text.
void vocab_view_hide(vocab_view_label_t *label);

// Shows option panel `side` with `text` (copied), `style` and font kind; clears nudge_repeat.
void vocab_view_set_panel(vocab_view_t *view, vocab_side_t side, const char *text,
                          vocab_panel_style_t style, bool text_en);

// Hides option panel `side` (text cleared, nudge_repeat off).
void vocab_view_hide_panel(vocab_view_t *view, vocab_side_t side);

// Shows the Stage 3 skeleton with the given parts (copied); filled = false, blink = true.
void vocab_view_set_skeleton(vocab_view_t *view, const char *pre, const char *suf,
                             const char *right, const char *other);

// Default badge of a card: TEST_LATER -> TEST_AGAIN, SPOT_CHECK -> SPOT_CHECK,
// TOO_EASY_CHECK -> TOO_EASY; LEARN: S1 -> NEW, S2 -> RECALL, S3 -> SPELL.
vocab_badge_t vocab_view_card_badge(vocab_purpose_t purpose, vocab_format_t format);

// Badge text (vocab_strings.h; "" for NONE) and its accent (text + border) / bg colors.
const char *vocab_view_badge_text(vocab_badge_t badge);
void vocab_view_badge_colors(vocab_badge_t badge, uint32_t *accent, uint32_t *bg);

// Option panel colors for a style.
void vocab_view_panel_colors(vocab_panel_style_t style, uint32_t *bg, uint32_t *border);

// Hint text for a kind; `word_audio` selects listen/look for REPLAY. "" for NONE.
const char *vocab_view_hint_text(vocab_hint_t kind, bool word_audio);

// Toast text ("" for NONE).
const char *vocab_view_toast_text(vocab_toast_t toast);

// Resolves hint.text/hint.color from hint.toast (white) or hint.kind (VOCAB_RGB_HINT).
// The session calls it after every update; `word_audio` = the current card's word clip can
// play now (volume > 0, audio available, clip found).
void vocab_view_resolve(vocab_view_t *view, bool word_audio);
