// main/vocab_strings.h - every user-visible Word Buddy string (spec 11.5 and 13), UTF-8.
//
// Rules:
// - Every Chinese UI string of the application lives here, exactly as listed in spec 13.
//   Stage and UI code use these macros and never type their own Chinese literals, so the
//   font inventory (tools/vocab_fonts.py scans string literals in main/main.c and
//   main/vocab_*.{c,h}) and the spec stay in one place.
// - Panel texts exclude the key glyph: the renderer draws "\u25B2"/"\u25BC" in its own label.
// - Format strings use printf conversions (%u, %s, %d); callers format into view buffers.
// - Bank meanings (zh), English text and IPA live in vocab_bank.c, not here.
//
// Owner: lead developer. Adding a string requires regenerating the font subsets (assets).
#pragma once

// --- Key glyphs (U+25B2, U+25BC, U+25CF) -------------------------------------------------
#define VOCAB_STR_KEY_UP              "▲"
#define VOCAB_STR_KEY_DOWN            "▼"
#define VOCAB_STR_KEY_OK              "●"

// --- Badges (status bar pill, spec 11.5) --------------------------------------------------
#define VOCAB_STR_BADGE_NEW           "新词"
#define VOCAB_STR_BADGE_RECALL        "回想"
#define VOCAB_STR_BADGE_SELF_RATE     "自评"
#define VOCAB_STR_BADGE_SPELL         "拼一拼"
#define VOCAB_STR_BADGE_TEST_AGAIN    "再考一次"
#define VOCAB_STR_BADGE_SPOT_CHECK    "突击检查"
#define VOCAB_STR_BADGE_TOO_EASY      "太简单？"
#define VOCAB_STR_BADGE_REMEMBER      "记住它"
#define VOCAB_STR_BADGE_PAUSE         "暂停"
#define VOCAB_STR_BADGE_DONE          "完成"

// --- Stage 1 (spec 5) ---------------------------------------------------------------------
#define VOCAB_STR_S1_PROMPT           "它是什么意思？"
#define VOCAB_STR_CORRECT             "答对了！"
#define VOCAB_STR_HOLD_MEANING        "记住它的意思"

// --- Stage 2 (spec 6) ---------------------------------------------------------------------
#define VOCAB_STR_S2_FRONT_WORD       "想一想：什么意思？"
#define VOCAB_STR_S2_FRONT_CLOZE      "听一听：空里填什么？"
#define VOCAB_STR_S2_KNOW             "认识，下次见"
#define VOCAB_STR_S2_FORGOT           "忘了，等下考我"
#define VOCAB_STR_S2_FORGOT_ACK       "好，等下再考你"

// --- Stage 3 (spec 7) ---------------------------------------------------------------------
#define VOCAB_STR_S3_PROMPT           "选字母，填空格"
#define VOCAB_STR_S3_RETRY            "再想想，再试一次"
#define VOCAB_STR_S3_SUCCESS          "拼对了！"
#define VOCAB_STR_S3_REVEAL           "记住正确的拼写"

// --- Session banners and prompts (spec 4.8, 11.5) -----------------------------------------
#define VOCAB_STR_SPOT_BANNER         "突击检查！"
#define VOCAB_STR_SPOT_PROMPT         "刚才的词，还记得吗？"
#define VOCAB_STR_TOO_EASY_ACK        "好，等下考考你！"
#define VOCAB_STR_TOO_EASY_BANNER     "太简单？考考你！"
#define VOCAB_STR_TOO_EASY_PROMPT     "答对就直接满级"
#define VOCAB_STR_PAUSE_BANNER        "休息一下"
#define VOCAB_STR_PAUSE_PROMPT        "要继续吗？"
#define VOCAB_STR_PAUSE_CONTINUE      "继续学习"
#define VOCAB_STR_PAUSE_HOME          "回到主页"
#define VOCAB_STR_SUMMARY_BANNER      "太棒了！"
#define VOCAB_STR_SUMMARY_SCORE_FMT   "答对 %u/%u"
#define VOCAB_STR_SUMMARY_LEARNED_FMT "学完 %u 个词"
#define VOCAB_STR_SUMMARY_XP_FMT      "+%u 经验"
#define VOCAB_STR_SUMMARY_LEVELUP_FMT "升级啦！%s"
#define VOCAB_STR_BANK_ERROR_BANNER   "词库出错了"
#define VOCAB_STR_BANK_ERROR_PROMPT   "请联系家长更新"

// --- Hint bar (spec 11.5) -----------------------------------------------------------------
#define VOCAB_STR_HINT_LISTEN         "● 再听一遍"
#define VOCAB_STR_HINT_LOOK           "● 再看一遍"
#define VOCAB_STR_HINT_THINK          "想一想…"
#define VOCAB_STR_HINT_FLIP           "按任意键翻面"
#define VOCAB_STR_HINT_PRESS_UP       "按 ▲ 继续"
#define VOCAB_STR_HINT_PRESS_DOWN     "按 ▼ 继续"
#define VOCAB_STR_HINT_SUMMARY        "● 再来一组  ▲ 回主页"
#define VOCAB_STR_HOME_GO             "按 ● 开始挑战"

// --- HOME (spec 4.9, 9, 10.5, 11.5) ---------------------------------------------------------
#define VOCAB_STR_LEVEL_FMT           "Lv.%u"
#define VOCAB_STR_XP_FMT              "经验 %u/%u"
#define VOCAB_STR_XP_MAX_FMT          "经验 %u（满级）"
#define VOCAB_STR_STATS_FMT           "已学 %u 个 · 掌握 %u 个"
#define VOCAB_STR_WARN_BATTERY        "电量低，请充电"
#define VOCAB_STR_WARN_SAVE           "进度暂时无法保存"
#define VOCAB_STR_BATT_PCT_FMT        "%d%%"
#define VOCAB_STR_BATT_PCT_UNKNOWN    "--%"

// Level titles Lv1..Lv10 (spec 4.9). Usage: static const char *const t[] = VOCAB_STR_TITLES_INIT;
#define VOCAB_STR_TITLES_INIT { \
    "单词新手", "单词学徒", "单词探险家", "单词小勇士", "单词小达人", \
    "单词高手", "单词明星", "单词冠军", "单词大师", "单词之王" }

// --- Toasts (spec 8.4, 8.7) ---------------------------------------------------------------
#define VOCAB_STR_TOAST_VOL_HIGH      "音量：大"
#define VOCAB_STR_TOAST_VOL_LOW       "音量：小"
#define VOCAB_STR_TOAST_MUTE          "静音"
#define VOCAB_STR_TOAST_NO_AUDIO      "声音不可用"
