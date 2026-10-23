// main/vocab_clips.c - word clip pack parser and lookup (spec 8.5, amendment A6); see
// vocab_clips.h for the blob format.
//
// Hand-written (no generated data): validates a self-describing blob byte by byte
// (little-endian reads, so any alignment of the embedded data works), then serves
// lookups. Pure: no allocation, no I/O; the host tests link it without any blob.
#include "vocab_clips.h"

#include <string.h>

#include "vocab_adpcm.h"

static vocab_clip_pack_t s_pack;
static vocab_clip_t s_table[VOCAB_CLIPS_MAX];
static size_t s_count;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// CRC-32/IEEE (reflected, polynomial 0xEDB88320), the same as zlib.crc32 in the tool.
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8u; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static void read_entry(const uint8_t *data, uint32_t index, vocab_clip_t *out)
{
    const uint8_t *e = data + VOCAB_CLIPS_HEADER_BYTES + index * VOCAB_CLIPS_ENTRY_BYTES;
    out->uid = rd16(e);
    out->offset = rd32(e + 4);
    out->n_samples = rd32(e + 8);
    out->n_bytes = rd32(e + 12);
}

vocab_clips_status_t vocab_clip_pack_open(vocab_clip_pack_t *pack, const uint8_t *data,
                                          size_t size)
{
    pack->data = NULL;
    pack->size = 0;
    pack->count = 0;
    if (!data) return VOCAB_CLIPS_ERR_ARG;
    if (size < VOCAB_CLIPS_HEADER_BYTES || size % 4u != 0) return VOCAB_CLIPS_ERR_SIZE;
    if (memcmp(data, VOCAB_CLIPS_MAGIC, 4) != 0) return VOCAB_CLIPS_ERR_MAGIC;
    if (rd16(data + 4) != VOCAB_CLIPS_VERSION || rd16(data + 6) != VOCAB_CLIPS_ENTRY_BYTES) {
        return VOCAB_CLIPS_ERR_VERSION;
    }
    if ((uint64_t)rd32(data + 12) != (uint64_t)size) return VOCAB_CLIPS_ERR_SIZE;
    const uint32_t count = rd32(data + 8);
    if (count > VOCAB_CLIPS_MAX) return VOCAB_CLIPS_ERR_COUNT;
    const uint32_t index_end = VOCAB_CLIPS_HEADER_BYTES + count * VOCAB_CLIPS_ENTRY_BYTES;
    if ((uint64_t)index_end > (uint64_t)size) return VOCAB_CLIPS_ERR_COUNT;
    uint32_t crc = crc32_update(0xFFFFFFFFu, data, 16u);
    crc = crc32_update(crc, data + VOCAB_CLIPS_HEADER_BYTES, (size_t)count * VOCAB_CLIPS_ENTRY_BYTES);
    if ((crc ^ 0xFFFFFFFFu) != rd32(data + 16)) return VOCAB_CLIPS_ERR_CRC;
    if (rd32(data + 20) != 0) return VOCAB_CLIPS_ERR_INDEX;

    uint32_t prev_uid = 0;
    uint64_t prev_end = index_end;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = data + VOCAB_CLIPS_HEADER_BYTES + i * VOCAB_CLIPS_ENTRY_BYTES;
        vocab_clip_t c;
        read_entry(data, i, &c);
        const uint64_t expect_bytes = VOCAB_ADPCM_HEADER_BYTES + ((uint64_t)c.n_samples + 1u) / 2u;
        if (c.uid <= prev_uid || rd16(e + 2) != 0 || c.offset % 4u != 0 || c.offset < prev_end ||
            c.n_samples == 0 || (uint64_t)c.n_bytes != expect_bytes ||
            (uint64_t)c.offset + c.n_bytes > (uint64_t)size) {
            return VOCAB_CLIPS_ERR_INDEX;
        }
        if (data[c.offset + 2u] > VOCAB_ADPCM_INDEX_MAX || data[c.offset + 3u] != 0) {
            return VOCAB_CLIPS_ERR_CLIP;
        }
        prev_uid = c.uid;
        prev_end = (uint64_t)c.offset + c.n_bytes;
    }
    pack->data = data;
    pack->size = (uint32_t)size;
    pack->count = count;
    return VOCAB_CLIPS_OK;
}

bool vocab_clip_pack_entry(const vocab_clip_pack_t *pack, size_t index, vocab_clip_t *out)
{
    if (!pack->data || index >= pack->count) return false;
    read_entry(pack->data, (uint32_t)index, out);
    return true;
}

bool vocab_clip_pack_find(const vocab_clip_pack_t *pack, vocab_uid_t uid, vocab_clip_t *out)
{
    size_t lo = 0;
    size_t hi = pack->data ? pack->count : 0;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const vocab_uid_t at =
            rd16(pack->data + VOCAB_CLIPS_HEADER_BYTES + mid * VOCAB_CLIPS_ENTRY_BYTES);
        if (at == uid) return vocab_clip_pack_entry(pack, mid, out);
        if (at < uid) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

const uint8_t *vocab_clip_pack_bytes(const vocab_clip_pack_t *pack, const vocab_clip_t *clip)
{
    if (!pack->data || !clip || clip->n_bytes == 0 ||
        (uint64_t)clip->offset + clip->n_bytes > pack->size) {
        return NULL;
    }
    return pack->data + clip->offset;
}

const char *vocab_clips_status_name(vocab_clips_status_t status)
{
    switch (status) {
    case VOCAB_CLIPS_OK: return "ok";
    case VOCAB_CLIPS_ERR_ARG: return "no data";
    case VOCAB_CLIPS_ERR_SIZE: return "bad size";
    case VOCAB_CLIPS_ERR_MAGIC: return "bad magic";
    case VOCAB_CLIPS_ERR_VERSION: return "unknown version";
    case VOCAB_CLIPS_ERR_COUNT: return "bad clip count";
    case VOCAB_CLIPS_ERR_CRC: return "index checksum mismatch";
    case VOCAB_CLIPS_ERR_INDEX: return "bad index entry";
    case VOCAB_CLIPS_ERR_CLIP: return "bad clip header";
    default: return "unknown";
    }
}

vocab_clips_status_t vocab_clips_install(const uint8_t *data, size_t size)
{
    vocab_clip_pack_t pack;
    const vocab_clips_status_t status = vocab_clip_pack_open(&pack, data, size);
    s_count = 0;
    for (uint32_t i = 0; i < pack.count; i++) (void)vocab_clip_pack_entry(&pack, i, &s_table[i]);
    s_pack = pack;
    s_count = pack.count;
    return status;
}

size_t vocab_clips_count(void)
{
    return s_count;
}

uint32_t vocab_clips_blob_bytes(void)
{
    return s_pack.size;
}

const vocab_clip_t *vocab_clip_at(size_t index)
{
    return index < s_count ? &s_table[index] : NULL;
}

const vocab_clip_t *vocab_clip_find(vocab_uid_t uid)
{
    size_t lo = 0;
    size_t hi = s_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (s_table[mid].uid == uid) return &s_table[mid];
        if (s_table[mid].uid < uid) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const uint8_t *vocab_clip_bytes(const vocab_clip_t *clip)
{
    return vocab_clip_pack_bytes(&s_pack, clip);
}
