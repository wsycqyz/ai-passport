// main/math_quiz.c —— math_quiz.h 的实现。
#include "math_quiz.h"

#include <stdbool.h>

void math_rng_seed(math_rng_t *rng, uint32_t seed)
{
    rng->state = seed ? seed : 0x1234567u;  // xorshift32 状态不能为 0
}

uint32_t math_rng_next(math_rng_t *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

// [0, n) 的均匀随机数(要求 n > 0)。
static int rng_below(math_rng_t *rng, int n)
{
    return (int)(math_rng_next(rng) % (uint32_t)n);
}

// [lo, hi] 的随机数(含端点,要求 lo <= hi)。
static int rng_range(math_rng_t *rng, int lo, int hi)
{
    return lo + rng_below(rng, hi - lo + 1);
}

char math_op_symbol(math_op_t op)
{
    switch (op) {
    case MATH_OP_ADD: return '+';
    case MATH_OP_SUB: return '-';
    case MATH_OP_MUL: return 'x';
    case MATH_OP_DIV: return '/';
    default:          return '?';
    }
}

// 就地随机洗牌整型数组(Fisher-Yates)。
static void shuffle(int *values, int count, math_rng_t *rng)
{
    for (int i = count - 1; i > 0; i--) {
        int j = rng_below(rng, i + 1);
        int t = values[i];
        values[i] = values[j];
        values[j] = t;
    }
}

// 生成四个选项:含唯一正确答案,其余为答案附近、非负、互不相同的干扰项。
static void fill_choices(math_question_t *q, math_rng_t *rng)
{
    int set[MATH_QUIZ_CHOICES];
    int count = 0;
    set[count++] = q->answer;

    // 常见误差(±1、±2、进退位等)优先,随机顺序尝试;三位数还加上百位
    // 进退位的常见误差(±99、±100、±101)。
#define MATH_QUIZ_OFFSET_COUNT 18
    static const int offsets[MATH_QUIZ_OFFSET_COUNT] = {
        1, -1, 2, -2, 3, -3, 5, -5, 10, -10, 11, -11, 99, -99, 100, -100, 101, -101,
    };
    int order[MATH_QUIZ_OFFSET_COUNT];
    for (int i = 0; i < MATH_QUIZ_OFFSET_COUNT; i++) order[i] = i;
    shuffle(order, MATH_QUIZ_OFFSET_COUNT, rng);

    for (int i = 0; i < MATH_QUIZ_OFFSET_COUNT && count < MATH_QUIZ_CHOICES; i++) {
        int cand = q->answer + offsets[order[i]];
        if (cand < 0) continue;
        bool dup = false;
        for (int k = 0; k < count; k++) {
            if (set[k] == cand) { dup = true; break; }
        }
        if (!dup) set[count++] = cand;
    }
    // 兜底:偏移不够时用 answer+1、answer+2… 顺延,必为非负且互不相同。
    for (int extra = 1; count < MATH_QUIZ_CHOICES; extra++) {
        int cand = q->answer + extra;
        bool dup = false;
        for (int k = 0; k < count; k++) {
            if (set[k] == cand) { dup = true; break; }
        }
        if (!dup) set[count++] = cand;
    }

    shuffle(set, MATH_QUIZ_CHOICES, rng);
    for (int i = 0; i < MATH_QUIZ_CHOICES; i++) {
        q->choices[i] = set[i];
        if (set[i] == q->answer) q->correct_index = i;
    }
}

void math_quiz_generate(math_question_t *q, math_rng_t *rng)
{
    q->op = (math_op_t)rng_below(rng, MATH_OP_COUNT);
    switch (q->op) {
    case MATH_OP_ADD:
        q->a = rng_range(rng, 1, 998);
        q->b = rng_range(rng, 1, 999 - q->a);  // 保证和不超过三位数
        q->answer = q->a + q->b;
        break;
    case MATH_OP_SUB:
        q->a = rng_range(rng, 1, 999);
        q->b = rng_range(rng, 0, q->a);        // 保证差非负
        q->answer = q->a - q->b;
        break;
    case MATH_OP_MUL: {
        int one_digit = rng_range(rng, 1, 9);       // 一位数因数
        int other_max = 999 / one_digit;            // 保证积不超过三位数
        int other = rng_range(rng, 1, other_max);
        if (rng_below(rng, 2)) {                     // 随机决定一位数因数在前还是在后
            q->a = one_digit;
            q->b = other;
        } else {
            q->a = other;
            q->b = one_digit;
        }
        q->answer = q->a * q->b;
        break;
    }
    case MATH_OP_DIV:
    default: {
        q->b = rng_range(rng, 1, 9);                 // 一位数除数
        int quotient_max = 999 / q->b;               // 保证被除数不超过三位数
        int quotient = rng_range(rng, 1, quotient_max);
        q->a = quotient * q->b;                      // 由商与除数反推,保证整除
        q->answer = quotient;
        break;
    }
    }
    fill_choices(q, rng);
}

void quiz_game_init(quiz_game_t *g, uint32_t seed)
{
    math_rng_seed(&g->rng, seed);
    g->state = QUIZ_SELECTING;
    g->selected = 0;
    g->wrong_count = 0;
    g->last_pick = -1;
    math_quiz_generate(&g->q, &g->rng);
}

static void next_question(quiz_game_t *g)
{
    g->state = QUIZ_SELECTING;
    g->selected = 0;
    g->wrong_count = 0;
    g->last_pick = -1;
    math_quiz_generate(&g->q, &g->rng);
}

quiz_feedback_t quiz_game_input(quiz_game_t *g, quiz_input_t in)
{
    // 等待态:任意按键进入下一题。
    if (g->state == QUIZ_CORRECT || g->state == QUIZ_REVEAL) {
        next_question(g);
        return QUIZ_FB_NEXT;
    }

    switch (in) {
    case QUIZ_INPUT_PREV:
        g->selected = (g->selected + MATH_QUIZ_CHOICES - 1) % MATH_QUIZ_CHOICES;
        return QUIZ_FB_MOVED;
    case QUIZ_INPUT_NEXT:
        g->selected = (g->selected + 1) % MATH_QUIZ_CHOICES;
        return QUIZ_FB_MOVED;
    case QUIZ_INPUT_SELECT:
    default:
        g->last_pick = g->selected;
        if (g->selected == g->q.correct_index) {
            g->state = QUIZ_CORRECT;
            return QUIZ_FB_CORRECT;
        }
        g->wrong_count++;
        if (g->wrong_count >= MATH_QUIZ_MAX_WRONG) {
            g->state = QUIZ_REVEAL;
            return QUIZ_FB_WRONG_REVEAL;
        }
        return QUIZ_FB_WRONG_RETRY;
    }
}
