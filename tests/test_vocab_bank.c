// tests/test_vocab_bank.c - word bank data contract (spec 12.1, 12.2), RNG and view helpers.
//
// Owner: lead developer. Validates every field rule of the embedded test bank, the British
// IPA strings decided by the user (2026-10-23), the category structure needed for lures,
// the deterministic RNG and the UTF-8 view helpers.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_rng.h"
#include "vocab_strings.h"
#include "vocab_test.h"
#include "vocab_view.h"

// User decision Q3: British broad IPA (OALD style), exact strings.
static const struct {
    vocab_uid_t uid;
    const char *ipa;
} EXPECTED_IPA[] = {
    { 101, "/ˈæpl/" }, { 102, "/bəˈnɑːnə/" }, { 103, "/peə/" }, { 104, "/ˈwɔːtəmelən/" },
    { 201, "/kæt/" }, { 202, "/dɒɡ/" }, { 203, "/dʌk/" }, { 204, "/ʃiːp/" },
    { 205, "/ˈtaɪɡə/" }, { 206, "/ˈelɪfənt/" }, { 301, "/bʌs/" }, { 302, "/kɑː/" },
    { 303, "/baɪk/" }, { 304, "/bəʊt/" }, { 305, "/treɪn/" }, { 401, "/pen/" },
    { 402, "/ˈpensl/" }, { 403, "/bʊk/" }, { 404, "/ˈruːlə/" }, { 405, "/ˈskuːlbæɡ/" },
    { 501, "/ɪə/" }, { 502, "/nəʊz/" }, { 503, "/maʊθ/" }, { 504, "/hænd/" },
    { 505, "/fʊt/" }, { 601, "/red/" }, { 602, "/bluː/" }, { 603, "/ɡriːn/" },
    { 604, "/ˈjeləʊ/" }, { 701, "/bred/" }, { 702, "/mɪlk/" }, { 703, "/eɡ/" },
    { 704, "/ˈwɔːtə/" }, { 705, "/dʒuːs/" }, { 706, "/ˌaɪs ˈkriːm/" }, { 801, "/rʌn/" },
    { 802, "/dʒʌmp/" }, { 803, "/swɪm/" }, { 804, "/sɪŋ/" }, { 805, "/sliːp/" },
    { 903, "/ˌɡʊdˈbaɪ/" },
};

// Non-ASCII code points allowed in IPA strings (spec 13 as amended by Q3: no U+025A).
static const unsigned IPA_SYMBOLS[] = {
    0x02C8, 0x02CC, 0x02D0, 0x00E6, 0x0259, 0x0251, 0x0252, 0x0254,
    0x028A, 0x028C, 0x026A, 0x014B, 0x03B8, 0x0283, 0x0292, 0x0261,
};

// Decodes one UTF-8 sequence; returns its length (0 = invalid) and writes the code point.
static size_t utf8_decode(const char *s, unsigned *cp)
{
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0) return 0;
    unsigned value = c & (0x7Fu >> len);
    for (size_t i = 1; i < len; i++) {
        const unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) return 0;
        value = (value << 6) | (cc & 0x3Fu);
    }
    *cp = value;
    return len;
}

static bool is_ascii_printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
    }
    return true;
}

static bool is_lower_chunk(const char *s)
{
    const size_t len = strlen(s);
    if (len < 1 || len > VOCAB_CLOZE_CHUNK_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 'a' || s[i] > 'z') return false;
    }
    return true;
}

