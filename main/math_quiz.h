// main/math_quiz.h —— 小学两位数以内加减乘除出题与答题状态机。
// 纯逻辑,不依赖 LVGL / ESP-IDF,便于主机测试;渲染与按键在 demo_math.c 中完成。
#pragma once

#include <stdint.h>

#define MATH_QUIZ_CHOICES   4   // 每题四个选项
#define MATH_QUIZ_MAX_WRONG 2   // 允许答错两次,第二次错后揭示答案

typedef enum {
    MATH_OP_ADD = 0,
    MATH_OP_SUB,
    MATH_OP_MUL,
    MATH_OP_DIV,
    MATH_OP_COUNT,
} math_op_t;

typedef struct {
    int       a;
    int       b;
    math_op_t op;
    int       answer;
    int       choices[MATH_QUIZ_CHOICES];
    int       correct_index;
} math_question_t;

typedef enum {
    QUIZ_SELECTING = 0,  // 正在选择答案
    QUIZ_CORRECT,        // 答对,等待按键进入下一题
    QUIZ_REVEAL,         // 两次答错,揭示答案,等待按键进入下一题
} quiz_state_t;

typedef enum {
    QUIZ_INPUT_PREV = 0,  // 上键:高亮上一项;等待态:进入下一题
    QUIZ_INPUT_NEXT,      // 下键:高亮下一项;等待态:进入下一题
    QUIZ_INPUT_SELECT,    // 确定键:提交高亮项;等待态:进入下一题
} quiz_input_t;

typedef enum {
    QUIZ_FB_MOVED = 0,     // 仅移动了高亮
    QUIZ_FB_CORRECT,       // 选择正确
    QUIZ_FB_WRONG_RETRY,   // 选择错误,仍可重选
    QUIZ_FB_WRONG_REVEAL,  // 再次错误,已揭示答案
    QUIZ_FB_NEXT,          // 进入了新的一题
} quiz_feedback_t;

// 确定性随机数(xorshift32),固定种子可复现,便于主机测试。
typedef struct {
    uint32_t state;
} math_rng_t;

void     math_rng_seed(math_rng_t *rng, uint32_t seed);
uint32_t math_rng_next(math_rng_t *rng);

// 生成一题:操作数与答案均在两位数以内(0..99)、非负;减法不出现负数;
// 乘法用九九表(1..9);除法保证整除且除数非零。四个选项含唯一正确答案。
void math_quiz_generate(math_question_t *q, math_rng_t *rng);

// 运算符字符:'+','-','x','/'(屏幕字库仅含 ASCII,不用 × ÷)。
char math_op_symbol(math_op_t op);

typedef struct {
    math_question_t q;
    quiz_state_t    state;
    int             selected;     // 当前高亮的选项下标
    int             wrong_count;  // 本题已答错次数
    int             last_pick;    // 最近提交的选项下标(-1 表示无),供 UI 标记
    math_rng_t      rng;
} quiz_game_t;

// 初始化一局:播种随机数并生成第一题。
void quiz_game_init(quiz_game_t *g, uint32_t seed);

// 处理一次输入,推进状态机并返回本次反馈供 UI 呈现。
quiz_feedback_t quiz_game_input(quiz_game_t *g, quiz_input_t in);
