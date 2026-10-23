// main/vocab_store.c - persistence policy over an injected key-value interface (see
// vocab_store.h).
#include "vocab_store.h"

#include <stdio.h>
#include <string.h>

#define META_KEY "meta"

static void chunk_key(char *key, size_t cap, unsigned index)
{
    snprintf(key, cap, "p%02u", index);
}

static uint16_t chunks_for(vocab_index_t count)
{
    return (uint16_t)((count + VOCAB_NVS_RECORDS_PER_CHUNK - 1u) / VOCAB_NVS_RECORDS_PER_CHUNK);
}

void vocab_store_init(vocab_store_t *store, const vocab_kv_ops_t *kv, void *kv_ctx)
{
    memset(store, 0, sizeof(*store));
    store->kv = kv;
    store->kv_ctx = kv_ctx;
    store->volatile_mode = kv == NULL || !kv->get || !kv->set || !kv->erase || !kv->commit;
}

// Bank indices sorted by uid (insertion sort; banks are small and this runs once).
static void build_order(vocab_store_t *store, const vocab_bank_t *bank)
{
    for (vocab_index_t i = 0; i < bank->count; i++) {
        vocab_index_t j = i;
        while (j > 0 && bank->entries[store->order[j - 1]].uid > bank->entries[i].uid) {
            store->order[j] = store->order[j - 1];
            j--;
        }
        store->order[j] = i;
    }
}

// Records of chunk `c` in uid order; returns the record count.
static uint8_t gather_chunk(const vocab_store_t *store, const vocab_record_t *records, uint16_t c,
                            vocab_record_t *out)
{
    const uint32_t first = (uint32_t)c * VOCAB_NVS_RECORDS_PER_CHUNK;
    uint8_t n = 0;
    for (uint32_t k = first; k < store->count && n < VOCAB_NVS_RECORDS_PER_CHUNK; k++) {
        out[n++] = records[store->order[k]];
    }
    return n;
}

// CRC of the record payload of an encoded chunk (count byte + records), so that a changed
// tick at write alone does not force a rewrite.
static uint32_t payload_crc(const uint8_t *blob, size_t size)
{
    if (size < VOCAB_CHUNK_HEADER_BYTES + VOCAB_CHUNK_CRC_BYTES) return 0;
    const size_t body = size - VOCAB_CHUNK_HEADER_BYTES - VOCAB_CHUNK_CRC_BYTES;
    return vocab_crc32(blob + 3, 1) ^ vocab_crc32(blob + VOCAB_CHUNK_HEADER_BYTES, body);
}

