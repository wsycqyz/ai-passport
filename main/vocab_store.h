// main/vocab_store.h - persistence policy over an injected key-value interface (spec 10).
//
// Layout (NVS namespace VOCAB_NVS_NAMESPACE): `meta` (vocab_progress meta blob) and progress
// chunks `p00`, `p01`, ... holding the records in uid order, 64 per chunk.
//
// Load (spec 10.3): read meta, then every chunk key p00..p31; validate each blob; merge
// records into the bank by uid (vocab_progress_merge); a corrupt chunk resets only its words;
// a corrupt or missing meta yields defaults with tick = max(tick at write) of the valid
// chunks; a bank fingerprint change (or an older version) forces a full rewrite that also
// erases surplus chunk keys; a newer format version (meta or chunk) switches to volatile
// mode so nothing is ever overwritten.
// Save (spec 10.4): only chunks whose record payload changed since the last successful save
// are written, then meta last, then surplus keys are erased, then one commit. Before any
// write the audio gate is polled until the worker is idle (up to `max_wait_ms`); otherwise
// the save is deferred with nothing written. Failures keep the RAM state and retry at the
// next save point; HOME shows the warning while storage is volatile or the last save failed.
// The store never erases the partition (no nvs_flash_erase).
//
// No ESP-IDF includes: the device adapter (nvs_get_blob/nvs_set_blob/nvs_erase_key/
// nvs_commit) lives in vocab_app.c; host tests pass fakes. Caller: the app task only (never
// the LVGL task or a button callback). Blocks only inside the kv operations and the gate's
// wait callback.
// Owner: lead developer (tests/test_vocab_store.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vocab_config.h"
#include "vocab_progress.h"
#include "vocab_types.h"

typedef enum {
    VOCAB_KV_OK = 0,
    VOCAB_KV_MISSING,   // the key does not exist
    VOCAB_KV_ERROR,     // read error or the blob does not fit the buffer
} vocab_kv_status_t;

// Key-value backend.
typedef struct {
    // Reads blob `key` into buf; *len holds the capacity in and the blob size out.
    vocab_kv_status_t (*get)(void *ctx, const char *key, void *buf, size_t *len);
    bool (*set)(void *ctx, const char *key, const void *buf, size_t len);
    bool (*erase)(void *ctx, const char *key);  // erasing a missing key succeeds
    bool (*commit)(void *ctx);
} vocab_kv_ops_t;

// Save gate (spec 10.4): writes happen only while the audio worker is idle.
typedef struct {
    bool (*idle)(void *ctx);               // true when no PCM is being written
    void (*wait)(void *ctx, uint32_t ms);  // block the caller (device: vTaskDelay)
    void *ctx;
} vocab_audio_gate_t;

#define VOCAB_STORE_GATE_STEP_MS  10u
#define VOCAB_STORE_MAX_CHUNKS    ((VOCAB_RECORDS_MAX + VOCAB_NVS_RECORDS_PER_CHUNK - 1u) / \
                                   VOCAB_NVS_RECORDS_PER_CHUNK)
#define VOCAB_STORE_PROBE_CHUNKS  32u  // chunk keys inspected at load (2048 words)

typedef enum {
    VOCAB_SAVE_OK = 0,     // changed data written and committed
    VOCAB_SAVE_CLEAN,      // nothing changed since the last successful save
    VOCAB_SAVE_DEFERRED,   // audio stayed busy: nothing written, retry at the next save point
    VOCAB_SAVE_VOLATILE,   // volatile mode: nothing is ever written
    VOCAB_SAVE_FAILED,     // a kv operation failed: RAM state kept, warning shown
} vocab_save_result_t;

typedef struct {
    const vocab_kv_ops_t *kv;      // NULL = volatile mode
    void *kv_ctx;
    bool volatile_mode;            // storage unusable or newer format: RAM only
    bool save_failed;              // last save failed: HOME warning until a save succeeds
    bool force_all;                // rewrite meta + every chunk + erase surplus keys
    uint16_t stored_chunks;        // chunk keys present in storage (p00 .. p(n-1))
    vocab_index_t count;           // bank entries
    uint16_t chunk_count;          // chunks needed for the bank
    bool chunk_known[VOCAB_STORE_MAX_CHUNKS];   // payload CRC below matches storage
    uint32_t chunk_crc[VOCAB_STORE_MAX_CHUNKS]; // CRC of the stored record payload
    bool meta_known;
    uint32_t meta_crc;                          // CRC of the stored meta blob
    vocab_index_t order[VOCAB_RECORDS_MAX];     // bank indices in ascending uid order
} vocab_store_t;

// `kv` NULL (e.g. nvs_flash_init failed) selects volatile mode.
void vocab_store_init(vocab_store_t *store, const vocab_kv_ops_t *kv, void *kv_ctx);

// Fills `records` (bank-parallel, bank->count <= VOCAB_RECORDS_MAX) and `meta` from storage,
// using fresh defaults for anything missing or corrupt. Returns true when stored progress was
// used. A bank larger than VOCAB_RECORDS_MAX switches to volatile mode and returns false.
bool vocab_store_load(vocab_store_t *store, const vocab_bank_t *bank, vocab_record_t *records,
                      vocab_meta_t *meta);

// Save point. `meta` provides tick, XP, volume and statistics (chunk count, version and
// fingerprint are filled in by the store). `gate` may be NULL (no audio in this build).
vocab_save_result_t vocab_store_save(vocab_store_t *store, const vocab_bank_t *bank,
                                     const vocab_record_t *records, const vocab_meta_t *meta,
                                     const vocab_audio_gate_t *gate, uint32_t max_wait_ms);

// True when HOME must show the "progress cannot be saved" warning.
bool vocab_store_warning(const vocab_store_t *store);
