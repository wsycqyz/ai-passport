// main/vocab_ui.c - LVGL renderer of the view model (see vocab_ui.h).
#include "vocab_ui.h"

#include <string.h>

#include "lvgl.h"
#include "vocab_config.h"
#include "vocab_fonts.h"
#include "vocab_layout.h"
#include "vocab_strings.h"

#define COLOR_UNSET 0xFFFFFFFFu
#define PANEL_BORDER 2
#define PULSE_BAR_H 4

// A label with cached style values, so unchanged values are never re-applied.
typedef struct {
    lv_obj_t *obj;
    uint32_t color;          // desired color from the view
    uint32_t applied;        // color actually applied (may be the pulse yellow)
    const lv_font_t *font;
    int32_t y;
} ui_label_t;

typedef struct {
    lv_obj_t *box;
    ui_label_t key;
    ui_label_t text;
    vocab_panel_style_t style;
    bool style_set;
    bool nudging;            // repeated nudge running
} ui_panel_t;

// Pet parts (coordinates relative to the 80x80 box, spec 11.7).
enum {
    PET_EAR_L, PET_EAR_R, PET_SHELL, PET_SPOT_1, PET_SPOT_2, PET_BODY, PET_EYE_L, PET_EYE_R,
    PET_MOUTH, PET_CHEEK_L, PET_CHEEK_R, PET_FOOT_L, PET_FOOT_R, PET_CROWN, PET_POINT_1,
    PET_POINT_2, PET_POINT_3, PET_FOOD, PET_PART_COUNT,
};

static bool s_ready;
static bool s_paused;
static bool s_first_render = true;
static volatile uint32_t s_frames;
static lv_obj_t *s_scr;
// Status bar
static ui_label_t s_progress, s_badge, s_batt_pct, s_volume, s_battery;
static uint32_t s_badge_bg = COLOR_UNSET;
// Card area
static lv_obj_t *s_card, *s_pulse_bar, *s_blank_box;
static ui_label_t s_word, s_ipa, s_meaning, s_sentence, s_example, s_s3_meaning, s_prompt, s_banner;
static ui_label_t s_pre, s_blank, s_suf;
static ui_panel_t s_panel[2];
static ui_label_t s_hint;
// HOME / SUMMARY
static lv_obj_t *s_pet, *s_xpbar;
static lv_obj_t *s_pet_part[PET_PART_COUNT];
static ui_label_t s_home_title, s_home_xp, s_home_stats, s_home_warn, s_home_go;
static ui_label_t s_sum_score, s_sum_xp, s_levelup;
// Animation bookkeeping
static vocab_view_anim_t s_seen;          // last sequence counters handled
static bool s_blinking, s_pet_idle_on;
static int s_pet_idle_stage = -1;
static bool s_pulse_active;
static ui_label_t *s_pulse_target[3];
static int32_t s_sk_text_bottom = 117;   // bottom of the skeleton text line (pulse anchor)
static int32_t s_sk_width = 200;         // width of the skeleton row (pulse bar width)
static int32_t s_pet_y = 44;
static int s_pet_stage = -1;             // growth stage currently drawn
static bool s_pet_shown;
static bool s_pet_eating;                // PET_EAT animations may be running
static bool s_recolor_pending;           // level up: new stage drawn when the jumps start
static int s_recolor_stage;

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

static lv_color_t rgb(uint32_t hex)
{
    return lv_color_hex(hex);
}

static lv_obj_t *make_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

static void fill(lv_obj_t *obj, uint32_t bg, int32_t radius)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, rgb(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
}

// Label with an explicit font; `w` > 0 gives a fixed width with centered, wrapped text.
static void make_label(ui_label_t *l, lv_obj_t *parent, const lv_font_t *font, int32_t x,
                       int32_t y, int32_t w)
{
    memset(l, 0, sizeof(*l));
    l->obj = lv_label_create(parent);
    l->color = COLOR_UNSET;
    l->applied = COLOR_UNSET;
    l->font = font;
    l->y = y;
    if (!l->obj) return;
    lv_obj_set_style_text_font(l->obj, font, 0);
    lv_label_set_text(l->obj, "");
    if (w > 0) {
        lv_obj_set_width(l->obj, w);
        lv_obj_set_style_text_align(l->obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l->obj, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_set_pos(l->obj, x, y);
    lv_obj_add_flag(l->obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_text(ui_label_t *l, const char *text)
{
    if (!l->obj) return;
    const char *now = lv_label_get_text(l->obj);
    if (!now || strcmp(now, text) != 0) lv_label_set_text(l->obj, text);
}

static bool is_pulse_target(const ui_label_t *l)
{
    for (int i = 0; i < 3; i++) {
        if (s_pulse_target[i] == l) return true;
    }
    return false;
}

static void apply_color(ui_label_t *l)
{
    if (!l->obj || l->color == COLOR_UNSET) return;
    const uint32_t want = s_pulse_active && is_pulse_target(l) ? VOCAB_RGB_YELLOW : l->color;
    if (want != l->applied) {
        lv_obj_set_style_text_color(l->obj, rgb(want), 0);
        l->applied = want;
    }
}

static void set_color(ui_label_t *l, uint32_t color)
{
    l->color = color;
    apply_color(l);
}

static void set_font(ui_label_t *l, const lv_font_t *font)
{
    if (!l->obj || l->font == font) return;
    lv_obj_set_style_text_font(l->obj, font, 0);
    l->font = font;
}

static void set_y(ui_label_t *l, int32_t y)
{
    if (!l->obj || l->y == y) return;
    lv_obj_set_y(l->obj, y);
    l->y = y;
}

static void apply_label(ui_label_t *l, const vocab_view_label_t *v)
{
    if (!l->obj) return;
    set_hidden(l->obj, !v->visible);
    if (!v->visible) return;
    set_text(l, v->text);
    set_color(l, v->color);
}

static int32_t line_height(const lv_font_t *font)
{
    return lv_font_get_line_height(font);
}

static int32_t text_width(const char *text, const lv_font_t *font)
{
    lv_point_t p = { 0, 0 };
    lv_text_get_size(&p, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

static int32_t text_height(const char *text, const lv_font_t *font, int32_t width)
{
    lv_point_t p = { 0, 0 };
    lv_text_get_size(&p, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    return p.y;
}

static const lv_font_t *word_font(vocab_word_size_t size)
{
    return size == VOCAB_WORD_SIZE_L ? VOCAB_FONT_EN_L
         : size == VOCAB_WORD_SIZE_M ? VOCAB_FONT_EN_M : VOCAB_FONT_EN_S;
}

static int32_t measure_cb(void *ctx, vocab_word_size_t size, const char *text)
{
    (void)ctx;
    return text_width(text, word_font(size));
}

static uint8_t wrap_cb(void *ctx, const char *text, int32_t wrap_px)
{
    (void)ctx;
    const int32_t lh = line_height(VOCAB_FONT_EN_S);
    const int32_t h = text_height(text, VOCAB_FONT_EN_S, wrap_px);
    return (uint8_t)(lh > 0 ? (h + lh - 1) / lh : 1);
}

// ---------------------------------------------------------------------------------------
// Animations (exec callbacks run in the LVGL task)
// ---------------------------------------------------------------------------------------

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)v, 0);
}

static void anim_border_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_width(obj, v, 0);
}

static void anim_translate_x_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x(obj, v, 0);
}

static void anim_translate_y_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, v, 0);
}