static size_t count_occurrences(const char *hay, const char *needle)
{
    size_t n = 0;
    if (!hay || !needle || !needle[0]) return 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

static void check_ipa(const vocab_entry_t *e)
{
    if (!e->ipa) return;
    const size_t len = strlen(e->ipa);
    CHECK(len >= 3 && e->ipa[0] == '/' && e->ipa[len - 1] == '/', "uid %u ipa slashes", e->uid);
    for (const char *p = e->ipa; *p;) {
        unsigned cp = 0;
        const size_t n = utf8_decode(p, &cp);
        CHECK(n > 0, "uid %u ipa invalid UTF-8", e->uid);
        if (n == 0) return;
        if (cp >= 0x80) {
            bool known = false;
            for (size_t i = 0; i < sizeof(IPA_SYMBOLS) / sizeof(IPA_SYMBOLS[0]); i++) {
                if (IPA_SYMBOLS[i] == cp) known = true;
            }
            CHECK(known, "uid %u ipa code point U+%04X outside the glyph set", e->uid, cp);
        } else {
            CHECK(cp >= 0x20 && cp <= 0x7E, "uid %u ipa control char", e->uid);
        }
        p += n;
    }
}

static void check_entry(const vocab_bank_t *bank, vocab_index_t index)
{
    const vocab_entry_t *e = &bank->entries[index];
    CHECK(e->uid != VOCAB_UID_NONE, "index %u uid 0", index);
    CHECK(e->text && strlen(e->text) >= 1 && strlen(e->text) <= VOCAB_TEXT_MAX_CHARS,
          "uid %u text length", e->uid);
    CHECK(e->text && is_ascii_printable(e->text), "uid %u text not ASCII", e->uid);
    CHECK(e->text && e->text[0] != ' ' && e->text[strlen(e->text) - 1] != ' ',
          "uid %u text padded", e->uid);
    CHECK(e->zh && vocab_utf8_count(e->zh) >= 1 && vocab_utf8_count(e->zh) <= VOCAB_ZH_MAX_CHARS,
          "uid %u zh length", e->uid);
    if (e->zh) {
        for (const char *p = e->zh; *p;) {
            unsigned cp = 0;
            const size_t n = utf8_decode(p, &cp);
            CHECK(n > 0 && cp >= 0x3400, "uid %u zh must be CJK only", e->uid);
            if (n == 0) break;
            p += n;
        }
    }
    CHECK(e->category >= 1 && e->category <= VOCAB_CATEGORY_MAX, "uid %u category", e->uid);
    check_ipa(e);

    const bool right = e->cloze_right != NULL;
    const bool wrong = e->cloze_wrong != NULL;
    CHECK(right == wrong, "uid %u cloze fields all or none", e->uid);
    CHECK(vocab_entry_has_cloze(e) == (right && wrong), "uid %u has_cloze", e->uid);
    if (right && wrong) {
        CHECK(is_lower_chunk(e->cloze_right) && is_lower_chunk(e->cloze_wrong),
              "uid %u chunks must be 1-3 lowercase letters", e->uid);
        CHECK(strlen(e->cloze_right) == strlen(e->cloze_wrong), "uid %u chunk lengths", e->uid);
        CHECK(strcmp(e->cloze_right, e->cloze_wrong) != 0, "uid %u chunks equal", e->uid);
        CHECK((size_t)e->cloze_pos + strlen(e->cloze_right) <= strlen(e->text) &&
                  strncmp(e->text + e->cloze_pos, e->cloze_right, strlen(e->cloze_right)) == 0,
              "uid %u text[pos] != right", e->uid);
    }

    CHECK(e->example && is_ascii_printable(e->example), "uid %u example ASCII", e->uid);
    CHECK(e->example && strlen(e->example) <= VOCAB_EXAMPLE_MAX_CHARS, "uid %u example length",
          e->uid);
    CHECK(e->example && count_occurrences(e->example, e->text) == 1,
          "uid %u example must contain the text exactly once", e->uid);
    if (e->example_cloze && e->example) {
        const char *at = strstr(e->example, e->text);
        char expected[64];
        if (at) {
            const size_t head = (size_t)(at - e->example);
            snprintf(expected, sizeof(expected), "%.*s%s%s", (int)head, e->example,
                     VOCAB_CLOZE_BLANK, at + strlen(e->text));
            CHECK(strcmp(expected, e->example_cloze) == 0, "uid %u example_cloze '%s' != '%s'",
                  e->uid, e->example_cloze, expected);
        }
    }
    CHECK(e->example_zh == NULL, "uid %u example_zh reserved", e->uid);
    CHECK(vocab_entry_has_example_cloze(e) == (e->example_cloze != NULL), "uid %u has_example_cloze",
          e->uid);
}

static void check_bank(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    CHECK(bank && bank->entries && bank->count == 48, "48 entries");
    if (!bank || !bank->entries) return;
    CHECK(bank->count <= VOCAB_RECORDS_MAX, "bank fits the static records");

    unsigned words = 0, phrases = 0, cloze = 0, ipa = 0;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        check_entry(bank, i);
        if (vocab_entry_kind(e) == VOCAB_KIND_PHRASE) phrases++; else words++;
        if (vocab_entry_has_cloze(e)) cloze++;
        if (vocab_entry_has_ipa(e)) ipa++;
        for (vocab_index_t j = (vocab_index_t)(i + 1); j < bank->count; j++) {
            CHECK(bank->entries[j].uid != e->uid, "duplicate uid %u", e->uid);
        }
        if (i > 0) CHECK(bank->entries[i - 1].uid < e->uid, "uids ascending at %u", e->uid);
    }
    CHECK(words == 40 && phrases == 8, "40 words / 8 phrases (got %u / %u)", words, phrases);
    CHECK(cloze == 34, "34 cloze entries (got %u)", cloze);
    CHECK(ipa == sizeof(EXPECTED_IPA) / sizeof(EXPECTED_IPA[0]), "IPA count %u", ipa);

    for (size_t k = 0; k < sizeof(EXPECTED_IPA) / sizeof(EXPECTED_IPA[0]); k++) {
        vocab_index_t idx = 0;
        CHECK(vocab_bank_find(bank, EXPECTED_IPA[k].uid, &idx), "uid %u missing", EXPECTED_IPA[k].uid);
        const vocab_entry_t *e = vocab_bank_at(bank, idx);
        CHECK(e && e->ipa && strcmp(e->ipa, EXPECTED_IPA[k].ipa) == 0, "uid %u IPA '%s' != '%s'",
              EXPECTED_IPA[k].uid, e && e->ipa ? e->ipa : "(null)", EXPECTED_IPA[k].ipa);
    }

    // Lure structure (spec 12.1): every WORD has same-kind entries in >= 8 other categories,
    // every PHRASE in >= 2 other categories.
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        bool seen_cat[VOCAB_CATEGORY_MAX + 1u] = { false };
        unsigned other = 0;
        for (vocab_index_t j = 0; j < bank->count; j++) {
            const vocab_entry_t *c = &bank->entries[j];
            if (j == i || c->category == e->category) continue;
            if (vocab_entry_kind(c) != vocab_entry_kind(e)) continue;
            if (!seen_cat[c->category]) {
                seen_cat[c->category] = true;
                other++;
            }
        }
        const unsigned need = vocab_entry_kind(e) == VOCAB_KIND_WORD ? 8u : 2u;
        CHECK(other >= need, "uid %u same-kind lure categories %u < %u", e->uid, other, need);
    }

    // Lookups.
    vocab_index_t idx = 0;
    CHECK(vocab_bank_find(bank, 706, &idx) && strcmp(bank->entries[idx].text, "ice cream") == 0,
          "find 706");
    CHECK(!vocab_bank_find(bank, 9999, &idx), "unknown uid");
    CHECK(vocab_bank_at(bank, bank->count) == NULL, "at() bounds");
    CHECK(vocab_entry_kind(&bank->entries[idx]) == VOCAB_KIND_PHRASE, "ice cream is a phrase");
}

