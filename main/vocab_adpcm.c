// main/vocab_adpcm.c - IMA-ADPCM decoder for the embedded word clips; see vocab_adpcm.h.
//
// Bit-exact with the reference decoder adpcm_step() in tools/vocab_audio/build_clips.py
// (tests/test_vocab_adpcm.c checks fixed vectors and every committed clip). Pure integer
// code: no allocation, no libm, no blocking.
#include "vocab_adpcm.h"

static const int16_t STEP_TABLE[VOCAB_ADPCM_INDEX_MAX + 1u] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279,
    307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

bool vocab_adpcm_init(vocab_adpcm_state_t *st, const uint8_t *clip, size_t clip_bytes)
{
    st->predictor = 0;
    st->step_index = 0;
    if (!clip || clip_bytes < VOCAB_ADPCM_HEADER_BYTES || clip[2] > VOCAB_ADPCM_INDEX_MAX) {
        return false;
    }
    const int32_t raw = (int32_t)clip[0] | ((int32_t)clip[1] << 8);
    st->predictor = (int16_t)(raw >= 32768 ? raw - 65536 : raw);
    st->step_index = clip[2];
    return true;
}

int16_t vocab_adpcm_step(vocab_adpcm_state_t *st, uint8_t code)
{
    // A corrupt state never indexes outside the table.
    const uint8_t index = st->step_index > VOCAB_ADPCM_INDEX_MAX ? (uint8_t)VOCAB_ADPCM_INDEX_MAX
                                                                 : st->step_index;
    const int32_t step = STEP_TABLE[index];
    int32_t diff = step >> 3;
    if (code & 4u) diff += step;
    if (code & 2u) diff += step >> 1;
    if (code & 1u) diff += step >> 2;

    int32_t predictor = st->predictor;
    predictor = (code & 8u) ? predictor - diff : predictor + diff;
    if (predictor > INT16_MAX) predictor = INT16_MAX;
    if (predictor < INT16_MIN) predictor = INT16_MIN;

    int32_t next = (int32_t)index + INDEX_TABLE[code & 15u];
    if (next < 0) next = 0;
    if (next > (int32_t)VOCAB_ADPCM_INDEX_MAX) next = (int32_t)VOCAB_ADPCM_INDEX_MAX;

    st->predictor = (int16_t)predictor;
    st->step_index = (uint8_t)next;
    return st->predictor;
}

size_t vocab_adpcm_decode(vocab_adpcm_state_t *st, const uint8_t *codes, size_t first_code,
                          size_t n_samples, int16_t *out)
{
    size_t k = first_code;
    for (size_t i = 0; i < n_samples; i++, k++) {
        const uint8_t byte = codes[k >> 1];
        const uint8_t code = (k & 1u) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
        out[i] = vocab_adpcm_step(st, code);
    }
    return n_samples;
}