static void anim_height_cb(void *obj, int32_t v)
{
    lv_obj_set_height(obj, v);
}

static void anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x(obj, v);
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

// SHAKE: x offsets +6, -6, +6, -6, 0 in 60 ms steps (value = elapsed ms).
static void anim_shake_cb(void *obj, int32_t v)
{
    static const int8_t offsets[5] = { VOCAB_SHAKE_OFFSET_PX, -VOCAB_SHAKE_OFFSET_PX,
                                       VOCAB_SHAKE_OFFSET_PX, -VOCAB_SHAKE_OFFSET_PX, 0 };
    int32_t step = v / (int32_t)VOCAB_SHAKE_STEP_MS;
    if (step > 4) step = 4;
    if (step < 0) step = 0;
    lv_obj_set_style_translate_x(obj, offsets[step], 0);
}

static void anim_noop_cb(void *obj, int32_t v)
{
    (void)obj;
    (void)v;
}

static void start_anim_path(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                            uint32_t duration, uint32_t delay, uint32_t reverse, uint32_t repeat,
                            uint32_t repeat_delay, lv_anim_completed_cb_t done,
                            lv_anim_path_cb_t path)
{
    if (!obj || s_paused) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_delay(&a, delay);
    if (reverse) lv_anim_set_reverse_duration(&a, reverse);
    if (repeat) {
        lv_anim_set_repeat_count(&a, repeat);
        lv_anim_set_repeat_delay(&a, repeat_delay);
    }
    lv_anim_set_path_cb(&a, path);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void start_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                       uint32_t duration, uint32_t delay, uint32_t reverse, uint32_t repeat,
                       uint32_t repeat_delay, lv_anim_completed_cb_t done)
{
    start_anim_path(obj, cb, from, to, duration, delay, reverse, repeat, repeat_delay, done,
                    lv_anim_path_ease_in_out);
}

static void pulse_done(lv_anim_t *a)
{
    (void)a;
    s_pulse_active = false;
    for (int i = 0; i < 3; i++) {
        if (s_pulse_target[i]) apply_color(s_pulse_target[i]);
    }
    set_hidden(s_pulse_bar, true);
}

static void confirm_done(lv_anim_t *a)
{
    lv_obj_set_style_border_width((lv_obj_t *)a->var, PANEL_BORDER, 0);
}

static void stop_blink(void)
{
    if (!s_blinking) return;
    lv_anim_delete(s_blank_box, anim_opa_cb);
    anim_opa_cb(s_blank_box, LV_OPA_COVER);
    s_blinking = false;
}

static void stop_panel_anims(ui_panel_t *p)
{
    lv_anim_delete(p->box, anim_border_cb);
    lv_obj_set_style_border_width(p->box, PANEL_BORDER, 0);
    p->nudging = false;
}

// NUDGE every 1000 ms: border 2 -> 4 -> 2 px over 300 ms, starting now.
static void start_nudge_repeat(ui_panel_t *p)
{
    if (s_paused) return;
    start_anim(p->box, anim_border_cb, PANEL_BORDER, PANEL_BORDER * 2, VOCAB_NUDGE_MS / 2, 0,
               VOCAB_NUDGE_MS / 2, LV_ANIM_REPEAT_INFINITE, VOCAB_NUDGE_PERIOD_MS - VOCAB_NUDGE_MS,
               NULL);
    p->nudging = true;
}

static void stop_shake(void)
{
    lv_anim_delete(s_blank_box, anim_shake_cb);
    lv_obj_set_style_translate_x(s_blank_box, 0, 0);
}

static void stop_pet_idle(void)
{
    lv_anim_delete(s_pet, anim_translate_y_cb);
    lv_anim_delete(s_pet, anim_translate_x_cb);
    lv_obj_set_style_translate_y(s_pet, 0, 0);
    lv_obj_set_style_translate_x(s_pet, 0, 0);
    for (int e = PET_EYE_L; e <= PET_EYE_R; e++) {
        lv_anim_delete(s_pet_part[e], anim_height_cb);
        lv_obj_set_height(s_pet_part[e], 10);
    }
    s_pet_idle_on = false;
    s_pet_idle_stage = -1;
}

