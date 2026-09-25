// components/sonic_link/src/sonic_rs.c
// Reed-Solomon GF(256) codec: systematic encoder plus an errors-and-erasures
// decoder (Berlekamp-Massey initialised with the erasure locator, Chien search,
// Forney algorithm). Parameters are documented in sonic_rs.h.
#include "sonic_rs.h"

#include <stdbool.h>
#include <string.h>

#define GF_POLY 0x11D
#define POLY_CAP (SONIC_RS_MAX_PARITY + 2)

static uint8_t s_exp[512];
static uint8_t s_log[256];
static bool s_ready;

void sonic_rs_init(void)
{
    if (s_ready) return;
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        s_exp[i] = (uint8_t)x;
        s_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= GF_POLY;
    }
    // Duplicate the table so products of two logarithms never need a modulo.
    for (int i = 255; i < 512; i++) s_exp[i] = s_exp[i - 255];
    s_ready = true;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return s_exp[s_log[a] + s_log[b]];
}

// Caller guarantees b != 0.
static inline uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (a == 0) return 0;
    return s_exp[s_log[a] + 255 - s_log[b]];
}

static inline uint8_t gf_alpha_pow(int e)
{
    e %= 255;
    if (e < 0) e += 255;
    return s_exp[e];
}

int sonic_rs_encode(const uint8_t *msg, size_t msg_len, uint8_t nsym, uint8_t *parity)
{
    if (!msg || !parity || nsym == 0 || nsym > SONIC_RS_MAX_PARITY ||
        msg_len == 0 || msg_len + nsym > SONIC_RS_MAX_CODEWORD) {
        return -1;
    }
    sonic_rs_init();

    // g(x) = prod_{i=0}^{nsym-1} (x + alpha^i), highest-degree coefficient first.
    uint8_t gen[SONIC_RS_MAX_PARITY + 1];
    size_t glen = 1;
    gen[0] = 1;
    for (int i = 0; i < nsym; i++) {
        const uint8_t root = s_exp[i];
        gen[glen] = 0;
        for (size_t k = glen; k >= 1; k--) gen[k] ^= gf_mul(root, gen[k - 1]);
        glen++;
    }

    // Remainder of msg(x) * x^nsym divided by g(x); parity[0] is its highest degree.
    memset(parity, 0, nsym);
    for (size_t i = 0; i < msg_len; i++) {
        const uint8_t feedback = msg[i] ^ parity[0];
        memmove(parity, parity + 1, (size_t)nsym - 1);
        parity[nsym - 1] = 0;
        if (feedback == 0) continue;
        for (int j = 0; j < nsym; j++) parity[j] ^= gf_mul(gen[j + 1], feedback);
    }
    return 0;
}

// S_j = r(alpha^j), j = 0..nsym-1. Returns true when every syndrome is zero.
static bool syndromes(const uint8_t *cw, size_t n, uint8_t nsym, uint8_t *synd)
{
    bool clean = true;
    for (int j = 0; j < nsym; j++) {
        const uint8_t a = s_exp[j];
        uint8_t s = 0;
        for (size_t p = 0; p < n; p++) s = gf_mul(s, a) ^ cw[p];
        synd[j] = s;
        if (s) clean = false;
    }
    return clean;
}

