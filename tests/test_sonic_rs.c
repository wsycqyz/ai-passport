// Host tests for the SonicLink Reed-Solomon codec and CRC.
#include "sonic_link.h"
#include "sonic_rs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

static uint32_t s_rng = 0x12345678u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t rnd_below(uint32_t n)
{
    return rnd() % n;
}

static void pick_distinct(uint8_t *out, size_t count, size_t n)
{
    uint8_t used[256] = { 0 };
    for (size_t i = 0; i < count; i++) {
        uint8_t p;
        do {
            p = (uint8_t)rnd_below((uint32_t)n);
        } while (used[p]);
        used[p] = 1;
        out[i] = p;
    }
}

static void test_known_vector(void)
{
    // QR-code "hello world" example (0x11D, generator 2, fcr 0), 10 parity bytes.
    const uint8_t msg[] = {
        0x40, 0xd2, 0x75, 0x47, 0x76, 0x17, 0x32, 0x06,
        0x27, 0x26, 0x96, 0xc6, 0xc6, 0x96, 0x70, 0xec,
    };
    const uint8_t expected[] = { 0xbc, 0x2a, 0x90, 0x13, 0x6b, 0xaf, 0xef, 0xfd, 0x4b, 0xe0 };
    uint8_t parity[10];
    CHECK(sonic_rs_encode(msg, sizeof(msg), 10, parity) == 0);
    CHECK(memcmp(parity, expected, sizeof(expected)) == 0);

    uint8_t cw[26];
    memcpy(cw, msg, sizeof(msg));
    memcpy(cw + sizeof(msg), parity, sizeof(parity));
    CHECK(sonic_rs_decode(cw, sizeof(cw), 10, NULL, 0) == 0);
    cw[0] ^= 0xFF;
    cw[7] ^= 0x01;
    cw[25] ^= 0x80;
    CHECK(sonic_rs_decode(cw, sizeof(cw), 10, NULL, 0) == 3);
    CHECK(memcmp(cw, msg, sizeof(msg)) == 0);
    CHECK(memcmp(cw + sizeof(msg), expected, sizeof(expected)) == 0);
}

static void test_random_within_capacity(void)
{
    uint8_t original[255];
    uint8_t cw[255];
    uint8_t positions[255];
    for (int trial = 0; trial < 4000; trial++) {
        const uint8_t nsym = (uint8_t)(2 + 2 * rnd_below(SONIC_RS_MAX_PARITY / 2));
        const size_t n = nsym + 1 + rnd_below(255u - nsym);
        const size_t k = n - nsym;
        for (size_t i = 0; i < k; i++) original[i] = (uint8_t)rnd();
        CHECK(sonic_rs_encode(original, k, nsym, original + k) == 0);

        // Split capacity randomly between erasures (1 unit) and errors (2 units).
        const size_t erasures = rnd_below(nsym + 1u);
        const size_t errors = rnd_below((uint32_t)((nsym - erasures) / 2 + 1));
        pick_distinct(positions, erasures + errors, n);
        memcpy(cw, original, n);
        for (size_t i = 0; i < erasures + errors; i++) {
            // Errors always change the byte; erased bytes may or may not.
            uint8_t delta = (uint8_t)rnd();
            if (i >= erasures && delta == 0) delta = 1;
            cw[positions[i]] ^= delta;
        }
        const int fixed = sonic_rs_decode(cw, n, nsym, positions, erasures);
        CHECK(fixed >= 0);
        CHECK(memcmp(cw, original, n) == 0);
        if (fixed < 0 || memcmp(cw, original, n) != 0) {
            fprintf(stderr, "  trial %d n=%zu nsym=%u erasures=%zu errors=%zu\n",
                    trial, n, nsym, erasures, errors);
            return;
        }
    }
}

static void test_beyond_capacity(void)
{
    uint8_t original[255];
    uint8_t cw[255];
    uint8_t positions[255];
    uint8_t check[SONIC_RS_MAX_PARITY];
    int rejected = 0;
    int miscorrected = 0;
    for (int trial = 0; trial < 2000; trial++) {
        const uint8_t nsym = (uint8_t)(4 + 2 * rnd_below(15));
        const size_t n = nsym + 8 + rnd_below(120);
        const size_t k = n - nsym;
        for (size_t i = 0; i < k; i++) original[i] = (uint8_t)rnd();
        sonic_rs_encode(original, k, nsym, original + k);
        const size_t errors = nsym / 2 + 1 + rnd_below(3);
        pick_distinct(positions, errors, n);
        memcpy(cw, original, n);
        for (size_t i = 0; i < errors; i++) cw[positions[i]] ^= (uint8_t)(1 + rnd_below(255));
        const int fixed = sonic_rs_decode(cw, n, nsym, NULL, 0);
        if (fixed < 0) {
            rejected++;
            continue;
        }
        // A reported success must at least be a valid codeword.
        sonic_rs_encode(cw, k, nsym, check);
        CHECK(memcmp(check, cw + k, nsym) == 0);
        CHECK(memcmp(cw, original, n) != 0);
        miscorrected++;
    }
    printf("  beyond capacity: %d rejected, %d miscorrected (CRC catches these)\n",
           rejected, miscorrected);
    CHECK(rejected > miscorrected * 5);
}

static void test_invalid_arguments(void)
{
    uint8_t buf[255] = { 0 };
    uint8_t parity[65];
    uint8_t erasure = 3;
    CHECK(sonic_rs_encode(buf, 10, 0, parity) == -1);
    CHECK(sonic_rs_encode(buf, 10, 65, parity) == -1);
    CHECK(sonic_rs_encode(buf, 250, 6, parity) == -1);
    CHECK(sonic_rs_decode(buf, 6, 6, NULL, 0) == -1);          // no data bytes
    CHECK(sonic_rs_decode(buf, 256, 6, NULL, 0) == -1);
    CHECK(sonic_rs_decode(buf, 20, 6, NULL, 1) == -1);         // missing erasure list
    CHECK(sonic_rs_decode(buf, 20, 6, &erasure, 7) == -1);     // more erasures than parity
    erasure = 25;
    CHECK(sonic_rs_decode(buf, 20, 6, &erasure, 1) == -1);     // position out of range
    CHECK(sonic_rs_decode(buf, 20, 6, NULL, 0) == 0);          // all-zero is a codeword
}

static void test_crc(void)
{
    CHECK(sonic_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    CHECK(sonic_crc16(NULL, 0) == 0xFFFF);
}

int main(void)
{
    sonic_rs_init();
    test_known_vector();
    test_random_within_capacity();
    test_beyond_capacity();
    test_invalid_arguments();
    test_crc();
    if (s_failures) {
        fprintf(stderr, "test_sonic_rs: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_sonic_rs: PASS\n");
    return 0;
}