// Shows the parts and colors of a growth stage (spec 11.7).
static void apply_pet_stage(int st)
{
    static const uint32_t body[5][2] = {
        { 0, 0 }, { 0x7DD3FCu, 0x0284C7u }, { 0x86EFACu, 0x16A34Au }, { 0xC4B5FDu, 0x7C3AEDu },
        { 0xC4B5FDu, 0x7C3AEDu },
    };
    if (st < VOCAB_PET_EGG || st > VOCAB_PET_CROWN) st = VOCAB_PET_EGG;
    const bool egg = st == VOCAB_PET_EGG;
    set_hidden(s_pet_part[PET_SHELL], !egg);
    set_hidden(s_pet_part[PET_SPOT_1], !egg);
    set_hidden(s_pet_part[PET_SPOT_2], !egg);
    for (int i = PET_BODY; i <= PET_FOOT_R; i++) set_hidden(s_pet_part[i], egg);
    set_hidden(s_pet_part[PET_CHEEK_L], st < VOCAB_PET_CHILD);
    set_hidden(s_pet_part[PET_CHEEK_R], st < VOCAB_PET_CHILD);
    set_hidden(s_pet_part[PET_EAR_L], st < VOCAB_PET_GROWN);
    set_hidden(s_pet_part[PET_EAR_R], st < VOCAB_PET_GROWN);
    for (int i = PET_CROWN; i <= PET_POINT_3; i++) set_hidden(s_pet_part[i], st != VOCAB_PET_CROWN);
    if (!egg) {
        const uint32_t fill_c = body[st][0];
        const uint32_t line_c = body[st][1];
        lv_obj_set_style_bg_color(s_pet_part[PET_BODY], rgb(fill_c), 0);
        lv_obj_set_style_border_color(s_pet_part[PET_BODY], rgb(line_c), 0);
        lv_obj_set_style_bg_color(s_pet_part[PET_EAR_L], rgb(fill_c), 0);
        lv_obj_set_style_bg_color(s_pet_part[PET_EAR_R], rgb(fill_c), 0);
        lv_obj_set_style_bg_color(s_pet_part[PET_FOOT_L], rgb(line_c), 0);
        lv_obj_set_style_bg_color(s_pet_part[PET_FOOT_R], rgb(line_c), 0);
    }
    s_pet_stage = st;
}

// Ends PET_EAT at once (pet hidden, SUMMARY left, screen off) and applies a pending
// level-up recolor so HOME always shows the current stage.
static void stop_pet_eat(void)
{
    if (s_pet_eating) {
        lv_anim_delete(s_pet_part[PET_FOOD], anim_x_cb);
        lv_anim_delete(s_pet_part[PET_FOOD], anim_y_cb);
        set_hidden(s_pet_part[PET_FOOD], true);
        lv_anim_delete(s_pet_part[PET_MOUTH], anim_height_cb);
        lv_obj_set_height(s_pet_part[PET_MOUTH], 4);
        lv_anim_delete(s_pet, anim_noop_cb);
        lv_anim_delete(s_pet, anim_translate_y_cb);
        lv_obj_set_style_translate_y(s_pet, 0, 0);
        s_pet_eating = false;
    }
    if (s_recolor_pending) {
        s_recolor_pending = false;
        apply_pet_stage(s_recolor_stage);
    }
}

static void stop_all(void)
{
    stop_blink();
    stop_shake();
    for (int i = 0; i < 2; i++) stop_panel_anims(&s_panel[i]);
    stop_pet_idle();
    stop_pet_eat();
    lv_anim_delete(s_pulse_bar, anim_noop_cb);
    pulse_done(NULL);
}

// ---------------------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------------------

static void refr_ready_cb(lv_event_t *e)
{
    (void)e;
    s_frames = s_frames + 1u;
}

