// main/vocab_pet.h - XP, levels, titles and pet stage (spec 4.9).
//
// Pure; any task; never blocks. Owner: lead developer (tests in tests/test_vocab_pet.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocab_view.h"

// Saturating uint32 addition for cumulative XP.
uint32_t vocab_xp_add(uint32_t xp, uint32_t delta);

// XP earned by a completed session: 5 + first_try + 2 x new_mastered (saturating).
uint32_t vocab_session_xp(uint32_t first_try, uint32_t new_mastered);

// Level 1..10 for cumulative XP (minimum XP per level: 0, 30, 90, ..., 1350).
uint8_t vocab_level_for_xp(uint32_t xp);

// Minimum cumulative XP of `level` (clamped to 1..10).
uint32_t vocab_level_min_xp(uint8_t level);

// Title of `level` (clamped to 1..10), UTF-8 from vocab_strings.h.
const char *vocab_level_title(uint8_t level);

// Pet drawing stage of `level`: Lv1 egg, Lv2-3 hatchling, Lv4-6 child, Lv7-9 grown, Lv10 crown.
vocab_pet_stage_t vocab_pet_stage_for_level(uint8_t level);
