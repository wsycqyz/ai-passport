// main/vocab_pet.c - XP, levels, titles and pet stage (see vocab_pet.h).
#include "vocab_pet.h"

#include "vocab_config.h"
#include "vocab_strings.h"

static const uint32_t LEVEL_XP[VOCAB_LEVEL_MAX] = VOCAB_LEVEL_XP_INIT;
static const char *const TITLES[VOCAB_LEVEL_MAX] = VOCAB_STR_TITLES_INIT;

static uint8_t clamp_level(uint8_t level)
{
    if (level < 1u) return 1u;
    return level > VOCAB_LEVEL_MAX ? (uint8_t)VOCAB_LEVEL_MAX : level;
}

uint32_t vocab_xp_add(uint32_t xp, uint32_t delta)
{
    return xp > UINT32_MAX - delta ? UINT32_MAX : xp + delta;
}

uint32_t vocab_session_xp(uint32_t first_try, uint32_t new_mastered)
{
    uint32_t xp = vocab_xp_add(VOCAB_XP_PER_SESSION, first_try * VOCAB_XP_FIRST_TRY);
    const uint32_t bonus = new_mastered > UINT32_MAX / VOCAB_XP_NEW_MASTERED
                               ? UINT32_MAX
                               : new_mastered * VOCAB_XP_NEW_MASTERED;
    return vocab_xp_add(xp, bonus);
}

uint8_t vocab_level_for_xp(uint32_t xp)
{
    uint8_t level = 1u;
    for (uint8_t i = 1u; i < VOCAB_LEVEL_MAX; i++) {
        if (xp >= LEVEL_XP[i]) level = (uint8_t)(i + 1u);
    }
    return level;
}

uint32_t vocab_level_min_xp(uint8_t level)
{
    return LEVEL_XP[clamp_level(level) - 1u];
}

const char *vocab_level_title(uint8_t level)
{
    return TITLES[clamp_level(level) - 1u];
}

vocab_pet_stage_t vocab_pet_stage_for_level(uint8_t level)
{
    level = clamp_level(level);
    if (level >= 10u) return VOCAB_PET_CROWN;
    if (level >= 7u) return VOCAB_PET_GROWN;
    if (level >= 4u) return VOCAB_PET_CHILD;
    if (level >= 2u) return VOCAB_PET_HATCHLING;
    return VOCAB_PET_EGG;
}
