// tests/test_vocab_store.c - persistence policy over a fake key-value store (spec 10.3 -
// 10.5; AC-40 rewrite, AC-41, AC-42, AC-44).
//
// Owner: lead developer.
#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_store.h"
#include "vocab_test.h"

// ---------------------------------------------------------------------------------------
// Fake NVS: a small table of blobs with an operation log and failure injection.
// ---------------------------------------------------------------------------------------
#define MAX_KEYS 40
typedef struct {
    char key[16];
    uint8_t data[VOCAB_CHUNK_MAX_BYTES];
    size_t len;
    bool used;
} blob_t;

typedef struct {
    blob_t blobs[MAX_KEYS];
    char log[64][20];
    int log_len;
    int sets;
    int erases;
    int commits;
    int fail_set_at;      // 1-based index of the set() call that fails (0 = never)
    bool fail_commit;
    bool fail_reads;
} fake_kv_t;

static fake_kv_t KV;

static blob_t *find(const char *key)
{
    for (int i = 0; i < MAX_KEYS; i++) {
        if (KV.blobs[i].used && strcmp(KV.blobs[i].key, key) == 0) return &KV.blobs[i];
    }
    return NULL;
}

static void log_op(const char *op, const char *key)
{
    if (KV.log_len < 64) snprintf(KV.log[KV.log_len++], sizeof(KV.log[0]), "%s %s", op, key);
}

static vocab_kv_status_t kv_get(void *ctx, const char *key, void *buf, size_t *len)
{
    (void)ctx;
    if (KV.fail_reads) return VOCAB_KV_ERROR;
    const blob_t *b = find(key);
    if (!b) return VOCAB_KV_MISSING;
    if (b->len > *len) return VOCAB_KV_ERROR;
    memcpy(buf, b->data, b->len);
    *len = b->len;
    return VOCAB_KV_OK;
}

static bool kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    KV.sets++;
    if (KV.fail_set_at && KV.sets == KV.fail_set_at) return false;
    blob_t *b = find(key);
    for (int i = 0; !b && i < MAX_KEYS; i++) {
        if (!KV.blobs[i].used) b = &KV.blobs[i];
    }
    if (!b || len > sizeof(b->data)) return false;
    b->used = true;
    snprintf(b->key, sizeof(b->key), "%s", key);
    memcpy(b->data, buf, len);
    b->len = len;
    log_op("set", key);
    return true;
}

static bool kv_erase(void *ctx, const char *key)
{
    (void)ctx;
    KV.erases++;
    blob_t *b = find(key);
    if (b) b->used = false;
    log_op("erase", key);
    return true;
}

static bool kv_commit(void *ctx)
{
    (void)ctx;
    KV.commits++;
    log_op("commit", "");
    return !KV.fail_commit;
}

static const vocab_kv_ops_t OPS = { kv_get, kv_set, kv_erase, kv_commit };

// Fake audio gate: busy for `busy_ms`, counting the time waited.
typedef struct {
    uint32_t busy_ms;
    uint32_t waited;
} fake_gate_t;

static bool gate_idle(void *ctx)
{
    const fake_gate_t *g = ctx;
    return g->waited >= g->busy_ms;
}

static void gate_wait(void *ctx, uint32_t ms)
{
    fake_gate_t *g = ctx;
    g->waited += ms;
}

static void kv_reset(void)
{
    memset(&KV, 0, sizeof(KV));
}

static void clear_log(void)
{
    KV.log_len = 0;
    KV.sets = 0;
    KV.erases = 0;
    KV.commits = 0;
}

// ---------------------------------------------------------------------------------------

static vocab_store_t store;
static vocab_record_t records[VOCAB_RECORDS_MAX];
static vocab_meta_t meta;

static void load(const vocab_bank_t *bank)
{
    vocab_store_init(&store, &OPS, NULL);
    vocab_store_load(&store, bank, records, &meta);
}

