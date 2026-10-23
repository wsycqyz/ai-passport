// main/vocab_view.c - view-model helpers (pure; see vocab_view.h).
#include "vocab_view.h"

#include <string.h>

#include "vocab_strings.h"

size_t vocab_text_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
        // Step back over UTF-8 continuation bytes so no character is split.
        while (len > 0 && ((unsigned char)src[len] & 0xC0u) == 0x80u) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

size_t vocab_utf8_count(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (((unsigned char)*s & 0xC0u) != 0x80u) n++;
    }
    return n;
}

void vocab_view_init(vocab_view_t *view)
{
    memset(view, 0, sizeof(*view));
    view->screen = VOCAB_SCREEN_BOOT;
    view->status.volume = VOCAB_VOL_ICON_HIGH;
    view->status.battery = VOCAB_BATT_ICON_UNKNOWN;
    view->hint.color = VOCAB_RGB_HINT;
    view->xpbar.max = 1;
}

void vocab_view_show(vocab_view_label_t *label, const char *text, uint32_t color)
{
    label->visible = true;
    label->color = color;
    vocab_text_copy(label->text, sizeof(label->text), text);
}

void vocab_view_hide(vocab_view_label_t *label)
{
    label->visible = false;
    label->text[0] = '\0';
}

void vocab_view_reset_card(vocab_view_t *view)
{
    vocab_view_card_t *card = &view->card;
    card->frame = false;
    vocab_view_hide(&card->word);
    vocab_view_hide(&card->ipa);
    vocab_view_hide(&card->meaning);
    vocab_view_hide(&card->sentence);
    vocab_view_hide(&card->example);
    vocab_view_hide(&card->s3_meaning);
    vocab_view_hide(&card->prompt);
    vocab_view_hide(&card->banner);
    memset(&card->skeleton, 0, sizeof(card->skeleton));
    vocab_view_hide_panel(view, VOCAB_SIDE_UP);
    vocab_view_hide_panel(view, VOCAB_SIDE_DOWN);
    view->badge = VOCAB_BADGE_NONE;
    view->hint.kind = VOCAB_HINT_NONE;
}

void vocab_view_set_panel(vocab_view_t *view, vocab_side_t side, const char *text,
                          vocab_panel_style_t style, bool text_en)
{
    vocab_view_panel_t *panel = &view->panel[side == VOCAB_SIDE_DOWN ? 1 : 0];
    panel->visible = true;
    panel->style = style;
    panel->text_en = text_en;
    panel->nudge_repeat = false;
    vocab_text_copy(panel->text, sizeof(panel->text), text);
}

void vocab_view_hide_panel(vocab_view_t *view, vocab_side_t side)
{
    vocab_view_panel_t *panel = &view->panel[side == VOCAB_SIDE_DOWN ? 1 : 0];
    panel->visible = false;
    panel->nudge_repeat = false;
    panel->style = VOCAB_PANEL_NEUTRAL;
    panel->text_en = false;
    panel->text[0] = '\0';
}

void vocab_view_set_skeleton(vocab_view_t *view, const char *pre, const char *suf,
                             const char *right, const char *other)
{
    vocab_view_skeleton_t *sk = &view->card.skeleton;
    sk->visible = true;
    vocab_text_copy(sk->pre, sizeof(sk->pre), pre);
    vocab_text_copy(sk->suf, sizeof(sk->suf), suf);
    vocab_text_copy(sk->right, sizeof(sk->right), right);
    vocab_text_copy(sk->other, sizeof(sk->other), other);
    sk->filled = false;
    sk->blink = true;
}

vocab_badge_t vocab_view_card_badge(vocab_purpose_t purpose, vocab_format_t format)
{
    switch (purpose) {
    case VOCAB_PURPOSE_TEST_LATER: return VOCAB_BADGE_TEST_AGAIN;
    case VOCAB_PURPOSE_SPOT_CHECK: return VOCAB_BADGE_SPOT_CHECK;
    case VOCAB_PURPOSE_TOO_EASY_CHECK: return VOCAB_BADGE_TOO_EASY;
    case VOCAB_PURPOSE_LEARN:
    default:
        break;
    }
    switch (format) {
    case VOCAB_FORMAT_S2: return VOCAB_BADGE_RECALL;
    case VOCAB_FORMAT_S3: return VOCAB_BADGE_SPELL;
    case VOCAB_FORMAT_S1:
    case VOCAB_FORMAT_NONE:
    default:
        return VOCAB_BADGE_NEW;
    }
}

typedef struct {
    const char *text;
    uint32_t accent;
    uint32_t bg;
} badge_style_t;

