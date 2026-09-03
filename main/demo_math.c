// main/demo_math.c —— 小学三位数以内加减乘除答题页(开机即进入)。
// 题目与判分逻辑在 math_quiz.c;本文件只负责渲染与按键。
// 上/下:移动高亮(四选项按线性顺序,与主菜单一致);确定:提交。
// 答对提示 CORRECT 并等待按键进入下一题;答错提示 TRY AGAIN 并可重选,
// 再次答错则揭示答案并等待按键进入下一题。长按“确定”仍可返回演示菜单。
// 算式按“操作数/运算符/操作数/结果”拆成独立控件摆成一行:字库没有“÷”
// 字形,除法运算符改用像素方块手绘的上下两点+一横,其余运算符仍用文字。
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
static lv_obj_t   *s_eq_a;      // 算式左操作数
static lv_obj_t   *s_op_box;    // 运算符占位框(文字或像素图形二选一显示)
static lv_obj_t   *s_op_label;  // 文字运算符:'+' '-' 'x'
static lv_obj_t   *s_op_div_top;// 除号像素图形:上点
static lv_obj_t   *s_op_div_bar;// 除号像素图形:横杠
static lv_obj_t   *s_op_div_bot;// 除号像素图形:下点
static lv_obj_t   *s_eq_b;      // 算式右操作数
static lv_obj_t   *s_eq_tail;   // "= ?" 或 "= 答案"
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

    lv_label_set_text_fmt(s_eq_a, "%d", q->a);
    lv_label_set_text_fmt(s_eq_b, "%d", q->b);
    if (s_game.state == QUIZ_SELECTING) {
        lv_label_set_text(s_eq_tail, "= ?");
    } else {
        lv_label_set_text_fmt(s_eq_tail, "= %d", q->answer);
    }

    // 字库没有“÷”字形:除法显示像素画的上下两点+一横,其余运算符用文字。
    if (q->op == MATH_OP_DIV) {
        lv_obj_add_flag(s_op_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_op_div_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_op_div_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_op_div_bot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(s_op_label, "%c", math_op_symbol(q->op));
        lv_obj_remove_flag(s_op_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_op_div_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_op_div_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_op_div_bot, LV_OBJ_FLAG_HIDDEN);
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
        lv_label_set_text_fmt(s_feedback, "ANS: %d", q->answer);
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

    // 算式一行:操作数/运算符/结果各自独立控件,靠 flex 横向排布并按内容
    // 宽度自适应(三位数与一位数宽度不同,不能像固定两位数时那样用一个
    // 定宽标签);flex 在本文件之外未被使用,这里专为这种变宽场景引入。
    lv_obj_t *row = lv_obj_create(qp);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_eq_a = lv_label_create(row);
    lv_obj_set_style_text_font(s_eq_a, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_eq_a, lv_color_hex(UI_INK), 0);

    s_op_box = lv_obj_create(row);
    lv_obj_remove_flag(s_op_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_op_box, 16, 16);
    lv_obj_set_style_bg_opa(s_op_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_op_box, 0, 0);
    lv_obj_set_style_pad_all(s_op_box, 0, 0);

    s_op_label = lv_label_create(s_op_box);
    lv_obj_set_style_text_font(s_op_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_op_label, lv_color_hex(UI_INK), 0);
    lv_obj_center(s_op_label);

    // 手绘“÷”:字库没有除号字形,用两个小方块加一条横杠拼出来。
    s_op_div_top = ui_pixel_block(s_op_box, 6, 1, 4, 4, UI_INK);
    s_op_div_bar = ui_pixel_block(s_op_box, 3, 7, 10, 2, UI_INK);
    s_op_div_bot = ui_pixel_block(s_op_box, 6, 11, 4, 4, UI_INK);

    s_eq_b = lv_label_create(row);
    lv_obj_set_style_text_font(s_eq_b, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_eq_b, lv_color_hex(UI_INK), 0);

    s_eq_tail = lv_label_create(row);
    lv_obj_set_style_text_font(s_eq_tail, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_eq_tail, lv_color_hex(UI_INK), 0);

    lv_obj_center(row);

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
        s_eq_a = s_op_box = s_op_label = s_op_div_top = s_op_div_bar = s_op_div_bot = NULL;
        s_eq_b = s_eq_tail = s_feedback = s_hint = s_battery = s_mascot = NULL;
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