static void make_pet(void)
{
    s_pet = make_box(s_scr, 80, 44, 80, 80);
    if (!s_pet) return;
    lv_obj_add_flag(s_pet, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // food flies in from outside
    static const struct {
        int16_t x, y, w, h, r;
    } geo[PET_PART_COUNT] = {
        [PET_EAR_L] = { 16, 18, 10, 12, 3 },  [PET_EAR_R] = { 54, 18, 10, 12, 3 },
        [PET_SHELL] = { 16, 14, 48, 60, 24 }, [PET_SPOT_1] = { 28, 30, 8, 8, 4 },
        [PET_SPOT_2] = { 46, 48, 8, 8, 4 },   [PET_BODY] = { 12, 26, 56, 46, 18 },
        [PET_EYE_L] = { 27, 40, 8, 10, 4 },   [PET_EYE_R] = { 45, 40, 8, 10, 4 },
        [PET_MOUTH] = { 34, 56, 12, 4, 2 },   [PET_CHEEK_L] = { 19, 52, 8, 5, 2 },
        [PET_CHEEK_R] = { 53, 52, 8, 5, 2 },  [PET_FOOT_L] = { 22, 70, 12, 6, 3 },
        [PET_FOOT_R] = { 46, 70, 12, 6, 3 },  [PET_CROWN] = { 27, 14, 26, 8, 0 },
        [PET_POINT_1] = { 27, 8, 6, 6, 0 },   [PET_POINT_2] = { 37, 6, 6, 6, 0 },
        [PET_POINT_3] = { 47, 8, 6, 6, 0 },   [PET_FOOD] = { 100, 40, 8, 8, 0 },
    };
    for (int i = 0; i < PET_PART_COUNT; i++) {
        s_pet_part[i] = make_box(s_pet, geo[i].x, geo[i].y, geo[i].w, geo[i].h);
        fill(s_pet_part[i], 0x0F172Au, geo[i].r);
    }
    fill(s_pet_part[PET_SHELL], 0xFDE68Au, 24);
    lv_obj_set_style_border_color(s_pet_part[PET_SHELL], rgb(0xF59E0Bu), 0);
    lv_obj_set_style_border_width(s_pet_part[PET_SHELL], 2, 0);
    fill(s_pet_part[PET_SPOT_1], 0xF59E0Bu, 4);
    fill(s_pet_part[PET_SPOT_2], 0xF59E0Bu, 4);
    lv_obj_set_style_border_width(s_pet_part[PET_BODY], 2, 0);
    fill(s_pet_part[PET_CHEEK_L], 0xF9A8D4u, 2);
    fill(s_pet_part[PET_CHEEK_R], 0xF9A8D4u, 2);
    for (int i = PET_CROWN; i <= PET_POINT_3; i++) fill(s_pet_part[i], VOCAB_RGB_GOLD, 0);
    fill(s_pet_part[PET_FOOD], 0xF97316u, 1);
    set_hidden(s_pet_part[PET_FOOD], true);
}

bool vocab_ui_create(void)
{
    s_scr = lv_obj_create(NULL);
    if (!s_scr) return false;
    lv_obj_remove_style_all(s_scr);
    fill(s_scr, VOCAB_RGB_SCREEN_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Status bar (texts with y < 30 stay within x 16..224).
    make_label(&s_progress, s_scr, VOCAB_FONT_ZH_S, 20, 6, 0);
    make_label(&s_badge, s_scr, VOCAB_FONT_ZH_S, 0, 0, 0);
    if (s_badge.obj) {
        // Pill h 22 (1 px border + padding around one F_ZH_S line); a taller line keeps the
        // content height instead of clipping glyphs.
        const int32_t lh = line_height(VOCAB_FONT_ZH_S);
        const int32_t pad_v = lh < 20 ? (20 - lh) / 2 : 0;
        lv_obj_set_style_radius(s_badge.obj, 11, 0);
        lv_obj_set_style_border_width(s_badge.obj, 1, 0);
        lv_obj_set_style_bg_opa(s_badge.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_badge.obj, 10, 0);
        lv_obj_set_style_pad_ver(s_badge.obj, pad_v, 0);
        if (lh <= 20) lv_obj_set_height(s_badge.obj, 22);
        lv_obj_set_style_max_width(s_badge.obj, 104, 0);
        lv_label_set_long_mode(s_badge.obj, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_align(s_badge.obj, LV_ALIGN_TOP_MID, 0, 5);
    }
    make_label(&s_batt_pct, s_scr, VOCAB_FONT_SYM, 136, 8, 36);
    if (s_batt_pct.obj) lv_obj_set_style_text_align(s_batt_pct.obj, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(&s_volume, s_scr, VOCAB_FONT_SYM, 176, 8, 18);
    make_label(&s_battery, s_scr, VOCAB_FONT_SYM, 198, 8, 24);

    // Card area (labels are screen children so they use screen coordinates).
    s_card = make_box(s_scr, 10, 34, 220, 120);
    fill(s_card, VOCAB_RGB_CARD_BG, 10);
    if (s_card) {
        lv_obj_set_style_border_color(s_card, rgb(VOCAB_RGB_CARD_BORDER), 0);
        lv_obj_set_style_border_width(s_card, 2, 0);
    }
    make_label(&s_word, s_scr, VOCAB_FONT_EN_L, 20, 40, 200);
    s_pulse_bar = make_box(s_scr, 20, 88, 200, PULSE_BAR_H);
    fill(s_pulse_bar, VOCAB_RGB_YELLOW, 2);
    set_hidden(s_pulse_bar, true);
    make_label(&s_ipa, s_scr, VOCAB_FONT_IPA, 20, 92, 200);
    make_label(&s_meaning, s_scr, VOCAB_FONT_ZH_L, 20, 90, 200);
    make_label(&s_sentence, s_scr, VOCAB_FONT_EN_S, 20, 44, 200);
    make_label(&s_example, s_scr, VOCAB_FONT_EN_XS, 20, 120, 200);
    make_label(&s_s3_meaning, s_scr, VOCAB_FONT_ZH_L, 20, 40, 200);
    make_label(&s_pre, s_scr, VOCAB_FONT_EN_L, 20, 80, 0);
    s_blank_box = make_box(s_scr, 100, 76, 40, 46);
    fill(s_blank_box, VOCAB_RGB_BLANK_BG, 4);
    if (s_blank_box) {
        lv_obj_set_style_border_color(s_blank_box, rgb(VOCAB_RGB_BLANK_LINE), 0);
        lv_obj_set_style_border_width(s_blank_box, 3, 0);
        lv_obj_set_style_border_side(s_blank_box, LV_BORDER_SIDE_BOTTOM, 0);
        set_hidden(s_blank_box, true);
        make_label(&s_blank, s_blank_box, VOCAB_FONT_EN_L, 0, 0, 0);
        if (s_blank.obj) {
            lv_obj_center(s_blank.obj);
            set_hidden(s_blank.obj, false);
        }
    }
    make_label(&s_suf, s_scr, VOCAB_FONT_EN_L, 140, 80, 0);
    make_label(&s_prompt, s_scr, VOCAB_FONT_ZH_S, 20, 124, 200);
    make_label(&s_banner, s_scr, VOCAB_FONT_ZH_L, 20, 56, 200);

    // Option panels: glyph at panel x + 14, text centered in panel x 36..216.
    for (int i = 0; i < 2; i++) {
        ui_panel_t *p = &s_panel[i];
        p->box = make_box(s_scr, 10, i == 0 ? 160 : 224, 220, 56);
        fill(p->box, VOCAB_RGB_CARD_BG, 10);
        if (!p->box) continue;
        lv_obj_set_style_border_width(p->box, PANEL_BORDER, 0);
        set_hidden(p->box, true);
        make_label(&p->key, p->box, VOCAB_FONT_ZH_L, 14, 0, 0);
        make_label(&p->text, p->box, VOCAB_FONT_ZH_L, 36, 0, 180);
        if (p->key.obj) {
            lv_label_set_text(p->key.obj, i == 0 ? VOCAB_STR_KEY_UP : VOCAB_STR_KEY_DOWN);
            lv_obj_align(p->key.obj, LV_ALIGN_LEFT_MID, 14, 0);
            set_hidden(p->key.obj, false);
        }
        if (p->text.obj) {
            lv_label_set_long_mode(p->text.obj, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(p->text.obj, LV_ALIGN_LEFT_MID, 36, 0);
            set_hidden(p->text.obj, false);
        }
    }
    make_label(&s_hint, s_scr, VOCAB_FONT_ZH_S, 20, 290, 200);

    // HOME / SUMMARY.
    make_pet();
    make_label(&s_home_title, s_scr, VOCAB_FONT_ZH_L, 20, 134, 200);
    s_xpbar = lv_bar_create(s_scr);
    if (s_xpbar) {
        lv_obj_remove_style_all(s_xpbar);
        lv_obj_set_pos(s_xpbar, 40, 170);
        lv_obj_set_size(s_xpbar, 160, 10);
        fill(s_xpbar, VOCAB_RGB_CARD_BORDER, 5);
        lv_obj_set_style_radius(s_xpbar, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_xpbar, rgb(VOCAB_RGB_HINT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_xpbar, LV_OPA_COVER, LV_PART_INDICATOR);
        set_hidden(s_xpbar, true);
    }
    make_label(&s_home_xp, s_scr, VOCAB_FONT_ZH_S, 20, 184, 200);
    make_label(&s_home_stats, s_scr, VOCAB_FONT_ZH_S, 20, 212, 200);
    make_label(&s_home_warn, s_scr, VOCAB_FONT_ZH_S, 20, 240, 200);
    make_label(&s_home_go, s_scr, VOCAB_FONT_ZH_L, 20, 286, 200);
    make_label(&s_sum_score, s_scr, VOCAB_FONT_ZH_L, 20, 92, 200);
    make_label(&s_sum_xp, s_scr, VOCAB_FONT_ZH_S, 20, 124, 200);
    make_label(&s_levelup, s_scr, VOCAB_FONT_ZH_S, 20, 266, 200);

    const lv_obj_t *const created[] = {
        s_progress.obj, s_badge.obj, s_batt_pct.obj, s_volume.obj, s_battery.obj, s_card,
        s_word.obj, s_pulse_bar, s_ipa.obj, s_meaning.obj, s_sentence.obj, s_example.obj,
        s_s3_meaning.obj, s_pre.obj, s_blank_box, s_blank.obj, s_suf.obj, s_prompt.obj,
        s_banner.obj, s_panel[0].box, s_panel[0].key.obj, s_panel[0].text.obj, s_panel[1].box,
        s_panel[1].key.obj, s_panel[1].text.obj, s_hint.obj, s_pet, s_home_title.obj, s_xpbar,
        s_home_xp.obj, s_home_stats.obj, s_home_warn.obj, s_home_go.obj, s_sum_score.obj,
        s_sum_xp.obj, s_levelup.obj,
    };
    for (size_t i = 0; i < sizeof(created) / sizeof(created[0]); i++) {
        if (!created[i]) return false;  // LVGL pool exhausted: never render a partial tree
    }
    for (int i = 0; i < PET_PART_COUNT; i++) {
        if (!s_pet_part[i]) return false;
    }
    set_hidden(s_card, true);
    set_hidden(s_pet, true);
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_add_event_cb(disp, refr_ready_cb, LV_EVENT_REFR_READY, NULL);
    lv_screen_load(s_scr);
    s_ready = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------

static const char *volume_symbol(vocab_vol_icon_t icon)
{
    switch (icon) {
    case VOCAB_VOL_ICON_HIGH: return LV_SYMBOL_VOLUME_MAX;
    case VOCAB_VOL_ICON_LOW: return LV_SYMBOL_VOLUME_MID;
    case VOCAB_VOL_ICON_MUTE:
    case VOCAB_VOL_ICON_NO_AUDIO:
    default:
        return LV_SYMBOL_MUTE;
    }
}

static const char *battery_symbol(vocab_batt_icon_t icon)
{
    switch (icon) {
    case VOCAB_BATT_ICON_FULL: return LV_SYMBOL_BATTERY_FULL;
    case VOCAB_BATT_ICON_3: return LV_SYMBOL_BATTERY_3;
    case VOCAB_BATT_ICON_2: return LV_SYMBOL_BATTERY_2;
    case VOCAB_BATT_ICON_1: return LV_SYMBOL_BATTERY_1;
    case VOCAB_BATT_ICON_EMPTY:
    case VOCAB_BATT_ICON_UNKNOWN:
    default:
        return LV_SYMBOL_BATTERY_EMPTY;
    }
}

static void render_status(const vocab_view_t *v)
{
    apply_label(&s_progress, &v->status.progress);
    apply_label(&s_batt_pct, &v->status.batt_pct);
    set_hidden(s_volume.obj, false);
    set_text(&s_volume, volume_symbol(v->status.volume));
    set_color(&s_volume, v->status.volume == VOCAB_VOL_ICON_MUTE ? VOCAB_RGB_WARN
                         : v->status.volume == VOCAB_VOL_ICON_NO_AUDIO ? VOCAB_RGB_PANEL_BORDER
                                                                       : VOCAB_RGB_DIM);
    set_hidden(s_battery.obj, false);
    set_text(&s_battery, battery_symbol(v->status.battery));
    set_color(&s_battery, v->status.battery == VOCAB_BATT_ICON_UNKNOWN ? VOCAB_RGB_PANEL_BORDER
                          : v->status.battery_low ? VOCAB_RGB_LOW_BATTERY : VOCAB_RGB_DIM);

    set_hidden(s_badge.obj, v->badge == VOCAB_BADGE_NONE);
    if (v->badge != VOCAB_BADGE_NONE) {
        uint32_t accent = 0;
        uint32_t bg = 0;
        vocab_view_badge_colors(v->badge, &accent, &bg);
        set_text(&s_badge, vocab_view_badge_text(v->badge));
        if (accent != s_badge.color) lv_obj_set_style_border_color(s_badge.obj, rgb(accent), 0);
        set_color(&s_badge, accent);
        if (bg != s_badge_bg) {
            lv_obj_set_style_bg_color(s_badge.obj, rgb(bg), 0);
            s_badge_bg = bg;
        }
    }
}

// Word label: font by the spec 11.3 rule. Returns the resting y (40; 38 for two lines at
// F_EN_S); layout_card_text() applies the final y once.
static int32_t render_word(const vocab_view_label_t *w)
{
    apply_label(&s_word, w);
    if (!w->visible) return s_word.y;
    const vocab_word_fit_t fit = vocab_layout_word_size(w->text, measure_cb, wrap_cb, NULL);
    set_font(&s_word, word_font(fit.size));
    return fit.lines > 1 ? 38 : 40;
}

static void render_skeleton(const vocab_view_skeleton_t *sk)
{
    const bool was_shown = !lv_obj_has_flag(s_blank_box, LV_OBJ_FLAG_HIDDEN);
    set_hidden(s_pre.obj, !sk->visible);
    set_hidden(s_suf.obj, !sk->visible);
    set_hidden(s_blank_box, !sk->visible);
    if (!sk->visible) {
        if (was_shown) {
            stop_blink();
            stop_shake();
        }
        return;
    }
    const vocab_word_size_t size =
        vocab_layout_skeleton_size(sk->pre, sk->right, sk->other, sk->suf, measure_cb, NULL);
    const lv_font_t *font = word_font(size);
    set_font(&s_pre, font);
    set_font(&s_suf, font);
    set_font(&s_blank, font);
    set_text(&s_pre, sk->pre);
    set_text(&s_suf, sk->suf);
    set_text(&s_blank, sk->filled ? sk->right : "");
    set_color(&s_pre, VOCAB_RGB_TEXT);
    set_color(&s_suf, VOCAB_RGB_TEXT);
    set_color(&s_blank, VOCAB_RGB_SUCCESS);
    const vocab_skeleton_geom_t g = vocab_layout_skeleton(
        text_width(sk->pre, font), text_width(sk->right, font), text_width(sk->other, font),
        text_width(sk->suf, font), VOCAB_SCREEN_W / 2);
    const int32_t row_y = 76;
    const int32_t row_h = 46;
    const int32_t text_y = row_y + (row_h - line_height(font)) / 2;
    s_sk_text_bottom = text_y + line_height(font);
    s_sk_width = g.total_w;
    // Positions change only per card or font: re-apply only then (no redraw per render).
    static int32_t applied[4] = { -1, -1, -1, -1 };
    const int32_t want[4] = { g.pre_x, g.suf_x, g.box_x, (g.box_w << 16) | text_y };
    if (memcmp(applied, want, sizeof(want)) != 0) {
        lv_obj_set_pos(s_pre.obj, g.pre_x, text_y);
        lv_obj_set_pos(s_suf.obj, g.suf_x, text_y);
        lv_obj_set_pos(s_blank_box, g.box_x, row_y);
        lv_obj_set_size(s_blank_box, g.box_w, row_h);
        memcpy(applied, want, sizeof(want));
    }
    if (sk->blink && !sk->filled && !s_paused && !s_blinking) {
        start_anim(s_blank_box, anim_opa_cb, LV_OPA_COVER, LV_OPA_30, VOCAB_BLINK_PERIOD_MS, 0,
                   VOCAB_BLINK_PERIOD_MS, LV_ANIM_REPEAT_INFINITE, 0, NULL);
        s_blinking = true;
    } else if ((!sk->blink || sk->filled) && s_blinking) {
        stop_blink();
    }
}

// Stage 2 back: word, meaning and a possibly two-line example stacked inside the card.
static void layout_card_text(const vocab_view_t *v, int32_t word_y)
{
    if (v->card.meaning.visible && v->card.example.visible && v->card.word.visible) {
        const int32_t pref[3] = { word_y, 90, 120 };
        const int32_t h[3] = {
            text_height(v->card.word.text, s_word.font, 200),
            line_height(VOCAB_FONT_ZH_L),
            text_height(v->card.example.text, VOCAB_FONT_EN_XS, 200),
        };
        int32_t y[3];
        vocab_layout_stack(pref, h, 3, 36, 152, y);
        set_y(&s_word, y[0]);
        set_y(&s_meaning, y[1]);
        set_y(&s_example, y[2]);
    } else {
        if (v->card.word.visible) set_y(&s_word, word_y);
        set_y(&s_meaning, 90);
        set_y(&s_example, 120);
    }
}

static void render_panels(const vocab_view_t *v)
{
    for (int i = 0; i < 2; i++) {
        const vocab_view_panel_t *pv = &v->panel[i];
        ui_panel_t *p = &s_panel[i];
        const bool was_shown = !lv_obj_has_flag(p->box, LV_OBJ_FLAG_HIDDEN);
        set_hidden(p->box, !pv->visible);
        if (!pv->visible) {
            if (was_shown) stop_panel_anims(p);  // also ends one-shot nudge / confirm
            continue;
        }
        if (!p->style_set || p->style != pv->style) {
            uint32_t bg = 0;
            uint32_t border = 0;
            vocab_view_panel_colors(pv->style, &bg, &border);
            lv_obj_set_style_bg_color(p->box, rgb(bg), 0);
            lv_obj_set_style_border_color(p->box, rgb(border), 0);
            set_color(&p->key, border);
            p->style = pv->style;
            p->style_set = true;
        }
        set_font(&p->text, pv->text_en ? VOCAB_FONT_EN_M : VOCAB_FONT_ZH_L);
        set_text(&p->text, pv->text);
        set_color(&p->text, VOCAB_RGB_TEXT);
        if (pv->nudge_repeat && !p->nudging) {
            start_nudge_repeat(p);
        } else if (!pv->nudge_repeat && p->nudging) {
            stop_panel_anims(p);
        }
    }
}

static void render_pet(const vocab_view_pet_t *pet, bool eat_starts)
{
    set_hidden(s_pet, !pet->visible);
    if (!pet->visible) {
        if (s_pet_shown) {
            stop_pet_idle();
            stop_pet_eat();
            s_pet_shown = false;
        }
        return;
    }
    s_pet_shown = true;
    const int32_t y = pet->on_summary ? 162 : 44;
    if (y != s_pet_y) {
        if (!pet->on_summary) stop_pet_eat();  // SUMMARY left: finish the level-up recolor
        lv_obj_set_y(s_pet, y);
        s_pet_y = y;
    }
    // PET_EAT after a level up keeps the old look until the jumps start (start_pet_eat).
    if (eat_starts && pet->level_up && !s_paused && s_pet_stage >= 0 &&
        (int)pet->stage != s_pet_stage) {
        s_recolor_pending = true;
        s_recolor_stage = (int)pet->stage;
    }
    if (!s_recolor_pending && (int)pet->stage != s_pet_stage) apply_pet_stage((int)pet->stage);
    // PET_IDLE on HOME: bob y +/-3 px (egg: x wobble +/-2 px), blink every 4 s.
    const bool want_idle = pet->idle && !s_paused;
    if (want_idle && (!s_pet_idle_on || s_pet_idle_stage != s_pet_stage)) {
        if (s_pet_idle_on) stop_pet_idle();
        const uint32_t half = VOCAB_PET_IDLE_PERIOD_MS / 2;
        if (s_pet_stage == VOCAB_PET_EGG) {
            start_anim(s_pet, anim_translate_x_cb, -2, 2, half, 0, half, LV_ANIM_REPEAT_INFINITE, 0, NULL);
        } else {
            start_anim(s_pet, anim_translate_y_cb, -3, 3, half, 0, half, LV_ANIM_REPEAT_INFINITE, 0, NULL);
            for (int e = PET_EYE_L; e <= PET_EYE_R; e++) {
                start_anim(s_pet_part[e], anim_height_cb, 10, 2, 80, VOCAB_PET_BLINK_PERIOD_MS, 80,
                           LV_ANIM_REPEAT_INFINITE, VOCAB_PET_BLINK_PERIOD_MS - 160, NULL);
            }
        }
        s_pet_idle_on = true;
        s_pet_idle_stage = s_pet_stage;
    } else if (!want_idle && s_pet_idle_on) {
        stop_pet_idle();
    }
}

static void food_done(lv_anim_t *a)
{
    (void)a;
    set_hidden(s_pet_part[PET_FOOD], true);
}

static void recolor_done(lv_anim_t *a)
{
    (void)a;
    if (s_recolor_pending) {
        s_recolor_pending = false;
        apply_pet_stage(s_recolor_stage);
    }
}

// PET_EAT on SUMMARY: food flies to the mouth (400 ms), the mouth chews twice (300 ms),
// then two jumps of -12 px; after a level up the body recolors as three jumps begin.
static void start_pet_eat(bool level_up)
{
    if (s_paused) return;
    s_pet_eating = true;
    lv_obj_t *food = s_pet_part[PET_FOOD];
    lv_obj_set_pos(food, 100, 40);
    set_hidden(food, false);
    start_anim(food, anim_x_cb, 100, 36, 400, 0, 0, 0, 0, food_done);
    start_anim(food, anim_y_cb, 40, 54, 400, 0, 0, 0, 0, NULL);
    if (s_pet_stage != VOCAB_PET_EGG) {
        start_anim(s_pet_part[PET_MOUTH], anim_height_cb, 4, 8, 75, 400, 75, 2, 0, NULL);
    }
    if (s_recolor_pending) start_anim(s_pet, anim_noop_cb, 0, 1, 1, 699, 0, 0, 0, recolor_done);
    start_anim(s_pet, anim_translate_y_cb, 0, -12, 150, 700, 150, level_up ? 3 : 2, 0, NULL);
}

static void start_pulse(const vocab_view_t *v)
{
    // WORD_PULSE on the visible English element: yellow text + 4 px underline bar 2 px under
    // the text for 600 ms (spec 11.4, 11.6).
    memset(s_pulse_target, 0, sizeof(s_pulse_target));
    int32_t bottom = -1;
    int32_t width = 0;
    if (v->card.word.visible || v->card.sentence.visible) {
        ui_label_t *l = v->card.word.visible ? &s_word : &s_sentence;
        s_pulse_target[0] = l;
        lv_obj_update_layout(l->obj);
        bottom = lv_obj_get_y(l->obj) + lv_obj_get_height(l->obj);
        width = v->card.word.visible ? text_width(v->card.word.text, s_word.font) : 200;
    } else if (v->card.skeleton.visible) {
        // Under the skeleton text line, not the 46 px box: the bar must end above the
        // prompt line at y 124.
        s_pulse_target[0] = &s_pre;
        s_pulse_target[1] = &s_suf;
        bottom = s_sk_text_bottom;
        width = s_sk_width;
    }
    if (bottom < 0 || s_paused) return;
    if (width > 200) width = 200;
    lv_obj_set_pos(s_pulse_bar, VOCAB_SCREEN_W / 2 - width / 2, bottom + 2);
    lv_obj_set_width(s_pulse_bar, width > 0 ? width : 1);
    set_hidden(s_pulse_bar, false);
    s_pulse_active = true;
    for (int i = 0; i < 3; i++) {
        if (s_pulse_target[i]) apply_color(s_pulse_target[i]);
    }
    lv_anim_delete(s_pulse_bar, anim_noop_cb);
    start_anim(s_pulse_bar, anim_noop_cb, 0, 1, VOCAB_WORD_PULSE_MS, 0, 0, 0, 0, pulse_done);
}

static void run_one_shots(const vocab_view_t *v)
{
    const vocab_view_anim_t *a = &v->anim;
    if (s_first_render) {
        s_seen = *a;  // never replay animations that happened before the first frame
        s_first_render = false;
        return;
    }
    if (a->shake_seq != s_seen.shake_seq && v->card.skeleton.visible) {
        // Linear time base: the callback maps elapsed ms to the 60 ms offset steps.
        lv_anim_delete(s_blank_box, anim_shake_cb);
        start_anim_path(s_blank_box, anim_shake_cb, 0, 5 * (int32_t)VOCAB_SHAKE_STEP_MS,
                        5 * VOCAB_SHAKE_STEP_MS, 0, 0, 0, 0, NULL, lv_anim_path_linear);
    }
    if (a->pulse_seq != s_seen.pulse_seq) start_pulse(v);
    for (int i = 0; i < 2; i++) {
        ui_panel_t *p = &s_panel[i];
        if (a->nudge_seq[i] != s_seen.nudge_seq[i] && v->panel[i].visible) {
            if (p->nudging) {
                start_nudge_repeat(p);  // wrong key in the hold wait: nudge now, then every 1 s
            } else {
                start_anim(p->box, anim_border_cb, PANEL_BORDER, PANEL_BORDER * 2, VOCAB_NUDGE_MS / 2,
                           0, VOCAB_NUDGE_MS / 2, 0, 0, NULL);
            }
        }
        if (a->confirm_seq[i] != s_seen.confirm_seq[i] && v->panel[i].visible) {
            lv_anim_delete(p->box, anim_border_cb);
            p->nudging = false;
            start_anim(p->box, anim_border_cb, PANEL_BORDER * 2, PANEL_BORDER * 2, VOCAB_S2_CONFIRM_MS,
                       0, 0, 0, 0, confirm_done);
        }
    }
    if (a->pet_seq != s_seen.pet_seq && v->pet.visible && v->pet.on_summary) {
        start_pet_eat(v->pet.level_up);
    }
    s_seen = *a;
}

void vocab_ui_render(const vocab_view_t *v)
{
    if (!s_ready || !v) return;
    render_status(v);

    set_hidden(s_card, !v->card.frame);
    const int32_t word_y = render_word(&v->card.word);
    apply_label(&s_ipa, &v->card.ipa);
    apply_label(&s_meaning, &v->card.meaning);
    apply_label(&s_sentence, &v->card.sentence);
    apply_label(&s_example, &v->card.example);
    apply_label(&s_s3_meaning, &v->card.s3_meaning);
    apply_label(&s_prompt, &v->card.prompt);
    apply_label(&s_banner, &v->card.banner);
    layout_card_text(v, word_y);
    render_skeleton(&v->card.skeleton);
    render_panels(v);

    set_hidden(s_hint.obj, v->hint.text[0] == '\0');
    set_text(&s_hint, v->hint.text);
    set_color(&s_hint, v->hint.color);

    apply_label(&s_home_title, &v->home.title);
    apply_label(&s_home_xp, &v->home.xp);
    apply_label(&s_home_stats, &v->home.stats);
    apply_label(&s_home_warn, &v->home.warn);
    apply_label(&s_home_go, &v->home.go);
    apply_label(&s_sum_score, &v->summary.score);
    apply_label(&s_sum_xp, &v->summary.xp);
    apply_label(&s_levelup, &v->summary.levelup);

    set_hidden(s_xpbar, !v->xpbar.visible);
    if (v->xpbar.visible) {
        static int32_t xpbar_y = 170;  // creation position
        const int32_t y = v->xpbar.on_summary ? 252 : 170;
        if (y != xpbar_y) {
            lv_obj_set_y(s_xpbar, y);
            xpbar_y = y;
        }
        lv_bar_set_range(s_xpbar, 0, v->xpbar.max > 0 ? v->xpbar.max : 1);
        lv_bar_set_value(s_xpbar, v->xpbar.value, LV_ANIM_OFF);
    }
    const bool eat_starts = !s_first_render && v->anim.pet_seq != s_seen.pet_seq &&
                            v->pet.visible && v->pet.on_summary;
    render_pet(&v->pet, eat_starts);
    run_one_shots(v);
    if (s_pulse_active && !v->card.word.visible && !v->card.sentence.visible &&
        !v->card.skeleton.visible) {
        lv_anim_delete(s_pulse_bar, anim_noop_cb);
        pulse_done(NULL);  // the pulsed element went away
    }
}

void vocab_ui_set_paused(bool paused)
{
    if (!s_ready || paused == s_paused) return;
    if (paused) stop_all();
    s_paused = paused;
}

uint32_t vocab_ui_frame_count(void)
{
    return s_frames;
}