static void test_fresh_and_round_trip(void)
{
    kv_reset();
    const vocab_bank_t *bank = vocab_bank_builtin();
    load(bank);
    CHECK(!store.volatile_mode && !vocab_store_warning(&store), "fresh storage usable");
    CHECK(meta.tick == 0 && meta.xp == 0 && records[0].uid == bank->entries[0].uid, "fresh defaults");

    records[3].mastery = 2;
    records[3].due = 9;
    records[3].flags = VOCAB_REC_SEEN;
    meta.tick = 7;
    meta.xp = 123;
    meta.volume_index = 1;
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK, "first save");
    CHECK(KV.log_len == 3 && strcmp(KV.log[0], "set p00") == 0 && strcmp(KV.log[1], "set meta") == 0 &&
              strcmp(KV.log[2], "commit ") == 0, "chunk before meta, one commit (AC-42)");
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_CLEAN && KV.sets == 0,
          "nothing changed -> no write");

    meta.volume_index = 2;  // volume only: meta only
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK &&
              KV.log_len == 2 && strcmp(KV.log[0], "set meta") == 0, "volume change writes meta only");

    // Reload ("reboot") sees the same state.
    vocab_record_t saved3 = records[3];
    load(bank);
    CHECK(memcmp(&records[3], &saved3, sizeof(saved3)) == 0 && meta.tick == 7 && meta.xp == 123 &&
              meta.volume_index == 2, "round trip through storage");
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_CLEAN,
          "a reload does not dirty anything");
}

static void test_gate(void)
{
    kv_reset();
    const vocab_bank_t *bank = vocab_bank_builtin();
    load(bank);
    records[0].mastery = 1;
    records[0].flags = VOCAB_REC_SEEN;
    fake_gate_t gate_state = { 5000, 0 };
    const vocab_audio_gate_t gate = { gate_idle, gate_wait, &gate_state };
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, &gate, VOCAB_SAVE_AUDIO_WAIT_MS) ==
              VOCAB_SAVE_DEFERRED, "audio busy: deferred");
    CHECK(KV.sets == 0 && KV.commits == 0, "no NVS write while audio is busy (AC-42)");
    CHECK(gate_state.waited >= VOCAB_SAVE_AUDIO_WAIT_MS && gate_state.waited < VOCAB_SAVE_AUDIO_WAIT_MS + 20,
          "waited up to 2000 ms (%u)", gate_state.waited);
    gate_state.busy_ms = 300;
    gate_state.waited = 0;
    CHECK(vocab_store_save(&store, bank, records, &meta, &gate, VOCAB_SAVE_AUDIO_WAIT_MS) == VOCAB_SAVE_OK &&
              gate_state.waited >= 300, "writes once audio is idle");
    gate_state.busy_ms = 50;
    gate_state.waited = 0;
    records[1].flags = VOCAB_REC_SEEN;
    CHECK(vocab_store_save(&store, bank, records, &meta, &gate, 0) == VOCAB_SAVE_DEFERRED,
          "opportunistic save (no wait) defers while busy");
}

static void test_failures(void)
{
    // NVS init failure: volatile mode, never writes, warning (AC-44).
    kv_reset();
    vocab_store_init(&store, NULL, NULL);
    vocab_store_load(&store, vocab_bank_builtin(), records, &meta);
    CHECK(store.volatile_mode && vocab_store_warning(&store), "volatile + warning");
    CHECK(vocab_store_save(&store, vocab_bank_builtin(), records, &meta, NULL, 0) == VOCAB_SAVE_VOLATILE &&
              KV.sets == 0 && KV.erases == 0, "volatile mode never writes or erases");

    // A failing set keeps the RAM state and retries at the next save point.
    kv_reset();
    const vocab_bank_t *bank = vocab_bank_builtin();
    load(bank);
    records[2].flags = VOCAB_REC_SEEN;
    KV.fail_set_at = 1;
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_FAILED &&
              vocab_store_warning(&store), "set failure -> warning");
    KV.fail_set_at = 0;
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK &&
              !vocab_store_warning(&store) && KV.sets == 2, "retry succeeds and clears the warning");
    records[2].mastery = 3;
    KV.fail_commit = true;
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_FAILED, "commit failure");
    KV.fail_commit = false;
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK && KV.sets >= 1,
          "dirty state kept after a failed commit");

    // Unreadable storage at boot: defaults, but storage stays usable.
    KV.fail_reads = true;
    load(bank);
    CHECK(!store.volatile_mode && records[2].mastery == 0, "read errors -> defaults");
    KV.fail_reads = false;
}

