// tests/test_vocab_layout.c - word-size rule, Stage 3 blank geometry and card stacking
// (spec 11.3, 7.3; AC-53, AC-25 blank width).
//
// Owner: lead developer. Stub metrics: 20 / 15 / 12 px per character at L / M / S.
#include <string.h>

#include "vocab_config.h"
#include "vocab_layout.h"
#include "vocab_test.h"

static int32_t stub_measure(void *ctx, vocab_word_size_t size, const char *text)
{
    (void)ctx;
    const int32_t per = size == VOCAB_WORD_SIZE_L ? 20 : size == VOCAB_WORD_SIZE_M ? 15 : 12;
    return per * (int32_t)strlen(text);
}

static uint8_t stub_lines(void *ctx, const char *text, int32_t wrap_px)
{
    (void)ctx;
    const int32_t w = 12 * (int32_t)strlen(text);
    return (uint8_t)((w + wrap_px - 1) / wrap_px);
}

static void test_word_size(void)
{
    vocab_word_fit_t fit = vocab_layout_word_size("apple", stub_measure, stub_lines, NULL);
    CHECK(fit.size == VOCAB_WORD_SIZE_L && fit.lines == 1 && fit.fits, "5 chars -> L");
    fit = vocab_layout_word_size("abcdefghi", stub_measure, stub_lines, NULL);   // 180 px at L
    CHECK(fit.size == VOCAB_WORD_SIZE_L, "180 px at L stays L");
    fit = vocab_layout_word_size("watermelon", stub_measure, stub_lines, NULL);  // 200 / 150
    CHECK(fit.size == VOCAB_WORD_SIZE_M, "200 px at L -> M (196 limit)");
    fit = vocab_layout_word_size("abcdefghijklm", stub_measure, stub_lines, NULL);  // 195 at M
    CHECK(fit.size == VOCAB_WORD_SIZE_M, "195 px at M stays M");
    fit = vocab_layout_word_size("Nice to meet you", stub_measure, stub_lines, NULL);  // 240 at M
    CHECK(fit.size == VOCAB_WORD_SIZE_S && fit.lines == 1 && fit.fits, "-> S, 1 line");
    fit = vocab_layout_word_size("abcdefghijklmnopqrstuvwxyz", stub_measure, stub_lines, NULL);
    CHECK(fit.size == VOCAB_WORD_SIZE_S && fit.lines == 2 && fit.fits, "S, 2 lines");
    fit = vocab_layout_word_size("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
                                 stub_measure, stub_lines, NULL);
    CHECK(!fit.fits && fit.lines > 2, "more than 2 lines at S is rejected (AC-53)");
}

static void test_skeleton(void)
{
    CHECK(vocab_layout_blank_width(10, 24) == 32 && vocab_layout_blank_width(30, 5) == 38,
          "blank = max(right, wrong) + 8 (AC-25)");
    // "b" + [u|a] + "s" at L: 20 + (20 + 8) + 20 = 68, centered on 120.
    vocab_skeleton_geom_t g = vocab_layout_skeleton(20, 20, 20, 20, 120);
    CHECK(g.total_w == 68 && g.pre_x == 86 && g.box_x == 106 && g.box_w == 28 && g.suf_x == 134,
          "centered group");
    // Width never depends on which chunk is right.
    vocab_skeleton_geom_t g1 = vocab_layout_skeleton(20, 40, 20, 20, 120);
    vocab_skeleton_geom_t g2 = vocab_layout_skeleton(20, 20, 40, 20, 120);
    CHECK(g1.box_w == g2.box_w && g1.total_w == g2.total_w, "the box width does not leak the answer");
    CHECK(vocab_layout_skeleton_size("b", "u", "a", "s", stub_measure, NULL) == VOCAB_WORD_SIZE_L,
          "short skeleton at L");
    // "watermelon" is 200 px at L: the word-size rule on the full text gives M (row 158).
    CHECK(vocab_layout_skeleton_size("wat", "er", "or", "melon", stub_measure, NULL) == VOCAB_WORD_SIZE_M,
          "watermelon skeleton at M");
    CHECK(vocab_layout_skeleton_size("abcdefghij", "ab", "cd", "klmno", stub_measure, NULL) ==
              VOCAB_WORD_SIZE_S, "long skeleton at S");
    // Full text "abcdefgh" 160 px at L: L even though the row is 80 + (60 + 8) + 60 = 208 > 196,
    // because it stays inside the card (<= 216); the answer state never changes the size.
    CHECK(vocab_layout_skeleton_size("abcd", "e", "xyz", "fgh", stub_measure, NULL) == VOCAB_WORD_SIZE_L,
          "size from the full correct text (spec 11.3)");
    CHECK(vocab_layout_skeleton_size("abcd", "xyz", "e", "fgh", stub_measure, NULL) == VOCAB_WORD_SIZE_M,
          "full text 'abcdxyzfgh' is 200 px at L -> M");
    // A much longer wrong chunk would push the row out of the card at L (80 + 88 + 60 = 228):
    // one size down (M: 60 + 68 + 45 = 173).
    CHECK(vocab_layout_skeleton_size("abcd", "e", "wxyz", "fgh", stub_measure, NULL) == VOCAB_WORD_SIZE_M,
          "row wider than the card steps down");
    CHECK(vocab_layout_skeleton_size(NULL, NULL, NULL, NULL, NULL, NULL) == VOCAB_WORD_SIZE_S,
          "no measure callback -> S");
}

static void test_stack(void)
{
    const int32_t pref[3] = { 40, 90, 120 };
    int32_t y[3];
    int32_t h[3] = { 44, 28, 22 };
    CHECK(vocab_layout_stack(pref, h, 3, 36, 152, y) && y[0] == 40 && y[1] == 90 && y[2] == 120,
          "preferred positions kept when they fit");
    h[2] = 44;  // the example wraps to two lines: 120 + 44 > 152
    CHECK(vocab_layout_stack(pref, h, 3, 36, 152, y) && y[2] == 108 && y[1] == 80 && y[0] == 36,
          "shifted up to fit (got %d %d %d)", (int)y[0], (int)y[1], (int)y[2]);
    h[0] = 80;
    CHECK(!vocab_layout_stack(pref, h, 3, 36, 152, y), "too tall is reported");
}

int main(void)
{
    test_word_size();
    test_skeleton();
    test_stack();
    return vocab_test_report("vocab layout tests");
}
