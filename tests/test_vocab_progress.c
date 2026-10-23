// tests/test_vocab_progress.c - persistence codec, migration and NVS budget (spec 10.2,
// 10.3, 10.6; AC-39, AC-40 merge rules, AC-45).
//
// Owner: lead developer.
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_progress.h"
#include "vocab_test.h"

static void test_crc_and_fingerprint(void)
{
    CHECK(vocab_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC-32/IEEE check value");
    const uint32_t fp = vocab_bank_fingerprint(vocab_bank_builtin());
    CHECK(fp == vocab_bank_fingerprint(vocab_bank_builtin()) && fp != 2166136261u, "fingerprint");

    // Order independence: the fingerprint is over the sorted uid list.
    const vocab_entry_t a[2] = { { .uid = 7, .text = "a" }, { .uid = 3, .text = "b" } };
    const vocab_entry_t b[2] = { { .uid = 3, .text = "b" }, { .uid = 7, .text = "a" } };
    const vocab_bank_t ba = { a, 2 };
    const vocab_bank_t bb = { b, 2 };
    CHECK(vocab_bank_fingerprint(&ba) == vocab_bank_fingerprint(&bb), "fingerprint sorts uids");
    // FNV-1a 32 of bytes 03 00 07 00, computed by hand.
    uint32_t h = 2166136261u;
    const uint8_t bytes[4] = { 3, 0, 7, 0 };
    for (int i = 0; i < 4; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    CHECK(vocab_bank_fingerprint(&ba) == h, "FNV-1a over little-endian uids");
}

static void test_meta_codec(void)
{
    vocab_meta_t meta;
    vocab_meta_defaults(&meta);
    CHECK(meta.version == VOCAB_FORMAT_VERSION && meta.volume_index == 0 && meta.tick == 0, "defaults");
    meta.tick = 0x1234;
    meta.xp = 0xA1B2C3D4u;
    meta.volume_index = 2;
    meta.chunk_count = 3;
    meta.bank_fingerprint = 0x01020304u;
    meta.objective_answers = 77;
    meta.first_try_correct = 55;
    uint8_t buf[VOCAB_META_BYTES];
    CHECK(vocab_meta_encode(&meta, buf, sizeof(buf)) == VOCAB_META_BYTES, "size 40");
    const uint8_t head[12] = { 0x4D, 0x56, 1, 2, 0x34, 0x12, 3, 0, 0xD4, 0xC3, 0xB2, 0xA1 };
    CHECK(memcmp(buf, head, sizeof(head)) == 0, "little-endian layout (AC-39)");
    for (int i = 24; i < 36; i++) CHECK(buf[i] == 0, "reserved byte %d", i);
    vocab_meta_t back;
    CHECK(vocab_meta_decode(buf, sizeof(buf), &back) && memcmp(&back, &meta, sizeof(meta)) == 0,
          "bit-exact round trip");
    uint8_t again[VOCAB_META_BYTES];
    vocab_meta_encode(&back, again, sizeof(again));
    CHECK(memcmp(buf, again, sizeof(buf)) == 0, "re-encode identical");
    for (size_t i = 0; i < sizeof(buf); i++) {
        uint8_t bad[VOCAB_META_BYTES];
        memcpy(bad, buf, sizeof(bad));
        bad[i] ^= 0x40u;
        CHECK(!vocab_meta_decode(bad, sizeof(bad), &back), "corruption at byte %u detected", (unsigned)i);
    }
    CHECK(!vocab_meta_decode(buf, sizeof(buf) - 1, &back), "short blob rejected");
    CHECK(vocab_meta_encode(&meta, buf, 39) == 0, "short buffer");
}

static void test_chunk_codec(void)
{
    vocab_record_t recs[VOCAB_NVS_RECORDS_PER_CHUNK];
    for (int i = 0; i < 64; i++) {
        recs[i].uid = (vocab_uid_t)(100 + i);
        recs[i].due = (vocab_tick_t)(1000 + 3 * i);
        recs[i].mastery = (uint8_t)(i % 6);
        recs[i].flags = (uint8_t)((i % 2 ? VOCAB_REC_SEEN : 0) | (i % 6 == 5 ? VOCAB_REC_EVER_MASTERED : 0));
        recs[i].lapses = (uint8_t)(i * 3);
        recs[i].reserved = 0;
    }
    uint8_t buf[VOCAB_CHUNK_MAX_BYTES];
    const size_t size = vocab_chunk_encode(recs, 64, 42, 2, buf, sizeof(buf));
    CHECK(size == 8 + 6 * 64 + 4 && size == VOCAB_CHUNK_MAX_BYTES, "full chunk size 396");
    CHECK(buf[0] == 0x50 && buf[1] == 0x56 && buf[2] == 1 && buf[3] == 64 && buf[4] == 42 &&
              buf[6] == 2, "chunk header");
    // Record 1: uid 101, due 1003, mastery 1, seen.
    CHECK(buf[14] == 101 && buf[15] == 0 && buf[16] == (1003 & 0xFF) && buf[17] == (1003 >> 8) &&
              buf[18] == (1 | 0x08) && buf[19] == 3, "record layout");
    vocab_chunk_header_t hdr;
    vocab_record_t back[64];
    CHECK(vocab_chunk_decode(buf, size, &hdr, back, 64) && hdr.count == 64 && hdr.tick == 42 &&
              hdr.index == 2, "decode header");
    CHECK(memcmp(back, recs, sizeof(recs)) == 0, "bit-exact record round trip (AC-39)");
    buf[100] ^= 1u;
    CHECK(!vocab_chunk_decode(buf, size, &hdr, back, 64), "CRC detects corruption");
    buf[100] ^= 1u;
    CHECK(!vocab_chunk_decode(buf, size - 6, &hdr, back, 64), "length mismatch rejected");
    buf[2] = 2;
    CHECK(!vocab_chunk_decode(buf, size, &hdr, back, 64), "other version rejected");
    CHECK(vocab_chunk_encode(recs, 65, 0, 0, buf, sizeof(buf)) == 0, "more than 64 records rejected");
    CHECK(vocab_chunk_encode(NULL, 0, 5, 0, buf, sizeof(buf)) == 12, "empty chunk");
}

static void test_merge(void)
{
    const vocab_entry_t entries[3] = { { .uid = 10 }, { .uid = 20 }, { .uid = 30 } };
    const vocab_bank_t bank = { entries, 3 };
    vocab_record_t records[3];
    memset(records, 0, sizeof(records));
    for (int i = 0; i < 3; i++) records[i].uid = entries[i].uid;
    bool assigned[3] = { false, false, false };
    const vocab_record_t loaded[] = {
        { 20, 15, 7, VOCAB_REC_SEEN | 0x80u, 4, 0 },   // mastery clamped, stray flag masked
        { 99, 1, 1, VOCAB_REC_SEEN, 0, 0 },             // unknown uid dropped
        { 20, 3, 1, VOCAB_REC_SEEN, 9, 0 },             // duplicate: first kept
        { 10, 900, 2, VOCAB_REC_SEEN, 0, 0 },           // absurd due repaired
    };
    const size_t n = vocab_progress_merge(&bank, records, assigned, loaded, 4, 100);
    CHECK(n == 2, "two records applied (got %u)", (unsigned)n);
    CHECK(records[1].mastery == 5 && records[1].due == 15 && records[1].lapses == 4 &&
              records[1].flags == VOCAB_REC_SEEN, "clamp + mask + first duplicate kept (AC-40)");
    CHECK(records[0].due == 100 + 2 && records[0].mastery == 2, "due > tick + 40 reset to tick + INTERVAL[m]");
    CHECK(records[2].mastery == 0 && !vocab_record_seen(&records[2]) && !assigned[2],
          "bank uid without a record starts fresh");
}

static void test_budget(void)
{
    const uint32_t nvs = 0x6000;  // partitions.csv
    vocab_nvs_budget_t b = vocab_nvs_budget(48, nvs);
    CHECK(b.chunks == 1 && b.usable == 630 && b.entries >= 15 && b.entries <= 22 && b.percent <= 4 &&
              !b.too_tight, "48 words: about 20 entries, 3 %% (got %u, %u %%)", b.entries, b.percent);
    b = vocab_nvs_budget(1000, nvs);
    CHECK(b.chunks == 16 && b.entries >= 235 && b.entries <= 250 && b.percent >= 37 && b.percent <= 40 &&
              !b.too_tight, "1000 words: about 245 entries, 39 %% (got %u, %u %%)", b.entries, b.percent);
    b = vocab_nvs_budget(2000, nvs);
    CHECK(b.chunks == 32 && b.entries >= 470 && b.entries <= 490 && b.percent >= 74 && b.too_tight,
          "2000 words flagged too tight (got %u, %u %%) (AC-45)", b.entries, b.percent);
    b = vocab_nvs_budget(2000, 0xC000);
    CHECK(!b.too_tight, "a 48 KB partition holds 2000 words");
}

int main(void)
{
    test_crc_and_fingerprint();
    test_meta_codec();
    test_chunk_codec();
    test_merge();
    test_budget();
    return vocab_test_report("vocab progress codec tests");
}
