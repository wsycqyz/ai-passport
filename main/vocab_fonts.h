// main/vocab_fonts.h - LVGL font roles of Word Buddy (spec 11.3, amendment A2).
//
// DEVICE ONLY (includes lvgl.h). Every label selects its font explicitly from these roles.
//
// Generated subsets (4 bpp, uncompressed, FORMAT0_TINY ASCII run + SPARSE_TINY rest,
// explicit line_height/base_line), produced by tools/vocab_fonts.py with Pillow/FreeType
// rasterization (never lv_font_conv bitmaps) into assets/fonts/vocab/ and compiled through
// main/CMakeLists.txt (fixed file names = symbol names):
//
//   role       symbol             source font             size  coverage
//   F_EN_L     vocab_font_en_36   Andika Bold             36    ASCII 0x20-0x7E
//   F_EN_M     vocab_font_en_28   Andika Bold             28    ASCII
//   F_EN_S     vocab_font_en_22   Andika Bold             22    ASCII
//   F_EN_XS    vocab_font_en_18   Andika Regular          18    ASCII (example sentences)
//   F_IPA      vocab_font_ipa_18  Andika Regular, ss13 a  18    ASCII + bank IPA symbols
//   F_ZH_L     vocab_font_zh_22   Noto Sans SC Medium     22    CJK inventory + ASCII + punct.
//   F_ZH_S     vocab_font_zh_16   Noto Sans SC Regular    16    CJK inventory + ASCII + punct.
//   F_SYM      lv_font_montserrat_14 (built in)        14    LV_SYMBOL_*, digits, %
//
// Symbol names are neutral on purpose (no Reserved Font Names of the OFL sources).
// The files are generated (VOCAB_FONTS_PLACEHOLDER 0; Milestone 1 compiled placeholders
// that fell back to Montserrat). Layout must read each font's real line_height (ink extents
// of the subset, not "size + n"): EN_L 37, EN_M 29, EN_S 23, EN_XS 19, IPA 20, ZH_L 26,
// ZH_S 19 px (assets/fonts/vocab/vocab_fonts_inventory.json). Only F_IPA (the IPA line)
// draws U+0061 as Andika's double-storey a (ss13) so IPA a and U+0251 differ (spec 11.3);
// every child-facing English role, F_EN_XS included, keeps Andika's single-storey a.
// F_IPA is added after Milestone 1: main/CMakeLists.txt must list
// "../assets/fonts/vocab/vocab_font_ipa_18.c" before VOCAB_FONT_IPA is used (integration).
// No fallback fonts: a missing glyph shows LVGL's placeholder box, and
// `python3 tools/vocab_fonts.py --check` (static gate) fails before that can ship.
// Regenerate after any UI string or bank change that adds a character:
//   PYTHONPATH=<Pillow> python3 tools/vocab_fonts.py generate --fonts-src <source fonts>
//
// Owner: assets developer. Contract written by the lead; only additive changes.
#pragma once

#include "lvgl.h"

#define VOCAB_FONTS_PLACEHOLDER 0

LV_FONT_DECLARE(vocab_font_en_36)
LV_FONT_DECLARE(vocab_font_en_28)
LV_FONT_DECLARE(vocab_font_en_22)
LV_FONT_DECLARE(vocab_font_en_18)
LV_FONT_DECLARE(vocab_font_zh_22)
LV_FONT_DECLARE(vocab_font_zh_16)
LV_FONT_DECLARE(vocab_font_ipa_18)

#define VOCAB_FONT_EN_L   (&vocab_font_en_36)
#define VOCAB_FONT_EN_M   (&vocab_font_en_28)
#define VOCAB_FONT_EN_S   (&vocab_font_en_22)
#define VOCAB_FONT_EN_XS  (&vocab_font_en_18)
#define VOCAB_FONT_ZH_L   (&vocab_font_zh_22)
#define VOCAB_FONT_ZH_S   (&vocab_font_zh_16)
#define VOCAB_FONT_SYM    (&lv_font_montserrat_14)
#define VOCAB_FONT_IPA    (&vocab_font_ipa_18)  // IPA line (lbl_ipa) only