static void check_rng(void)
{
    vocab_rng_t a, b;
    vocab_rng_seed(&a, 42);
    vocab_rng_seed(&b, 42);
    for (int i = 0; i < 100; i++) CHECK(vocab_rng_next(&a) == vocab_rng_next(&b), "determinism");
    vocab_rng_seed(&a, 0);
    CHECK(vocab_rng_next(&a) != vocab_rng_next(&a), "seed 0 usable");

    unsigned ups = 0;
    for (uint32_t seed = 1; seed <= 10000; seed++) {
        vocab_rng_t r;
        vocab_rng_seed(&r, seed);
        if (vocab_rng_bit(&r)) ups++;
    }
    CHECK(ups >= 4500 && ups <= 5500, "first bit balanced over 10000 seeds (%u)", ups);

    unsigned hist[7] = { 0 };
    vocab_rng_seed(&a, 7);
    for (int i = 0; i < 70000; i++) {
        const uint32_t v = vocab_rng_below(&a, 7);
        CHECK(v < 7, "below range");
        if (v < 7) hist[v]++;
    }
    for (int i = 0; i < 7; i++) CHECK(hist[i] > 9000 && hist[i] < 11000, "below uniform %d", i);
    CHECK(vocab_rng_below(&a, 0) == 0 && vocab_rng_below(&a, 1) == 0, "below degenerate");

    vocab_index_t items[10];
    for (vocab_index_t i = 0; i < 10; i++) items[i] = i;
    vocab_rng_shuffle(&a, items, 10);
    unsigned mask = 0;
    for (int i = 0; i < 10; i++) mask |= 1u << items[i];
    CHECK(mask == 0x3FFu, "shuffle is a permutation");
}

