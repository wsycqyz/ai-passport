// main/demo_math.c —— 小学两位数加减乘除答题页(开机即进入)。
// 题目与判分逻辑在 math_quiz.c;本文件只负责渲染与按键。
// 上/下:移动高亮(四选项按线性顺序,与主菜单一致);确定:提交。
// 答对提示 CORRECT 并等待按键进入下一题;答错提示 TRY AGAIN 并可重选,
// 再次答错则揭示答案并等待按键进入下一题。长按“确定”仍可返回演示菜单。
#include "demo.h"
#include "math_quiz.h"
#include "bsp_battery.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <stdbool.h>

#define COL_GREEN    0x2E9E48   // 正确文字/边框
#define COL_GREEN_BG 0xCDEBB0   // 正确卡片底色
#define COL_RED_BG   0xF3C6C2   // 错误卡片底色

static lv_obj_t   *s_scr;
static lv_obj_t   *s_equation;
static lv_obj_t   *s_feedback;
static lv_obj_t   *s_hint;
static lv_obj_t   *s_battery;
static lv_obj_t   *s_mascot;
static lv_obj_t   *s_cards[MATH_QUIZ_CHOICES];
static lv_obj_t   *s_choice[MATH_QUIZ_CHOICES];
static lv_timer_t *s_batt_timer;
static quiz_game_t s_game;

// 右上角电量(避开白云,读值为 -1 时留空)。跑在 LVGL 任务里可直接操作对象。
static void battery_refresh(void)
{
    if (!s_battery) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_battery, "");
        return;
    }
    lv_label_set_text_fmt(s_battery, "%d%%", soc);
    lv_obj_set_style_text_color(s_battery,
        lv_color_hex(soc < 20 ? UI_RED : 0xFFFFFF), 0);
}

static void batt_tick(lv_timer_t *t)
{
    (void)t;
    battery_refresh();
}

static void render(void)
{
    const math_question_t *q = &s_game.q;
    char op = math_op_symbol(q->op);

    if (s_game.state == QUIZ_SELECTING) {
        lv_label_set_text_fmt(s_equation, "%d %c %d = ?", q->a, op, q->b);
    } else {
        lv_label_set_text_fmt(s_equation, "%d %c %d = %d", q->a, op, q->b, q->answer);
    }

    for (int i = 0; i < MATH_QUIZ_CHOICES; i++) {
        lv_label_set_text_fmt(s_choice[i], "%d", q->choices[i]);
        bool selected = (s_game.state == QUIZ_SELECTING) && (i == s_game.selected);
        ui_pixel_set_selected(s_cards[i], selected, true);

        bool mark_right = (i == q->correct_index) &&
                          (s_game.state == QUIZ_CORRECT || s_game.state == QUIZ_REVEAL);
        bool mark_wrong = (i == s_game.last_pick) && (i != q->correct_index) &&
                          (s_game.wrong_count > 0);
        if (mark_right) {
            lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(COL_GREEN_BG), 0);
            lv_obj_set_style_border_color(s_cards[i], lv_color_hex(COL_GREEN), 0);
        } else if (mark_wrong) {
            lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(COL_RED_BG), 0);
            lv_obj_set_style_border_color(s_cards[i], lv_color_hex(UI_RED), 0);
        }
    }

    switch (s_game.state) {
    case QUIZ_CORRECT:
        lv_label_set_text(s_feedback, "CORRECT!");
        lv_obj_set_style_text_color(s_feedback, lv_color_hex(COL_GREEN), 0);
        lv_label_set_text(s_hint, "PRESS ANY KEY");
        break;
    case QUIZ_REVEAL:
        lv_label_set_text_fmt(s_feedback, "ANSWER: %d", q->answer);
        lv_obj_set_style_text_color(s_feedback, lv_color_hex(UI_RED), 0);
        lv_label_set_text(s_hint, "PRESS ANY KEY");
        break;
    default:  // QUIZ_SELECTING
        if (s_game.wrong_count > 0) {
            lv_label_set_text(s_feedback, "TRY AGAIN");
            lv_obj_set_style_text_color(s_feedback, lv_color_hex(UI_RED), 0);
        } else {
            lv_label_set_text(s_feedback, "CHOOSE");
            lv_obj_set_style_text_color(s_feedback, lv_color_hex(UI_INK), 0);
        }
        lv_label_set_text(s_hint, "UP/DN MOVE   OK PICK");
        break;
    }
}

void demo_math_enter(void)
{
    quiz_game_init(&s_game, esp_random() ^ (uint32_t)esp_timer_get_time());

    s_scr = ui_pixel_screen_create("MATH");

    // 右上角电量:白云约在 x>=188、y<=25,放到其下方的空闲蓝天区。
    s_battery = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -6, 28);
    lv_label_set_text(s_battery, "");

    lv_obj_t *qp = ui_pixel_panel_create(s_scr, 16, 50, 208, 46, UI_PAPER);
    s_equation = lv_label_create(qp);
    lv_obj_set_style_text_font(s_equation, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_equation, lv_color_hex(UI_INK), 0);
    lv_obj_center(s_equation);

    // 四个选项 2x2 网格,和主菜单一样用上/下线性移动高亮。
    for (int i = 0; i < MATH_QUIZ_CHOICES; i++) {
        int x = 20 + (i % 2) * 102;
        int y = 108 + (i / 2) * 50;
        s_cards[i] = ui_pixel_panel_create(s_scr, x, y, 94, 44, UI_PAPER);
        s_choice[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_choice[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_choice[i], lv_color_hex(UI_INK), 0);
        lv_obj_center(s_choice[i]);
    }

    lv_obj_t *fp = ui_pixel_panel_create(s_scr, 16, 214, 160, 46, UI_PAPER);
    s_feedback = lv_label_create(fp);
    lv_obj_set_style_text_font(s_feedback, &lv_font_montserrat_20, 0);
    lv_obj_center(s_feedback);

    // 操作提示放在底部草地上,避开右下角的吉祥物。
    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 10, -6);

    s_mascot = ui_pixel_mascot_create(s_scr, 194, 238);

    render();
    battery_refresh();
    s_batt_timer = lv_timer_create(batt_tick, 5000, NULL);
    lv_screen_load(s_scr);
}

void demo_math_exit(void)
{
    if (s_batt_timer) {
        lv_timer_delete(s_batt_timer);
        s_batt_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_equation = s_feedback = s_hint = s_battery = s_mascot = NULL;
        for (int i = 0; i < MATH_QUIZ_CHOICES; i++) {
            s_cards[i] = NULL;
            s_choice[i] = NULL;
        }
    }
}

void demo_math_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    quiz_input_t in;
    switch (btn) {
    case BSP_BTN_UP:   in = QUIZ_INPUT_PREV;   break;
    case BSP_BTN_DOWN: in = QUIZ_INPUT_NEXT;   break;
    case BSP_BTN_OK:
    default:           in = QUIZ_INPUT_SELECT; break;
    }

    quiz_feedback_t fb = quiz_game_input(&s_game, in);
    if (fb == QUIZ_FB_CORRECT || fb == QUIZ_FB_NEXT) {
        ui_pixel_mascot_jump(s_mascot);
    }
    render();
}