static const badge_style_t BADGES[] = {
    [VOCAB_BADGE_NONE] = { "", VOCAB_RGB_DIM, VOCAB_RGB_SCREEN_BG },
    [VOCAB_BADGE_NEW] = { VOCAB_STR_BADGE_NEW, VOCAB_RGB_S1, VOCAB_RGB_S1_BG },
    [VOCAB_BADGE_RECALL] = { VOCAB_STR_BADGE_RECALL, VOCAB_RGB_KNOW, VOCAB_RGB_KNOW_BG },
    [VOCAB_BADGE_SELF_RATE] = { VOCAB_STR_BADGE_SELF_RATE, VOCAB_RGB_KNOW, VOCAB_RGB_KNOW_BG },
    [VOCAB_BADGE_SPELL] = { VOCAB_STR_BADGE_SPELL, VOCAB_RGB_S3, VOCAB_RGB_S3_BG },
    [VOCAB_BADGE_TEST_AGAIN] = { VOCAB_STR_BADGE_TEST_AGAIN, VOCAB_RGB_S1, VOCAB_RGB_S1_BG },
    [VOCAB_BADGE_SPOT_CHECK] = { VOCAB_STR_BADGE_SPOT_CHECK, VOCAB_RGB_SPOT, VOCAB_RGB_SPOT_BG },
    [VOCAB_BADGE_TOO_EASY] = { VOCAB_STR_BADGE_TOO_EASY, VOCAB_RGB_PINK, VOCAB_RGB_PINK_BG },
    [VOCAB_BADGE_REMEMBER] = { VOCAB_STR_BADGE_REMEMBER, VOCAB_RGB_SUCCESS, VOCAB_RGB_CORRECT_BG },
    [VOCAB_BADGE_PAUSE] = { VOCAB_STR_BADGE_PAUSE, VOCAB_RGB_SOFT, VOCAB_RGB_PAUSE_BG },
    [VOCAB_BADGE_DONE] = { VOCAB_STR_BADGE_DONE, VOCAB_RGB_SUCCESS, VOCAB_RGB_CORRECT_BG },
};

const char *vocab_view_badge_text(vocab_badge_t badge)
{
    if ((size_t)badge >= sizeof(BADGES) / sizeof(BADGES[0])) return "";
    return BADGES[badge].text;
}

void vocab_view_badge_colors(vocab_badge_t badge, uint32_t *accent, uint32_t *bg)
{
    const badge_style_t *style =
        (size_t)badge < sizeof(BADGES) / sizeof(BADGES[0]) ? &BADGES[badge] : &BADGES[0];
    if (accent) *accent = style->accent;
    if (bg) *bg = style->bg;
}

void vocab_view_panel_colors(vocab_panel_style_t style, uint32_t *bg, uint32_t *border)
{
    uint32_t b = VOCAB_RGB_CARD_BG;
    uint32_t line = VOCAB_RGB_PANEL_BORDER;
    switch (style) {
    case VOCAB_PANEL_S1: line = VOCAB_RGB_S1; break;
    case VOCAB_PANEL_S3: line = VOCAB_RGB_S3; break;
    case VOCAB_PANEL_KNOW: b = VOCAB_RGB_KNOW_BG; line = VOCAB_RGB_KNOW; break;
    case VOCAB_PANEL_FORGOT: b = VOCAB_RGB_FORGOT_BG; line = VOCAB_RGB_FORGOT; break;
    case VOCAB_PANEL_CORRECT: b = VOCAB_RGB_CORRECT_BG; line = VOCAB_RGB_SUCCESS; break;
    case VOCAB_PANEL_NEUTRAL:
    default:
        break;
    }
    if (bg) *bg = b;
    if (border) *border = line;
}

const char *vocab_view_hint_text(vocab_hint_t kind, bool word_audio)
{
    switch (kind) {
    case VOCAB_HINT_REPLAY: return word_audio ? VOCAB_STR_HINT_LISTEN : VOCAB_STR_HINT_LOOK;
    case VOCAB_HINT_THINK: return VOCAB_STR_HINT_THINK;
    case VOCAB_HINT_FLIP: return VOCAB_STR_HINT_FLIP;
    case VOCAB_HINT_PRESS_UP: return VOCAB_STR_HINT_PRESS_UP;
    case VOCAB_HINT_PRESS_DOWN: return VOCAB_STR_HINT_PRESS_DOWN;
    case VOCAB_HINT_SUMMARY: return VOCAB_STR_HINT_SUMMARY;
    case VOCAB_HINT_NONE:
    default:
        return "";
    }
}

const char *vocab_view_toast_text(vocab_toast_t toast)
{
    switch (toast) {
    case VOCAB_TOAST_VOL_HIGH: return VOCAB_STR_TOAST_VOL_HIGH;
    case VOCAB_TOAST_VOL_LOW: return VOCAB_STR_TOAST_VOL_LOW;
    case VOCAB_TOAST_MUTE: return VOCAB_STR_TOAST_MUTE;
    case VOCAB_TOAST_NO_AUDIO: return VOCAB_STR_TOAST_NO_AUDIO;
    case VOCAB_TOAST_NONE:
    default:
        return "";
    }
}

void vocab_view_resolve(vocab_view_t *view, bool word_audio)
{
    vocab_view_hint_t *hint = &view->hint;
    if (hint->toast != VOCAB_TOAST_NONE) {
        vocab_text_copy(hint->text, sizeof(hint->text), vocab_view_toast_text(hint->toast));
        hint->color = VOCAB_RGB_TEXT;
    } else {
        vocab_text_copy(hint->text, sizeof(hint->text), vocab_view_hint_text(hint->kind, word_audio));
        hint->color = VOCAB_RGB_HINT;
    }
}