static void test_corruption_and_migration(void)
{
    const vocab_bank_t *bank = vocab_bank_builtin();
    // Corrupt chunk: only its words restart (AC-41). Use a bank of 100 entries -> 2 chunks.
    static vocab_entry_t big[100];
    for (int i = 0; i < 100; i++) {
        big[i] = bank->entries[i % bank->count];
        big[i].uid = (vocab_uid_t)(5000 + i);
    }
    const vocab_bank_t big_bank = { big, 100 };
    kv_reset();
    load(&big_bank);
    CHECK(store.chunk_count == 2, "100 words -> 2 chunks");
    for (int i = 0; i < 100; i++) {
        records[i].mastery = 3;
        records[i].flags = VOCAB_REC_SEEN;
        records[i].due = 20;
    }
    meta.tick = 15;
    CHECK(vocab_store_save(&store, &big_bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK, "save 2 chunks");
    find("p01")->data[20] ^= 0xFFu;
    load(&big_bank);
    CHECK(records[0].mastery == 3 && records[63].mastery == 3, "chunk 0 intact");
    CHECK(records[64].mastery == 0 && records[99].mastery == 0 && !vocab_record_seen(&records[64]),
          "corrupt chunk 1 words restart fresh");
    clear_log();
    CHECK(vocab_store_save(&store, &big_bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK &&
              strcmp(KV.log[0], "set p01") == 0, "only the repaired chunk is rewritten");

    // Corrupt meta: defaults with tick = max(tick at write) (AC-41).
    find("meta")->data[8] ^= 0x01u;
    load(&big_bank);
    CHECK(meta.tick == 15 && meta.xp == 0 && meta.volume_index == 0 && records[0].mastery == 3,
          "corrupt meta -> defaults, tick from chunks");

    // Newer format: volatile, never overwritten (AC-41).
    vocab_meta_t newer;
    vocab_meta_defaults(&newer);
    newer.version = VOCAB_FORMAT_VERSION + 1;
    uint8_t blob[VOCAB_META_BYTES];
    vocab_meta_encode(&newer, blob, sizeof(blob));
    kv_set(NULL, "meta", blob, sizeof(blob));
    load(&big_bank);
    clear_log();
    CHECK(store.volatile_mode && vocab_store_save(&store, &big_bank, records, &meta, NULL, 0) ==
              VOCAB_SAVE_VOLATILE && KV.sets == 0 && KV.erases == 0, "newer version never overwritten");

    // A newer meta with another length/layout (no valid CRC for this firmware) is recognized
    // by its header alone and left alone too.
    uint8_t longer[VOCAB_META_BYTES + 8];
    memset(longer, 0xA5, sizeof(longer));
    longer[0] = (uint8_t)(VOCAB_META_MAGIC & 0xFFu);
    longer[1] = (uint8_t)(VOCAB_META_MAGIC >> 8);
    longer[2] = (uint8_t)(VOCAB_FORMAT_VERSION + 1);
    kv_set(NULL, "meta", longer, sizeof(longer));
    load(&big_bank);
    clear_log();
    CHECK(store.volatile_mode && vocab_store_save(&store, &big_bank, records, &meta, NULL, 0) ==
              VOCAB_SAVE_VOLATILE && KV.sets == 0 && KV.erases == 0,
          "newer meta of another length never overwritten");

    // Bank change: fingerprint mismatch rewrites everything and erases surplus keys (AC-40).
    kv_reset();
    load(&big_bank);
    for (int i = 0; i < 100; i++) records[i].flags = VOCAB_REC_SEEN;
    vocab_store_save(&store, &big_bank, records, &meta, NULL, 0);
    load(bank);  // the 48-word bank shares no uid with the 100-word test bank
    CHECK(store.force_all && store.stored_chunks == 2, "fingerprint change forces a rewrite");
    CHECK(!vocab_record_seen(&records[0]), "unknown uids dropped, bank uids fresh");
    clear_log();
    CHECK(vocab_store_save(&store, bank, records, &meta, NULL, 0) == VOCAB_SAVE_OK, "rewrite");
    CHECK(KV.log_len == 4 && strcmp(KV.log[0], "set p00") == 0 && strcmp(KV.log[1], "set meta") == 0 &&
              strcmp(KV.log[2], "erase p01") == 0 && strcmp(KV.log[3], "commit ") == 0,
          "chunks, meta, surplus erase, commit");
    CHECK(!find("p01"), "surplus chunk key removed");
}

int main(void)
{
    test_fresh_and_round_trip();
    test_gate();
    test_failures();
    test_corruption_and_migration();
    return vocab_test_report("vocab store tests");
}