bool vocab_store_load(vocab_store_t *store, const vocab_bank_t *bank, vocab_record_t *records,
                      vocab_meta_t *meta)
{
    vocab_meta_defaults(meta);
    if (!bank || !bank->entries || !records || bank->count > VOCAB_RECORDS_MAX) {
        store->volatile_mode = true;
        return false;
    }
    meta->bank_fingerprint = vocab_bank_fingerprint(bank);
    store->count = bank->count;
    store->chunk_count = chunks_for(bank->count);
    build_order(store, bank);
    for (vocab_index_t i = 0; i < bank->count; i++) {
        memset(&records[i], 0, sizeof(records[i]));
        records[i].uid = bank->entries[i].uid;
    }
    if (store->volatile_mode) return false;

    // Meta first: a newer format must never be overwritten, whatever its length or layout.
    uint8_t buf[VOCAB_CHUNK_MAX_BYTES];
    size_t len = sizeof(buf);
    vocab_meta_t stored;
    bool meta_valid = false;
    if (store->kv->get(store->kv_ctx, META_KEY, buf, &len) == VOCAB_KV_OK) {
        if (len >= 3 && buf[0] == (VOCAB_META_MAGIC & 0xFFu) && buf[1] == (VOCAB_META_MAGIC >> 8) &&
            buf[2] > VOCAB_FORMAT_VERSION) {
            store->volatile_mode = true;
            return false;
        }
        if (vocab_meta_decode(buf, len, &stored)) {
            meta_valid = true;
            store->meta_known = true;
            store->meta_crc = vocab_crc32(buf, VOCAB_META_BYTES - VOCAB_CHUNK_CRC_BYTES);
        }
    }

    // First pass over chunks: presence, validity, newer versions, highest tick at write.
    bool valid[VOCAB_STORE_PROBE_CHUNKS] = { false };
    vocab_tick_t max_tick = 0;
    bool any_chunk = false;
    for (unsigned c = 0; c < VOCAB_STORE_PROBE_CHUNKS; c++) {
        char key[8];
        chunk_key(key, sizeof(key), c);
        len = sizeof(buf);
        const vocab_kv_status_t st = store->kv->get(store->kv_ctx, key, buf, &len);
        if (st != VOCAB_KV_OK) continue;  // missing or unreadable
        store->stored_chunks = (uint16_t)(c + 1u);
        if (len >= 3 && buf[0] == (VOCAB_CHUNK_MAGIC & 0xFFu) &&
            buf[1] == (VOCAB_CHUNK_MAGIC >> 8) && buf[2] > VOCAB_FORMAT_VERSION) {
            store->volatile_mode = true;  // written by a newer firmware: leave it alone
            return false;
        }
        vocab_chunk_header_t hdr;
        if (!vocab_chunk_decode(buf, len, &hdr, NULL, 0) || hdr.index != c) continue;
        valid[c] = true;
        any_chunk = true;
        if (hdr.tick > max_tick) max_tick = hdr.tick;
    }

    if (meta_valid) {
        *meta = stored;
        if (meta->volume_index >= VOCAB_VOL_LEVEL_COUNT) meta->volume_index = VOCAB_VOL_DEFAULT_INDEX;
        if (stored.bank_fingerprint != vocab_bank_fingerprint(bank) ||
            stored.version < VOCAB_FORMAT_VERSION) {
            store->force_all = true;
        }
    } else if (any_chunk) {
        meta->tick = max_tick;  // corrupt meta: keep dues meaningful (spec 10.5)
    }
    meta->version = (uint8_t)VOCAB_FORMAT_VERSION;
    meta->bank_fingerprint = vocab_bank_fingerprint(bank);

    // Second pass: merge valid chunks by uid.
    bool assigned[VOCAB_RECORDS_MAX] = { false };
    vocab_record_t loaded[VOCAB_NVS_RECORDS_PER_CHUNK];
    size_t merged = 0;
    for (unsigned c = 0; c < VOCAB_STORE_PROBE_CHUNKS; c++) {
        if (!valid[c]) continue;
        char key[8];
        chunk_key(key, sizeof(key), c);
        len = sizeof(buf);
        vocab_chunk_header_t hdr;
        if (store->kv->get(store->kv_ctx, key, buf, &len) != VOCAB_KV_OK ||
            !vocab_chunk_decode(buf, len, &hdr, loaded, VOCAB_NVS_RECORDS_PER_CHUNK)) {
            continue;
        }
        merged += vocab_progress_merge(bank, records, assigned, loaded, hdr.count, meta->tick);
        if (c < VOCAB_STORE_MAX_CHUNKS) {
            store->chunk_known[c] = true;
            store->chunk_crc[c] = payload_crc(buf, len);
        }
    }
    return meta_valid || merged > 0;
}

bool vocab_store_warning(const vocab_store_t *store)
{
    return store->volatile_mode || store->save_failed;
}

// Encodes chunk `c` of the current records into buf; returns its size.
static size_t encode_chunk(const vocab_store_t *store, const vocab_record_t *records, uint16_t c,
                           vocab_tick_t tick, uint8_t *buf, size_t cap)
{
    vocab_record_t chunk[VOCAB_NVS_RECORDS_PER_CHUNK];
    const uint8_t n = gather_chunk(store, records, c, chunk);
    return vocab_chunk_encode(chunk, n, tick, c, buf, cap);
}

