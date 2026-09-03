// tests/test_math_quiz.c —— math_quiz 纯逻辑主机测试(不依赖 LVGL/ESP-IDF)。
#include <assert.h>
#include <string.h>

#include "math_quiz.h"

static int in_range(int v, int lo, int hi)
{
    return v >= lo && v <= hi;
}

// 从任意选择态出发,按 NEXT 把高亮移动到目标下标。
static void move_to(quiz_game_t *g, int target)
{
    for (int guard = 0; g->selected != target && guard < MATH_QUIZ_CHOICES; guard++) {
        quiz_game_input(g, QUIZ_INPUT_NEXT);
    }
    assert(g->selected == target);
}

static void test_rng(void)
{
    math_rng_t a, b;
    math_rng_seed(&a, 12345u);
    math_rng_seed(&b, 12345u);
    for (int i = 0; i < 256; i++) {
        assert(math_rng_next(&a) == math_rng_next(&b));  // 同种子可复现
    }
    math_rng_t z;
    math_rng_seed(&z, 0u);                                // 种子 0 需被修正为非零
    assert(math_rng_next(&z) != 0u);
}

static void test_symbols(void)
{
    assert(math_op_symbol(MATH_OP_ADD) == '+');
    assert(math_op_symbol(MATH_OP_SUB) == '-');
    assert(math_op_symbol(MATH_OP_MUL) == 'x');
    assert(math_op_symbol(MATH_OP_DIV) == '/');
}

static void test_generation(void)
{
    for (uint32_t seed = 1u; seed <= 5000u; seed++) {
        math_rng_t rng;
        math_rng_seed(&rng, seed);
        math_question_t q;
        memset(&q, 0, sizeof q);
        math_quiz_generate(&q, &rng);

        // 操作数与答案均在三位数以内且非负。
        assert(in_range(q.a, 0, 999));
        assert(in_range(q.b, 0, 999));
        assert(in_range(q.answer, 0, 999));

        // 运算结果正确,且符合小学约束。
        switch (q.op) {
        case MATH_OP_ADD:
            assert(q.answer == q.a + q.b);
            break;
        case MATH_OP_SUB:
            assert(q.answer == q.a - q.b);
            assert(q.a >= q.b);                 // 差非负
            break;
        case MATH_OP_MUL:
            assert(q.answer == q.a * q.b);
            assert(q.a >= 1 && q.b >= 1);
            assert(in_range(q.a, 1, 9) || in_range(q.b, 1, 9));  // 一个因数为一位数
            assert(q.answer <= 999);                              // 积不超三位数
            break;
        case MATH_OP_DIV:
            assert(q.b > 0);                    // 除数非零
            assert(in_range(q.b, 1, 9));         // 除数为一位数
            assert(q.a == q.answer * q.b);      // 整除
            assert(q.a % q.b == 0);
            assert(q.a <= 999);                  // 被除数不超三位数
            break;
        default:
            assert(0 && "unexpected op");
        }

        // 选项:恰有一个等于答案,四项互不相同且非负,correct_index 指向答案。
        assert(in_range(q.correct_index, 0, MATH_QUIZ_CHOICES - 1));
        assert(q.choices[q.correct_index] == q.answer);
        int answer_hits = 0;
        for (int i = 0; i < MATH_QUIZ_CHOICES; i++) {
            assert(q.choices[i] >= 0);
            if (q.choices[i] == q.answer) answer_hits++;
            for (int j = i + 1; j < MATH_QUIZ_CHOICES; j++) {
                assert(q.choices[i] != q.choices[j]);
            }
        }
        assert(answer_hits == 1);
    }
}

static void test_correct_first_try(void)
{
    quiz_game_t g;
    quiz_game_init(&g, 42u);
    move_to(&g, g.q.correct_index);

    assert(quiz_game_input(&g, QUIZ_INPUT_SELECT) == QUIZ_FB_CORRECT);
    assert(g.state == QUIZ_CORRECT);

    // 等待态:任意键进入下一题。
    assert(quiz_game_input(&g, QUIZ_INPUT_PREV) == QUIZ_FB_NEXT);
    assert(g.state == QUIZ_SELECTING);
    assert(g.wrong_count == 0);
    assert(g.last_pick == -1);
}

static void test_wrong_twice_reveals(void)
{
    quiz_game_t g;
    quiz_game_init(&g, 7u);

    int wrong1 = (g.q.correct_index + 1) % MATH_QUIZ_CHOICES;
    move_to(&g, wrong1);
    assert(quiz_game_input(&g, QUIZ_INPUT_SELECT) == QUIZ_FB_WRONG_RETRY);
    assert(g.state == QUIZ_SELECTING);          // 仍可重选
    assert(g.wrong_count == 1);

    int wrong2 = (g.q.correct_index + 2) % MATH_QUIZ_CHOICES;
    move_to(&g, wrong2);
    assert(quiz_game_input(&g, QUIZ_INPUT_SELECT) == QUIZ_FB_WRONG_REVEAL);
    assert(g.state == QUIZ_REVEAL);             // 第二次错,揭示答案
    assert(g.wrong_count == MATH_QUIZ_MAX_WRONG);

    assert(quiz_game_input(&g, QUIZ_INPUT_SELECT) == QUIZ_FB_NEXT);
    assert(g.state == QUIZ_SELECTING);
}

static void test_wrong_then_correct(void)
{
    quiz_game_t g;
    quiz_game_init(&g, 99u);

    int wrong1 = (g.q.correct_index + 1) % MATH_QUIZ_CHOICES;
    move_to(&g, wrong1);
    assert(quiz_game_input(&g, QUIZ_INPUT_SELECT) == QUIZ_FB_WRONG_RETRY);

    move_to(&g, g.q.correct_index);
    assert(quiz_game_input(&g, QUIZ_INPUT_SELECT) == QUIZ_FB_CORRECT);
    assert(g.state == QUIZ_CORRECT);
}

static void test_navigation_wraps(void)
{
    quiz_game_t g;
    quiz_game_init(&g, 3u);
    assert(g.selected == 0);
    assert(quiz_game_input(&g, QUIZ_INPUT_PREV) == QUIZ_FB_MOVED);
    assert(g.selected == MATH_QUIZ_CHOICES - 1);   // 上键从头回绕到尾
    assert(quiz_game_input(&g, QUIZ_INPUT_NEXT) == QUIZ_FB_MOVED);
    assert(g.selected == 0);                       // 下键回绕
}

int main(void)
{
    test_rng();
    test_symbols();
    test_generation();
    test_correct_first_try();
    test_wrong_twice_reveals();
    test_wrong_then_correct();
    test_navigation_wraps();
    return 0;
}
