// tests/test_vocab_pet.c - XP, levels, titles and pet stage (spec 4.9; AC-56).
//
// Owner: lead developer.
#include <string.h>

#include "vocab_config.h"
#include "vocab_pet.h"
#include "vocab_strings.h"
#include "vocab_test.h"

int main(void)
{
    static const uint32_t thresholds[VOCAB_LEVEL_MAX] = VOCAB_LEVEL_XP_INIT;
    static const char *const titles[VOCAB_LEVEL_MAX] = VOCAB_STR_TITLES_INIT;
    const uint32_t expected[VOCAB_LEVEL_MAX] = { 0, 30, 90, 180, 300, 450, 630, 840, 1080, 1350 };
    CHECK(memcmp(thresholds, expected, sizeof(expected)) == 0, "spec 4.9 thresholds");
    for (uint8_t level = 1; level <= VOCAB_LEVEL_MAX; level++) {
        const uint32_t min = expected[level - 1];
        CHECK(vocab_level_min_xp(level) == min, "min XP of Lv%u", level);
        CHECK(vocab_level_for_xp(min) == level, "Lv%u at its threshold", level);
        if (min > 0) CHECK(vocab_level_for_xp(min - 1) == level - 1, "below Lv%u", level);
        CHECK(strcmp(vocab_level_title(level), titles[level - 1]) == 0, "title of Lv%u", level);
    }
    CHECK(vocab_level_for_xp(UINT32_MAX) == 10, "Lv10 is the maximum; XP keeps counting");
    CHECK(vocab_level_min_xp(0) == 0 && vocab_level_min_xp(11) == 1350, "level clamped");

    // XP = 5 + first-try objective answers + 2 x words mastered for the first time.
    CHECK(vocab_session_xp(0, 0) == 5 && vocab_session_xp(6, 0) == 11 && vocab_session_xp(4, 2) == 13,
          "session XP formula");
    CHECK(vocab_session_xp(UINT32_MAX, UINT32_MAX) == UINT32_MAX, "saturating session XP");
    CHECK(vocab_xp_add(UINT32_MAX - 1, 5) == UINT32_MAX && vocab_xp_add(10, 5) == 15, "saturating add");

    // Level-up detection is a level change across the SUMMARY XP addition.
    const uint32_t before = 85;
    const uint32_t after = vocab_xp_add(before, vocab_session_xp(1, 0));
    CHECK(vocab_level_for_xp(before) == 2 && vocab_level_for_xp(after) == 3, "Lv2 -> Lv3 detected");

    const vocab_pet_stage_t stages[11] = {
        VOCAB_PET_EGG, VOCAB_PET_EGG, VOCAB_PET_HATCHLING, VOCAB_PET_HATCHLING, VOCAB_PET_CHILD,
        VOCAB_PET_CHILD, VOCAB_PET_CHILD, VOCAB_PET_GROWN, VOCAB_PET_GROWN, VOCAB_PET_GROWN,
        VOCAB_PET_CROWN,
    };
    for (uint8_t level = 1; level <= 10; level++) {
        CHECK(vocab_pet_stage_for_level(level) == stages[level], "pet stage of Lv%u", level);
    }
    return vocab_test_report("vocab pet tests");
}
