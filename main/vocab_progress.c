// main/vocab_progress.c - persistent-state codec (see vocab_progress.h).
#include "vocab_progress.h"

#include <string.h>

#include "vocab_config.h"

#define STATE_MASTERY_MASK 0x07u
#define STATE_SEEN         0x08u
#define STATE_EVER         0x10u

uint32_t vocab_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; data && i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t vocab_bank_fingerprint(const vocab_bank_t *bank)
{
    uint32_t hash = 2166136261u;
    if (!bank || !bank->entries) return hash;
    // Emit uids in ascending order without a sorted copy: repeatedly take the next larger uid.
    uint32_t last = 0;
    bool first = true;
    for (vocab_index_t n = 0; n < bank->count; n++) {
        uint32_t best = 0x10000u;
        for (vocab_index_t i = 0; i < bank->count; i++) {
            const uint32_t uid = bank->entries[i].uid;
            if ((first || uid > last) && uid < best) best = uid;
        }
        if (best > 0xFFFFu) break;  // duplicates collapse; the bank test rejects them anyway
        const uint8_t bytes[2] = { (uint8_t)(best & 0xFFu), (uint8_t)(best >> 8) };
        for (int b = 0; b < 2; b++) {
            hash ^= bytes[b];
            hash *= 16777619u;
        }
        last = best;
        first = false;
    }
    return hash;
}

void vocab_meta_defaults(vocab_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->version = (uint8_t)VOCAB_FORMAT_VERSION;
    meta->volume_index = (uint8_t)VOCAB_VOL_DEFAULT_INDEX;
}

static void put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, v & 0xFFFFu);
    put_u16(p + 2, v >> 16);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16);
}

size_t vocab_meta_encode(const vocab_meta_t *meta, uint8_t *buf, size_t cap)
{
    if (!meta || !buf || cap < VOCAB_META_BYTES) return 0;
    memset(buf, 0, VOCAB_META_BYTES);
    put_u16(buf + 0, VOCAB_META_MAGIC);
    buf[2] = meta->version;
    buf[3] = meta->volume_index;
    put_u16(buf + 4, meta->tick);
    put_u16(buf + 6, meta->chunk_count);
    put_u32(buf + 8, meta->xp);
    put_u32(buf + 12, meta->bank_fingerprint);
    put_u32(buf + 16, meta->objective_answers);
    put_u32(buf + 20, meta->first_try_correct);
    // bytes 24..35 reserved (0)
    put_u32(buf + 36, vocab_crc32(buf, 36));
    return VOCAB_META_BYTES;
}

bool vocab_meta_decode(const uint8_t *buf, size_t len, vocab_meta_t *meta)
{
    if (!buf || !meta || len != VOCAB_META_BYTES) return false;
    if (get_u16(buf) != VOCAB_META_MAGIC) return false;
    if (get_u32(buf + 36) != vocab_crc32(buf, 36)) return false;
    meta->version = buf[2];
    meta->volume_index = buf[3];
    meta->tick = get_u16(buf + 4);
    meta->chunk_count = get_u16(buf + 6);
    meta->xp = get_u32(buf + 8);
    meta->bank_fingerprint = get_u32(buf + 12);
    meta->objective_answers = get_u32(buf + 16);
    meta->first_try_correct = get_u32(buf + 20);
    return true;
}

size_t vocab_chunk_encode(const vocab_record_t *records, uint8_t count, vocab_tick_t tick,
                          uint16_t index, uint8_t *buf, size_t cap)
{
    const size_t size = VOCAB_CHUNK_HEADER_BYTES + (size_t)count * VOCAB_CHUNK_RECORD_BYTES +
                        VOCAB_CHUNK_CRC_BYTES;
    if (!buf || (count > 0 && !records) || count > VOCAB_NVS_RECORDS_PER_CHUNK || cap < size) {
        return 0;
    }
    put_u16(buf + 0, VOCAB_CHUNK_MAGIC);
    buf[2] = (uint8_t)VOCAB_FORMAT_VERSION;
    buf[3] = count;
    put_u16(buf + 4, tick);
    put_u16(buf + 6, index);
    uint8_t *p = buf + VOCAB_CHUNK_HEADER_BYTES;
    for (uint8_t i = 0; i < count; i++, p += VOCAB_CHUNK_RECORD_BYTES) {
        const vocab_record_t *r = &records[i];
        uint8_t state = (uint8_t)(r->mastery & STATE_MASTERY_MASK);
        if (r->flags & VOCAB_REC_SEEN) state |= STATE_SEEN;
        if (r->flags & VOCAB_REC_EVER_MASTERED) state |= STATE_EVER;
        put_u16(p, r->uid);
        put_u16(p + 2, r->due);
        p[4] = state;
        p[5] = r->lapses;
    }
    put_u32(p, vocab_crc32(buf, (size_t)(p - buf)));
    return size;
}

