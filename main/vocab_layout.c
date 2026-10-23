// main/vocab_layout.c - pure layout math (see vocab_layout.h).
#include "vocab_layout.h"

#include "vocab_config.h"

vocab_word_fit_t vocab_layout_word_size(const char *text, vocab_measure_fn measure,
                                        vocab_wrap_lines_fn wrap_lines, void *ctx)
{
    vocab_word_fit_t fit = { VOCAB_WORD_SIZE_S, 1u, true };
    if (!text || !measure) return fit;
    if (measure(ctx, VOCAB_WORD_SIZE_L, text) <= VOCAB_WORD_FIT_PX) {
        fit.size = VOCAB_WORD_SIZE_L;
        return fit;
    }
    if (measure(ctx, VOCAB_WORD_SIZE_M, text) <= VOCAB_WORD_FIT_PX) {
        fit.size = VOCAB_WORD_SIZE_M;
        return fit;
    }
    fit.lines = wrap_lines ? wrap_lines(ctx, text, VOCAB_WORD_WRAP_PX) : 2u;
    if (fit.lines == 0) fit.lines = 1;
    fit.fits = fit.lines <= 2u;
    return fit;
}

int32_t vocab_layout_blank_width(int32_t right_px, int32_t other_px)
{
    return (right_px > other_px ? right_px : other_px) + VOCAB_BLANK_PAD_PX;
}

vocab_skeleton_geom_t vocab_layout_skeleton(int32_t pre_w, int32_t right_w, int32_t other_w,
                                            int32_t suf_w, int32_t center_x)
{
    vocab_skeleton_geom_t g;
    g.box_w = vocab_layout_blank_width(right_w, other_w);
    g.total_w = pre_w + g.box_w + suf_w;
    g.pre_x = center_x - g.total_w / 2;
    g.box_x = g.pre_x + pre_w;
    g.suf_x = g.box_x + g.box_w;
    return g;
}

vocab_word_size_t vocab_layout_skeleton_size(const char *pre, const char *right,
                                             const char *other, const char *suf,
                                             vocab_measure_fn measure, void *ctx)
{
    if (!measure) return VOCAB_WORD_SIZE_S;
    int s = VOCAB_WORD_SIZE_L;
    // Word-size rule on the full correct text (the sum of its parts, one line).
    for (; s < VOCAB_WORD_SIZE_S; s++) {
        const vocab_word_size_t size = (vocab_word_size_t)s;
        const int32_t full = (pre ? measure(ctx, size, pre) : 0) +
                             (right ? measure(ctx, size, right) : 0) +
                             (suf ? measure(ctx, size, suf) : 0);
        if (full <= VOCAB_WORD_FIT_PX) break;
    }
    // Step down while the row with the blank box (sized from both chunks) leaves the card.
    for (; s < VOCAB_WORD_SIZE_S; s++) {
        const vocab_word_size_t size = (vocab_word_size_t)s;
        const int32_t row = (pre ? measure(ctx, size, pre) : 0) +
                            vocab_layout_blank_width(right ? measure(ctx, size, right) : 0,
                                                     other ? measure(ctx, size, other) : 0) +
                            (suf ? measure(ctx, size, suf) : 0);
        if (row <= VOCAB_SKELETON_MAX_PX) break;
    }
    return (vocab_word_size_t)s;
}

bool vocab_layout_stack(const int32_t *pref_y, const int32_t *height, size_t n, int32_t top,
                        int32_t bottom, int32_t *out_y)
{
    if (!pref_y || !height || !out_y || n == 0) return true;
    for (size_t i = 0; i < n; i++) {
        int32_t y = pref_y[i] < top ? top : pref_y[i];
        if (i > 0 && y < out_y[i - 1] + height[i - 1]) y = out_y[i - 1] + height[i - 1];
        out_y[i] = y;
    }
    int32_t limit = bottom;
    for (size_t k = n; k-- > 0;) {
        if (out_y[k] + height[k] > limit) out_y[k] = limit - height[k];
        limit = out_y[k];
    }
    return out_y[0] >= top;
}