static void check_view_helpers(void)
{
    char buf[8];
    // "苹果" is 6 bytes; a 6-byte buffer keeps only the first character (3 bytes + NUL).
    CHECK(vocab_text_copy(buf, 6, "苹果") == 3 && strcmp(buf, "苹") == 0, "UTF-8 safe truncation");
    CHECK(vocab_text_copy(buf, sizeof(buf), NULL) == 0 && buf[0] == '\0', "NULL source");
    CHECK(vocab_utf8_count("很高兴见到你") == 6, "utf8 count");
    CHECK(vocab_utf8_count("/ˈæpl/") == 6, "utf8 count IPA");

    vocab_view_t v;
    vocab_view_init(&v);
    CHECK(v.screen == VOCAB_SCREEN_BOOT, "init screen");
    v.hint.kind = VOCAB_HINT_REPLAY;
    vocab_view_resolve(&v, true);
    CHECK(strcmp(v.hint.text, VOCAB_STR_HINT_LISTEN) == 0, "replay listen");
    vocab_view_resolve(&v, false);
    CHECK(strcmp(v.hint.text, VOCAB_STR_HINT_LOOK) == 0, "replay look");
    v.hint.toast = VOCAB_TOAST_MUTE;
    vocab_view_resolve(&v, true);
    CHECK(strcmp(v.hint.text, VOCAB_STR_TOAST_MUTE) == 0 && v.hint.color == VOCAB_RGB_TEXT, "toast");

    v.anim.shake_seq = 7;
    vocab_view_set_panel(&v, VOCAB_SIDE_DOWN, "x", VOCAB_PANEL_S1, false);
    v.panel[1].nudge_repeat = true;
    v.badge = VOCAB_BADGE_NEW;
    vocab_view_reset_card(&v);
    CHECK(!v.panel[1].visible && !v.panel[1].nudge_repeat && v.badge == VOCAB_BADGE_NONE,
          "reset_card clears the stage region");
    CHECK(v.anim.shake_seq == 7, "reset_card keeps animation counters");

    CHECK(strcmp(vocab_view_badge_text(VOCAB_BADGE_SPOT_CHECK), VOCAB_STR_BADGE_SPOT_CHECK) == 0,
          "badge text");
    CHECK(vocab_view_card_badge(VOCAB_PURPOSE_LEARN, VOCAB_FORMAT_S3) == VOCAB_BADGE_SPELL, "badge S3");
    CHECK(vocab_view_card_badge(VOCAB_PURPOSE_TEST_LATER, VOCAB_FORMAT_S1) == VOCAB_BADGE_TEST_AGAIN,
          "badge test later");
    uint32_t bg = 0, border = 0;
    vocab_view_panel_colors(VOCAB_PANEL_CORRECT, &bg, &border);
    CHECK(bg == 0x065F46u && border == 0x34D399u, "correct panel colors");
}

int main(void)
{
    check_bank();
    check_rng();
    check_view_helpers();
    return vocab_test_report("vocab bank, RNG and view helper tests");
}