bool vocab_chunk_decode(const uint8_t *buf, size_t len, vocab_chunk_header_t *header,
                        vocab_record_t *records, size_t cap)
{
    if (!buf || !header || len < VOCAB_CHUNK_HEADER_BYTES + VOCAB_CHUNK_CRC_BYTES) return false;
    if (get_u16(buf) != VOCAB_CHUNK_MAGIC || buf[2] != VOCAB_FORMAT_VERSION) return false;
    const uint8_t count = buf[3];
    const size_t body = (size_t)count * VOCAB_CHUNK_RECORD_BYTES;
    if (count > VOCAB_NVS_RECORDS_PER_CHUNK ||
        len != VOCAB_CHUNK_HEADER_BYTES + body + VOCAB_CHUNK_CRC_BYTES) {
        return false;
    }
    const size_t crc_at = VOCAB_CHUNK_HEADER_BYTES + body;
    if (get_u32(buf + crc_at) != vocab_crc32(buf, crc_at)) return false;
    header->version = buf[2];
    header->count = count;
    header->tick = get_u16(buf + 4);
    header->index = get_u16(buf + 6);
    const uint8_t *p = buf + VOCAB_CHUNK_HEADER_BYTES;
    for (uint8_t i = 0; i < count && i < cap && records; i++, p += VOCAB_CHUNK_RECORD_BYTES) {
        vocab_record_t *r = &records[i];
        memset(r, 0, sizeof(*r));
        r->uid = get_u16(p);
        r->due = get_u16(p + 2);
        r->mastery = (uint8_t)(p[4] & STATE_MASTERY_MASK);
        if (p[4] & STATE_SEEN) r->flags |= VOCAB_REC_SEEN;
        if (p[4] & STATE_EVER) r->flags |= VOCAB_REC_EVER_MASTERED;
        r->lapses = p[5];
    }
    return true;
}

size_t vocab_progress_merge(const vocab_bank_t *bank, vocab_record_t *records, bool *assigned,
                            const vocab_record_t *loaded, size_t n, vocab_tick_t tick)
{
    static const uint8_t intervals[VOCAB_MASTERY_MAX + 1u] = VOCAB_INTERVALS_INIT;
    if (!bank || !bank->entries || !records || !assigned || !loaded) return 0;
    size_t applied = 0;
    const uint32_t latest = (uint32_t)tick + 2u * intervals[VOCAB_MASTERY_MAX];
    for (size_t i = 0; i < n; i++) {
        const vocab_record_t *src = &loaded[i];
        vocab_index_t at = VOCAB_INDEX_NONE;
        for (vocab_index_t b = 0; b < bank->count; b++) {
            if (bank->entries[b].uid == src->uid) {
                at = b;
                break;
            }
        }
        if (at == VOCAB_INDEX_NONE || assigned[at]) continue;  // unknown uid / duplicate
        vocab_record_t *dst = &records[at];
        dst->uid = src->uid;
        dst->mastery = src->mastery > VOCAB_MASTERY_MAX ? (uint8_t)VOCAB_MASTERY_MAX : src->mastery;
        dst->due = (uint32_t)src->due > latest ? vocab_tick_add(tick, intervals[dst->mastery])
                                               : src->due;
        dst->flags = (uint8_t)(src->flags & (VOCAB_REC_SEEN | VOCAB_REC_EVER_MASTERED));
        dst->lapses = src->lapses;
        dst->reserved = 0;
        assigned[at] = true;
        applied++;
    }
    return applied;
}

static uint32_t blob_entries(uint32_t bytes)
{
    return 2u + (bytes + 31u) / 32u;
}

vocab_nvs_budget_t vocab_nvs_budget(uint32_t words, uint32_t partition_bytes)
{
    vocab_nvs_budget_t b;
    memset(&b, 0, sizeof(b));
    const uint32_t per = VOCAB_NVS_RECORDS_PER_CHUNK;
    b.chunks = (words + per - 1u) / per;
    const uint32_t full = words / per;
    const uint32_t rest = words % per;
    b.entries = 1u + blob_entries(VOCAB_META_BYTES) +
                full * blob_entries(VOCAB_CHUNK_HEADER_BYTES + per * VOCAB_CHUNK_RECORD_BYTES +
                                    VOCAB_CHUNK_CRC_BYTES);
    if (rest) {
        b.entries += blob_entries(VOCAB_CHUNK_HEADER_BYTES + rest * VOCAB_CHUNK_RECORD_BYTES +
                                  VOCAB_CHUNK_CRC_BYTES);
    }
    const uint32_t pages = partition_bytes / 4096u;
    b.usable = pages > 1u ? (pages - 1u) * 126u : 0u;
    b.percent = b.usable ? (b.entries * 100u + b.usable / 2u) / b.usable : 100u;
    b.too_tight = b.usable == 0 || b.entries * 2u > b.usable;
    return b;
}