// Writes changed chunks, then meta, erases surplus chunk keys and commits once.
static bool write_changes(vocab_store_t *store, const vocab_record_t *records,
                          const vocab_meta_t *meta, const bool *changed, bool write_meta,
                          const uint8_t *meta_buf)
{
    uint8_t buf[VOCAB_CHUNK_MAX_BYTES];
    char key[8];
    for (uint16_t c = 0; c < store->chunk_count; c++) {
        if (!changed[c]) continue;
        const size_t size = encode_chunk(store, records, c, meta->tick, buf, sizeof(buf));
        chunk_key(key, sizeof(key), c);
        if (!store->kv->set(store->kv_ctx, key, buf, size)) return false;
    }
    if (write_meta && !store->kv->set(store->kv_ctx, META_KEY, meta_buf, VOCAB_META_BYTES)) {
        return false;
    }
    for (uint16_t c = store->chunk_count; c < store->stored_chunks; c++) {
        chunk_key(key, sizeof(key), c);
        if (!store->kv->erase(store->kv_ctx, key)) return false;
    }
    return store->kv->commit(store->kv_ctx);
}

vocab_save_result_t vocab_store_save(vocab_store_t *store, const vocab_bank_t *bank,
                                     const vocab_record_t *records, const vocab_meta_t *meta,
                                     const vocab_audio_gate_t *gate, uint32_t max_wait_ms)
{
    if (store->volatile_mode) return VOCAB_SAVE_VOLATILE;
    if (!bank || !records || !meta || bank->count != store->count) return VOCAB_SAVE_FAILED;

    uint8_t buf[VOCAB_CHUNK_MAX_BYTES];
    bool changed[VOCAB_STORE_MAX_CHUNKS] = { false };
    uint32_t crc[VOCAB_STORE_MAX_CHUNKS] = { 0 };
    bool any = false;
    for (uint16_t c = 0; c < store->chunk_count; c++) {
        const size_t size = encode_chunk(store, records, c, meta->tick, buf, sizeof(buf));
        crc[c] = payload_crc(buf, size);
        changed[c] = store->force_all || !store->chunk_known[c] || store->chunk_crc[c] != crc[c];
        any = any || changed[c];
    }
    vocab_meta_t out = *meta;
    out.version = (uint8_t)VOCAB_FORMAT_VERSION;
    out.chunk_count = store->chunk_count;
    out.bank_fingerprint = vocab_bank_fingerprint(bank);
    uint8_t meta_buf[VOCAB_META_BYTES];
    vocab_meta_encode(&out, meta_buf, sizeof(meta_buf));
    // CRC of the content only: a CRC over a blob that embeds its own CRC is a constant.
    const uint32_t meta_crc = vocab_crc32(meta_buf, VOCAB_META_BYTES - VOCAB_CHUNK_CRC_BYTES);
    const bool meta_changed = store->force_all || !store->meta_known || store->meta_crc != meta_crc;
    const bool surplus = store->stored_chunks > store->chunk_count;
    if (!any && !meta_changed && !surplus) return VOCAB_SAVE_CLEAN;

    // Save gate: never write flash while audio is playing.
    if (gate && gate->idle) {
        uint32_t waited = 0;
        while (!gate->idle(gate->ctx)) {
            if (waited >= max_wait_ms || !gate->wait) return VOCAB_SAVE_DEFERRED;
            gate->wait(gate->ctx, VOCAB_STORE_GATE_STEP_MS);
            waited += VOCAB_STORE_GATE_STEP_MS;
        }
    }

    if (!write_changes(store, records, meta, changed, meta_changed || any, meta_buf)) {
        store->save_failed = true;
        return VOCAB_SAVE_FAILED;
    }
    for (uint16_t c = 0; c < store->chunk_count; c++) {
        store->chunk_known[c] = true;
        store->chunk_crc[c] = crc[c];
    }
    store->meta_known = true;
    store->meta_crc = meta_crc;
    store->stored_chunks = store->chunk_count;
    store->force_all = false;
    store->save_failed = false;
    return VOCAB_SAVE_OK;
}