int sonic_rs_decode(uint8_t *codeword, size_t n, uint8_t nsym,
                    const uint8_t *erasures, size_t erasure_count)
{
    if (!codeword || nsym == 0 || nsym > SONIC_RS_MAX_PARITY ||
        n <= nsym || n > SONIC_RS_MAX_CODEWORD || erasure_count > nsym ||
        (erasure_count > 0 && !erasures)) {
        return -1;
    }
    for (size_t k = 0; k < erasure_count; k++) {
        if (erasures[k] >= n) return -1;
    }
    sonic_rs_init();

    uint8_t synd[SONIC_RS_MAX_PARITY];
    if (syndromes(codeword, n, nsym, synd)) return 0;

    // Polynomials below are stored lowest-degree coefficient first.
    // Erasure locator Gamma(x) = prod (1 + Z_l x), Z_l = alpha^(n-1-pos).
    uint8_t lambda[POLY_CAP] = { 0 };
    uint8_t prev[POLY_CAP];
    uint8_t tmp[POLY_CAP];
    lambda[0] = 1;
    for (size_t k = 0; k < erasure_count; k++) {
        const uint8_t z = gf_alpha_pow((int)(n - 1 - erasures[k]));
        for (size_t i = k + 1; i >= 1; i--) lambda[i] ^= gf_mul(z, lambda[i - 1]);
    }

    // Berlekamp-Massey initialised with Gamma (Blahut's erasure form): the first
    // erasure_count syndromes are already explained by the known positions.
    memcpy(prev, lambda, sizeof(prev));
    const int f = (int)erasure_count;
    int len = f;
    int shift = 1;
    uint8_t last_d = 1;
    for (int k = f; k < nsym; k++) {
        uint8_t d = 0;
        for (int i = 0; i <= k && i < POLY_CAP; i++) d ^= gf_mul(lambda[i], synd[k - i]);
        if (d == 0) {
            shift++;
            continue;
        }
        const uint8_t coef = gf_div(d, last_d);
        const bool grow = 2 * len <= k + f;
        if (grow) memcpy(tmp, lambda, sizeof(tmp));
        for (int i = 0; i + shift < POLY_CAP; i++) {
            if (prev[i]) lambda[i + shift] ^= gf_mul(coef, prev[i]);
        }
        if (grow) {
            len = k + 1 + f - len;
            memcpy(prev, tmp, sizeof(prev));
            last_d = d;
            shift = 1;
        } else {
            shift++;
        }
    }

    int degree = POLY_CAP - 1;
    while (degree > 0 && lambda[degree] == 0) degree--;
    if (degree != len || 2 * (len - f) + f > nsym) return -1;

    // Chien search over the (shortened) codeword positions only.
    uint8_t pos[SONIC_RS_MAX_PARITY];
    int found = 0;
    for (size_t p = 0; p < n; p++) {
        const uint8_t xinv = gf_alpha_pow(-(int)(n - 1 - p));
        uint8_t v = 0;
        for (int i = degree; i >= 0; i--) v = gf_mul(v, xinv) ^ lambda[i];
        if (v == 0) {
            if (found >= degree) return -1;
            pos[found++] = (uint8_t)p;
        }
    }
    if (found != degree) return -1;

    // Error evaluator Omega(x) = S(x) * Lambda(x) mod x^nsym.
    uint8_t omega[SONIC_RS_MAX_PARITY];
    for (int i = 0; i < nsym; i++) {
        uint8_t acc = 0;
        for (int j = 0; j <= i && j <= degree; j++) acc ^= gf_mul(lambda[j], synd[i - j]);
        omega[i] = acc;
    }

    // Forney with first root alpha^0: e = X * Omega(X^-1) / Lambda'(X^-1).
    for (int k = 0; k < found; k++) {
        const int e = (int)(n - 1 - pos[k]);
        const uint8_t x = gf_alpha_pow(e);
        const uint8_t xinv = gf_alpha_pow(-e);
        uint8_t num = 0;
        for (int i = nsym - 1; i >= 0; i--) num = gf_mul(num, xinv) ^ omega[i];
        // Formal derivative in characteristic 2 keeps only odd-degree terms.
        const uint8_t x2 = gf_mul(xinv, xinv);
        uint8_t den = 0;
        for (int i = (degree & 1) ? degree : degree - 1; i >= 1; i -= 2) {
            den = gf_mul(den, x2) ^ lambda[i];
        }
        if (den == 0) return -1;
        codeword[pos[k]] ^= gf_mul(x, gf_div(num, den));
    }

    // Reject miscorrections that do not land on a valid codeword.
    if (!syndromes(codeword, n, nsym, synd)) return -1;
    return found;
}
