// main/vocab_layout.h - pure layout math for the renderer (spec 11.3, 11.4, 7.3).
//
// Widths and heights come from injected callbacks or plain numbers: the renderer measures
// with the real LVGL fonts (lv_text_get_size), host tests pass stub metrics.
// Pure; any task; never blocks. Owner: lead developer (tests/test_vocab_layout.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VOCAB_WORD_SIZE_L = 0,  // F_EN_L (36 px), single line
    VOCAB_WORD_SIZE_M,      // F_EN_M (28 px), single line
    VOCAB_WORD_SIZE_S,      // F_EN_S (22 px), wrapped at VOCAB_WORD_WRAP_PX, <= 2 lines
} vocab_word_size_t;

// Returns the single-line pixel width of `text` in the font of `size`.
typedef int32_t (*vocab_measure_fn)(void *ctx, vocab_word_size_t size, const char *text);

// Returns the number of lines `text` needs at F_EN_S wrapped at `wrap_px`.
typedef uint8_t (*vocab_wrap_lines_fn)(void *ctx, const char *text, int32_t wrap_px);

typedef struct {
    vocab_word_size_t size;
    uint8_t lines;   // 1, or 2 for a wrapped F_EN_S phrase
    bool fits;       // false: more than 2 lines at F_EN_S (a bank validation error)
} vocab_word_fit_t;

// Word-size rule (spec 11.3): width <= VOCAB_WORD_FIT_PX at L -> L; else <= VOCAB_WORD_FIT_PX
// at M -> M; else S wrapped at VOCAB_WORD_WRAP_PX (fits when <= 2 lines).
vocab_word_fit_t vocab_layout_word_size(const char *text, vocab_measure_fn measure,
                                        vocab_wrap_lines_fn wrap_lines, void *ctx);

// Stage 3 blank box width: max(width(right), width(other)) + VOCAB_BLANK_PAD_PX.
int32_t vocab_layout_blank_width(int32_t right_px, int32_t other_px);

// Stage 3 skeleton row: prefix, blank box, suffix centered as one group on `center_x`.
typedef struct {
    int32_t pre_x;
    int32_t box_x;
    int32_t box_w;
    int32_t suf_x;
    int32_t total_w;
} vocab_skeleton_geom_t;

vocab_skeleton_geom_t vocab_layout_skeleton(int32_t pre_w, int32_t right_w, int32_t other_w,
                                            int32_t suf_w, int32_t center_x);

// Skeleton font size (spec 11.3: "Stage 3 sizes the skeleton from the full correct text"):
// the word-size rule on pre + right + suf (one line: L, M or S), then stepped down while the
// whole row (prefix + blank box sized from both chunks + suffix) is wider than
// VOCAB_SKELETON_MAX_PX. It depends only on the four strings, never on the answer state, so
// success never re-flows the row.
#define VOCAB_SKELETON_MAX_PX  216  // card inner width: x 12..228 (card 10..230, border 2)

vocab_word_size_t vocab_layout_skeleton_size(const char *pre, const char *right,
                                             const char *other, const char *suf,
                                             vocab_measure_fn measure, void *ctx);

// Vertical stack: elements i = 0..n-1 with preferred tops `pref_y` and heights `height`
// are placed in order without overlap inside [top, bottom]: preferred positions are kept
// where possible, elements are pushed down to avoid overlap, then shifted up if the last
// one would pass `bottom`. Returns false when they cannot fit (then they may overlap top).
bool vocab_layout_stack(const int32_t *pref_y, const int32_t *height, size_t n, int32_t top,
                        int32_t bottom, int32_t *out_y);
